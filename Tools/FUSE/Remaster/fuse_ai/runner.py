"""``run_recipe()``: the one path from a recipe to a result (plan §3.3, §0.1.5, §3.5).

Order of operations, identical in every mode::

    1. validate + resolve inputs (read ``path`` inputs, fill their sha256; verify given hashes)
    2. pick the provider: the recipe's, else the §3.1 default for the task; ``mock`` mode routes to ``mock``
    3. canonical recipe hash (§3.3)
    4. W0.8 cloud opt-in guard (policy.check_request)       <- always, before the cache and the provider
    5. replay: cache hit or ReplayMiss
       record: cache hit (unless force) or [VRAM check -> budget check -> audit upload -> provider.run ->
               cache store -> ledger]
       mock:   mock provider, no cache
    6. return the result and the recipe updated with ``output`` (hashes) and ``cost``

There is deliberately no flag that skips step 4.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

from . import policy, providers
from .audit import AuditLog
from .budget import Budget, Clock, utc_now
from .cache import DEFAULT_CACHE_DIR, Mode, ResultCache
from .errors import RecipeError, ReplayMiss, VramExceeded
from .providers import Request, Result
from .recipe import Recipe

VRAM_BUDGET_MB = 22 * 1024  # §3.5: 22 GB for models and activations on the 3090


@dataclass
class RunOutcome:
    recipe_hash: str
    mode: Mode
    provider_id: str          # the provider that produced the result (mock in mock mode)
    recipe_provider_id: str   # the provider the recipe names (or the task default)
    result: Result
    recipe: Recipe            # updated copy: output + cost filled in
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
        }


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

    def run(self, recipe: Recipe, base_dir: Optional[str | Path] = None) -> RunOutcome:
        recipe = Recipe.from_json(recipe.to_json())  # private copy; the caller's object is never mutated
        input_data = _resolve_inputs(recipe, Path(base_dir) if base_dir else None, self.cache)

        named = recipe.provider or providers.default_provider_for(recipe.task)
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

        audit_entries: List[Dict[str, Any]] = []
        if self.mode is Mode.REPLAY:
            result = self.cache.lookup(recipe_hash)
            if result is None:
                raise ReplayMiss(f"replay miss for recipe {recipe.id!r} (hash {recipe_hash}) in {self.cache.root}; "
                                 "record it on the authoring machine with --mode record")
        elif self.mode is Mode.MOCK:
            result = provider.run(req)
        else:
            cached = None if self.force else self.cache.lookup(recipe_hash)
            if cached is not None:
                result = cached
            else:
                vram = provider.vram_mb(req)
                if vram > VRAM_BUDGET_MB:
                    raise VramExceeded(f"{effective} needs ~{vram} MB VRAM for {recipe.task.value}, above the "
                                       f"{VRAM_BUDGET_MB} MB budget of the 3090 profile; pick a quantised model "
                                       "or CPU offload (§3.5)")
                est = provider.estimate(req)
                self.budget.check(info, est)
                if decision.cloud:
                    audit_entries.append(self._audit_upload(recipe, recipe_hash, info, decision, req))
                result = provider.run(req)
                self.budget.record(info, result.cost)
                self.cache.store(recipe_hash, recipe.hash_payload(), result)

        if not result.outputs:
            raise RecipeError(f"provider {effective!r} returned no outputs for recipe {recipe.id!r}")
        primary_sha = hashlib.sha256(result.primary.data).hexdigest()
        prev = recipe.output or {}
        matches = (prev.get("sha256") == primary_sha) if prev.get("sha256") else None
        out = dict(prev)
        out["sha256"] = primary_sha
        out["files"] = [{"name": o.name, "media_type": o.media_type, "sha256": hashlib.sha256(o.data).hexdigest()}
                        for o in result.outputs]
        recipe.output = out
        recipe.cost = result.cost.to_json()
        return RunOutcome(recipe_hash=recipe_hash, mode=self.mode, provider_id=result.provider_id or effective,
                          recipe_provider_id=named, result=result, recipe=recipe, decision=decision,
                          output_matches=matches, audit_entries=audit_entries)

    def _audit_upload(self, recipe: Recipe, recipe_hash: str, info: providers.ProviderInfo,
                      decision: policy.Decision, req: Request) -> Dict[str, Any]:
        conf = self.project.confirmed or {}
        return self.audit.append(
            "upload",
            recipe_hash=recipe_hash, recipe_id=recipe.id, provider=info.id,
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
