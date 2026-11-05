"""``run_recipe()`` / ``Runner.run_many()``: the one path from recipes to results (plan §3.3, §0.1.5, §3.5).

Order of operations, identical in every mode::

    1. validate + resolve inputs (read ``path`` inputs, fill their sha256; verify given hashes)
    2. pick the provider: the recipe's, else routing.route() (§3.1 default, or the local fallback for
       originals without the cloud opt-in, W4.6); ``mock`` mode routes to ``mock``. A routed recipe gets the
       chosen provider (and its default pinned model) written in before hashing, so the cache key names it.
    3. canonical recipe hash (§3.3)
    4. W0.8 cloud opt-in guard (policy.check_request)       <- always, before the cache and the provider
    5. replay: cache hit or ReplayMiss
       record: cache hit (unless force) or [VRAM check -> budget check -> audit upload -> provider.run ->
               cache store -> ledger]
       mock:   mock provider, no cache
    6. prompt outputs (``params.prompt``) are parsed and schema-checked - live *and* replayed (W4.2)
    7. return the result and the recipe updated with ``output`` (hashes, distribution, watermark) and ``cost``

``run_many()`` (the CLI path, W4.5) runs steps 1-4 for *every* recipe first, then a budget preflight over all
cache misses (:meth:`Budget.check_plan`), and only then calls any provider: a batch whose total estimate
breaches a cap is refused before the first call. Recipes with ``params.batch: true`` whose provider offers
``run_batch`` (gemini) are submitted as one batch job per (provider, model).

There is deliberately no flag that skips step 4.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

from . import policy, prompts, providers, routing
from .audit import AuditLog
from .budget import Budget, Clock, utc_now
from .cache import DEFAULT_CACHE_DIR, Mode, ResultCache
from .errors import RecipeError, ReplayMiss, VramExceeded
from .providers import Cost, Request, Result
from .recipe import Recipe

VRAM_BUDGET_MB = 22 * 1024  # §3.5: 22 GB for models and activations on the 3090


@dataclass
class RunOutcome:
    recipe_hash: str
    mode: Mode
    provider_id: str          # the provider that produced the result (mock in mock mode)
    recipe_provider_id: str   # the provider the recipe names (or the routed one)
    result: Result
    recipe: Recipe            # updated copy: provider/model resolved, output + cost filled in
    decision: policy.Decision
    output_matches: Optional[bool] = None  # vs. the recipe's recorded output.sha256, when it had one
    audit_entries: List[Dict[str, Any]] = field(default_factory=list)

    def summary(self) -> Dict[str, Any]:
        return {
            "id": self.recipe.id,
            "hash": self.recipe_hash,
            "mode": self.mode.value,
            "provider": self.provider_id,
            "recipe_provider": self.recipe_provider_id,
            "from_cache": self.result.from_cache,
            "outputs": [{"name": o.name, "media_type": o.media_type, "sha256": hashlib.sha256(o.data).hexdigest(),
                         "size": len(o.data)} for o in self.result.outputs],
            "cost": self.result.cost.to_json(),
            "output_matches": self.output_matches,
            "uploads_original": self.decision.uploads_original,
            "distribution": (self.recipe.output or {}).get("distribution"),
        }


@dataclass
class Prepared:
    recipe: Recipe
    req: Request
    named: str
    effective: str
    info: providers.ProviderInfo
    provider: Any
    recipe_hash: str
    decision: policy.Decision
    result: Optional[Result] = None
    audit_entries: List[Dict[str, Any]] = field(default_factory=list)


def _resolve_inputs(recipe: Recipe, base_dir: Optional[Path], cache: ResultCache) -> List[Optional[bytes]]:
    data: List[Optional[bytes]] = []
    for n, ref in enumerate(recipe.inputs):
        blob: Optional[bytes] = None
        if ref.path:
            p = Path(ref.path)
            if not p.is_absolute() and base_dir is not None:
                p = base_dir / p
            try:
                blob = p.read_bytes()
            except OSError as e:
                raise RecipeError(f"recipe {recipe.id!r}: cannot read inputs[{n}] {p}: {e}") from None
            sha = hashlib.sha256(blob).hexdigest()
            if ref.sha256 and ref.sha256 != sha:
                raise RecipeError(f"recipe {recipe.id!r}: inputs[{n}] {p} has sha256 {sha}, recipe says {ref.sha256}")
            ref.sha256 = sha
        else:
            for key in (ref.sha256, ref.canonical_sha256):
                if key and cache.has_blob(key):
                    blob = cache.get_blob(key)
                    break
        data.append(blob)
    return data


def distribution_of(recipe: Recipe, result: Result) -> str:
    """§2.2: computed, never set by hand. Originals anywhere in the inputs make the output recipe-only."""
    if result.meta.get("distribution") == "never":
        return "never"
    if any(i.is_original_content for i in recipe.inputs):
        return "recipe-only"
    return "ship"


class Runner:
    """Holds the per-run state (cache, project policy, budget, audit log) for one or more recipes."""

    def __init__(self, mode: Mode | str = Mode.REPLAY, *, cache_dir: Optional[str | Path] = None,
                 project: Optional[policy.ProjectPolicy] = None, budget: Optional[Budget] = None,
                 clock: Clock = utc_now, force: bool = False):
        self.mode = Mode.parse(mode) if isinstance(mode, str) else mode
        self.cache = ResultCache(cache_dir if cache_dir is not None else DEFAULT_CACHE_DIR)
        self.project = project or policy.ProjectPolicy()
        self.clock = clock
        self.budget = budget or Budget(clock=clock)
        self.force = force
        self.audit = AuditLog(self.project.audit_path(self.cache.root), clock=clock)

    # ---- steps 1-4 ----------------------------------------------------------------------------------------------

    def prepare(self, recipe: Recipe, base_dir: Optional[str | Path] = None) -> Prepared:
        recipe = Recipe.from_json(recipe.to_json())  # private copy; the caller's object is never mutated
        input_data = _resolve_inputs(recipe, Path(base_dir) if base_dir else None, self.cache)

        named = recipe.provider or routing.route(recipe.task, recipe.inputs, self.project)
        if not recipe.provider:
            recipe.provider = named
            if not recipe.model:
                recipe.model = routing.default_model(named, recipe.task) or {}
        named_info = providers.info(named)
        if recipe.task not in named_info.tasks:
            raise RecipeError(f"recipe {recipe.id!r}: provider {named!r} does not support task {recipe.task.value}")
        effective = "mock" if self.mode is Mode.MOCK else named
        info = providers.info(effective)
        provider = providers.get(effective)

        recipe_hash = recipe.canonical_hash()
        req = Request(task=recipe.task, provider_id=effective, model=dict(recipe.model), params=dict(recipe.params),
                      seed=recipe.seed, inputs=recipe.inputs, input_data=input_data,
                      purpose=recipe.purpose or recipe.task.value, recipe_hash=recipe_hash)

        # ---- W0.8: the guard, before the cache and before any provider call -----------------------------------
        decision = policy.check_request(req, info, self.project)
        return Prepared(recipe, req, named, effective, info, provider, recipe_hash, decision)

    # ---- step 5 -------------------------------------------------------------------------------------------------

    def _lookup(self, p: Prepared) -> bool:
        if self.mode is Mode.REPLAY:
            p.result = self.cache.lookup(p.recipe_hash)
            if p.result is None:
                raise ReplayMiss(f"replay miss for recipe {p.recipe.id!r} (hash {p.recipe_hash}) in {self.cache.root}; "
                                 "record it on the authoring machine with --mode record")
            return True
        if self.mode is Mode.MOCK:
            return False
        if not self.force:
            p.result = self.cache.lookup(p.recipe_hash)
        return p.result is not None

    def _vram(self, p: Prepared) -> None:
        vram = p.provider.vram_mb(p.req)
        if vram > VRAM_BUDGET_MB:
            raise VramExceeded(f"{p.effective} needs ~{vram} MB VRAM for {p.recipe.task.value}, above the "
                               f"{VRAM_BUDGET_MB} MB budget of the 3090 profile; pick a quantised model "
                               "or CPU offload (§3.5)")

    def _call(self, p: Prepared) -> None:
        if self.mode is Mode.MOCK:
            p.result = p.provider.run(p.req)
            return
        self._vram(p)
        self.budget.check(p.info, p.provider.estimate(p.req))
        if p.decision.cloud:
            p.audit_entries.append(self._audit_upload(p))
        p.result = p.provider.run(p.req)
        self.budget.record(p.info, p.result.cost)
        self.cache.store(p.recipe_hash, p.recipe.hash_payload(), p.result)

    def _call_batch(self, group: List[Prepared]) -> None:
        head = group[0]
        est = Cost()
        for p in group:
            self._vram(p)
            e = p.provider.estimate(p.req)
            est.usd += e.usd
            est.seconds += e.seconds
            est.gpu = est.gpu or e.gpu
        self.budget.check(head.info, est)
        for p in group:
            if p.decision.cloud:
                p.audit_entries.append(self._audit_upload(p))
        results = head.provider.run_batch([p.req for p in group])
        for p, r in zip(group, results):
            p.result = r
            self.budget.record(p.info, r.cost)
            self.cache.store(p.recipe_hash, p.recipe.hash_payload(), r)

    # ---- steps 6-7 ----------------------------------------------------------------------------------------------

    def finish(self, p: Prepared) -> RunOutcome:
        result, recipe = p.result, p.recipe
        assert result is not None
        if not result.outputs:
            raise RecipeError(f"provider {p.effective!r} returned no outputs for recipe {recipe.id!r}")
        if not result.meta.get("mock"):
            pr = prompts.for_params(recipe.params, recipe.task)
            if pr is not None and pr.output_schema is not None:
                prompts.parse_output(pr, result.primary.data)  # SchemaViolation on a bad (replayed) answer
        primary_sha = hashlib.sha256(result.primary.data).hexdigest()
        prev = recipe.output or {}
        matches = (prev.get("sha256") == primary_sha) if prev.get("sha256") else None
        out = dict(prev)
        out["sha256"] = primary_sha
        out["files"] = [{"name": o.name, "media_type": o.media_type, "sha256": hashlib.sha256(o.data).hexdigest()}
                        for o in result.outputs]
        out["distribution"] = distribution_of(recipe, result)
        if result.meta.get("watermark"):
            out["watermark"] = result.meta["watermark"]
        recipe.output = out
        recipe.cost = result.cost.to_json()
        return RunOutcome(recipe_hash=p.recipe_hash, mode=self.mode, provider_id=result.provider_id or p.effective,
                          recipe_provider_id=p.named, result=result, recipe=recipe, decision=p.decision,
                          output_matches=matches, audit_entries=p.audit_entries)

    # ---- entry points -------------------------------------------------------------------------------------------

    def run(self, recipe: Recipe, base_dir: Optional[str | Path] = None) -> RunOutcome:
        p = self.prepare(recipe, base_dir)
        if not self._lookup(p):
            self._call(p)
        return self.finish(p)

    def plan(self, items: Sequence[Tuple[Recipe, Optional[str | Path]]]) -> List[Prepared]:
        """Steps 1-4 plus the cache lookup for every recipe; raises before any provider call."""
        prepared = [self.prepare(r, b) for r, b in items]
        for p in prepared:
            self._lookup(p)
        return prepared

    def preflight(self, prepared: Sequence[Prepared]) -> List[Tuple[Prepared, Cost]]:
        """Budget + VRAM check over every cache miss together (W4.5); raises BudgetExceeded before any call."""
        misses = [p for p in prepared if p.result is None]
        if self.mode is not Mode.RECORD:
            return []
        plan = []
        for p in misses:
            self._vram(p)
            plan.append((p, p.provider.estimate(p.req)))  # gemini's estimate applies the batch discount itself
        self.budget.check_plan([(p.info, est) for p, est in plan])
        return plan

    def run_many(self, items: Sequence[Tuple[Recipe, Optional[str | Path]]]) -> List[RunOutcome]:
        prepared = self.plan(items)
        self.preflight(prepared)
        batches: Dict[Tuple[str, str], List[Prepared]] = {}
        for p in prepared:
            if p.result is not None:
                continue
            if self.mode is Mode.RECORD and p.req.params.get("batch") and hasattr(p.provider, "run_batch"):
                batches.setdefault((p.effective, p.req.model_id), []).append(p)
            else:
                self._call(p)
        for group in batches.values():
            self._call_batch(group)
        return [self.finish(p) for p in prepared]

    def _audit_upload(self, p: Prepared) -> Dict[str, Any]:
        conf = self.project.confirmed or {}
        recipe, info, decision, req = p.recipe, p.info, p.decision, p.req
        return self.audit.append(
            "upload",
            recipe_hash=p.recipe_hash, recipe_id=recipe.id, provider=info.id,
            model=req.model_id or None, task=recipe.task.value, purpose=req.purpose,
            inputs=[{"hash": i.content_key, "origin": i.origin, "derived_from_original": bool(i.derived_from_original)}
                    for i in recipe.inputs],
            originals=decision.uploads_original, opt_in=self.project.cloud_upload,
            confirmed_by=conf.get("by"), tier=decision.tier, game_id=self.project.game_id or None)


def run_recipe(recipe: Recipe, mode: Mode | str = Mode.REPLAY, *, cache_dir: Optional[str | Path] = None,
               project: Optional[policy.ProjectPolicy] = None, budget: Optional[Budget] = None,
               base_dir: Optional[str | Path] = None, clock: Clock = utc_now, force: bool = False) -> RunOutcome:
    """Run one recipe (see the module docstring for the order of operations)."""
    return Runner(mode, cache_dir=cache_dir, project=project, budget=budget, clock=clock, force=force).run(
        recipe, base_dir=base_dir)
