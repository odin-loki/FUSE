"""The ``fuse.ai-recipe/1`` schema and its canonical hash (plan §3.3).

A recipe is a committed JSON file describing one AI call::

    {"schema": "fuse.ai-recipe/1", "id": "tex-upscale/6f1c9a…", "task": "UPSCALE", "provider": "local-onnx",
     "model": {"id": …, "sha256": …, "licence": …, "training_data": …},
     "params": {…}, "seed": 7, "purpose": "…",
     "inputs": [{"oaid": …, "canonical_sha256": …, "origin": "original", "derived_from_original": false}],
     "output": {"sha256": …, "tolerance": {…}}, "cost": {…}, "review": {"state": "approved", "by": …, "date": …}}

**Canonical hash** = sha256 of the canonical JSON (keys sorted, no whitespace, UTF-8) of the normalised
recipe with ``output`` and ``review`` removed (§3.3). We also drop ``cost`` (it is the *measured* spend of
a run, written back after recording, like ``output``) and each input's ``path`` (a machine-local locator;
the content hash stands in for it). Everything else - task, provider, model pin, params, seed, purpose,
input content hashes *and provenance* - is part of the key.

``inputs[].origin`` / ``derived_from_original`` carry provenance for the W0.8 cloud guard (§0.1.5). A
missing ``origin`` normalises to ``original`` (fail closed); ``origin: derived`` must state
``derived_from_original`` explicitly.
"""

from __future__ import annotations

import copy
import hashlib
import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

from . import RECIPE_SCHEMA
from .errors import RecipeError
from .providers import ORIGINS, InputRef, Task

_HEX64 = re.compile(r"^[0-9a-f]{64}$")
REVIEW_STATES = ("draft", "approved", "rejected")
TOP_LEVEL_KEYS = {"schema", "id", "task", "provider", "model", "params", "seed", "purpose", "inputs", "output",
                  "cost", "review"}
INPUT_KEYS = {"oaid", "canonical_sha256", "sha256", "path", "name", "media_type", "origin", "derived_from_original"}
HASH_EXCLUDED_KEYS = ("output", "review", "cost")


def canonical_json(obj: Any) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False, allow_nan=False).encode("utf-8")


@dataclass
class Recipe:
    id: str
    task: Task
    provider: Optional[str] = None
    model: Dict[str, Any] = field(default_factory=dict)
    params: Dict[str, Any] = field(default_factory=dict)
    seed: int = 0
    purpose: str = ""
    inputs: List[InputRef] = field(default_factory=list)
    output: Optional[Dict[str, Any]] = None
    cost: Optional[Dict[str, Any]] = None
    review: Optional[Dict[str, Any]] = None
    extensions: Dict[str, Any] = field(default_factory=dict)  # "x-…" keys, preserved and hashed
    schema: str = RECIPE_SCHEMA

    # ---- (de)serialisation --------------------------------------------------------------------------------------

    @classmethod
    def from_json(cls, d: Any) -> "Recipe":
        validate(d)
        inputs = []
        for item in d.get("inputs", []):
            inputs.append(InputRef(
                oaid=item.get("oaid"), canonical_sha256=item.get("canonical_sha256"), sha256=item.get("sha256"),
                path=item.get("path"), name=item.get("name"), media_type=item.get("media_type"),
                origin=item.get("origin", "original"),
                derived_from_original=bool(item.get("derived_from_original", False))))
        return cls(id=d["id"], task=Task.parse(d["task"]), provider=d.get("provider"),
                   model=copy.deepcopy(d.get("model", {})), params=copy.deepcopy(d.get("params", {})),
                   seed=int(d.get("seed", 0)), purpose=d.get("purpose", ""), inputs=inputs,
                   output=copy.deepcopy(d.get("output")), cost=copy.deepcopy(d.get("cost")),
                   review=copy.deepcopy(d.get("review")),
                   extensions={k: copy.deepcopy(v) for k, v in d.items() if k.startswith("x-")})

    def to_json(self) -> Dict[str, Any]:
        d: Dict[str, Any] = {"schema": self.schema, "id": self.id, "task": self.task.value,
                             "params": copy.deepcopy(self.params), "seed": self.seed}
        if self.provider:
            d["provider"] = self.provider
        if self.model:
            d["model"] = copy.deepcopy(self.model)
        if self.purpose:
            d["purpose"] = self.purpose
        d["inputs"] = [_input_json(i) for i in self.inputs]
        for key in ("output", "cost", "review"):
            val = getattr(self, key)
            if val is not None:
                d[key] = copy.deepcopy(val)
        d.update(copy.deepcopy(self.extensions))
        return d

    # ---- hashing ------------------------------------------------------------------------------------------------

    def hash_payload(self) -> Dict[str, Any]:
        d = self.to_json()
        for key in HASH_EXCLUDED_KEYS:
            d.pop(key, None)
        for i, item in enumerate(d["inputs"]):
            if not (item.get("canonical_sha256") or item.get("sha256")):
                raise RecipeError(f"recipe {self.id!r}: input {i} has no content hash yet (resolve it first)")
            item.pop("path", None)
        return d

    def canonical_hash(self) -> str:
        return hashlib.sha256(canonical_json(self.hash_payload())).hexdigest()


def _input_json(i: InputRef) -> Dict[str, Any]:
    d: Dict[str, Any] = {"origin": i.origin}
    for key in ("oaid", "canonical_sha256", "sha256", "path", "name", "media_type"):
        val = getattr(i, key)
        if val is not None:
            d[key] = val
    if i.origin == "derived" or i.derived_from_original:
        d["derived_from_original"] = bool(i.derived_from_original)
    return d


# ---- validation -------------------------------------------------------------------------------------------------


def validate(d: Any) -> None:
    """Raise RecipeError with every problem found (not only the first)."""
    errs: List[str] = []
    if not isinstance(d, dict):
        raise RecipeError("recipe must be a JSON object")
    if d.get("schema") != RECIPE_SCHEMA:
        errs.append(f"schema must be {RECIPE_SCHEMA!r}, got {d.get('schema')!r}")
    for key in d:
        if key not in TOP_LEVEL_KEYS and not key.startswith("x-"):
            errs.append(f"unknown key {key!r} (extensions must start with 'x-')")
    if not isinstance(d.get("id"), str) or not d.get("id"):
        errs.append("id must be a non-empty string")
    task = d.get("task")
    if not isinstance(task, str) or task not in Task.__members__:
        errs.append(f"task must be one of {', '.join(Task.__members__)}; got {task!r}")
    if "provider" in d and (not isinstance(d["provider"], str) or not d["provider"]):
        errs.append("provider must be a non-empty string when present")
    model = d.get("model", {})
    if not isinstance(model, dict):
        errs.append("model must be an object")
    else:
        if "id" in model and not isinstance(model["id"], str):
            errs.append("model.id must be a string")
        if "sha256" in model and not _is_hex64(model["sha256"]):
            errs.append("model.sha256 must be 64 lowercase hex digits")
    if not isinstance(d.get("params", {}), dict):
        errs.append("params must be an object")
    seed = d.get("seed", 0)
    if not isinstance(seed, int) or isinstance(seed, bool):
        errs.append("seed must be an integer")
    if "purpose" in d and not isinstance(d["purpose"], str):
        errs.append("purpose must be a string")
    inputs = d.get("inputs", [])
    if not isinstance(inputs, list):
        errs.append("inputs must be an array")
        inputs = []
    for n, item in enumerate(inputs):
        where = f"inputs[{n}]"
        if not isinstance(item, dict):
            errs.append(f"{where} must be an object")
            continue
        for key in item:
            if key not in INPUT_KEYS:
                errs.append(f"{where}: unknown key {key!r}")
        for key in ("canonical_sha256", "sha256"):
            if key in item and not _is_hex64(item[key]):
                errs.append(f"{where}.{key} must be 64 lowercase hex digits")
        for key in ("oaid", "path", "name", "media_type"):
            if key in item and not isinstance(item[key], str):
                errs.append(f"{where}.{key} must be a string")
        if not any(item.get(k) for k in ("canonical_sha256", "sha256", "path")):
            errs.append(f"{where} needs canonical_sha256, sha256 or path")
        origin = item.get("origin", "original")
        if origin not in ORIGINS:
            errs.append(f"{where}.origin must be one of {', '.join(ORIGINS)}; got {origin!r}")
        dfo = item.get("derived_from_original")
        if dfo is not None and not isinstance(dfo, bool):
            errs.append(f"{where}.derived_from_original must be a boolean")
        if origin == "derived" and dfo is None:
            errs.append(f"{where}: origin 'derived' must state derived_from_original (true/false)")
    output = d.get("output")
    if output is not None:
        if not isinstance(output, dict):
            errs.append("output must be an object")
        elif "sha256" in output and not _is_hex64(output["sha256"]):
            errs.append("output.sha256 must be 64 lowercase hex digits")
    if d.get("cost") is not None and not isinstance(d.get("cost"), dict):
        errs.append("cost must be an object")
    review = d.get("review")
    if review is not None:
        if not isinstance(review, dict):
            errs.append("review must be an object")
        elif review.get("state") not in REVIEW_STATES:
            errs.append(f"review.state must be one of {', '.join(REVIEW_STATES)}")
    if errs:
        raise RecipeError("invalid recipe: " + "; ".join(errs))


def _is_hex64(v: Any) -> bool:
    return isinstance(v, str) and bool(_HEX64.match(v))


# ---- file helpers -----------------------------------------------------------------------------------------------


def loads(text: str) -> Recipe:
    try:
        d = json.loads(text)
    except json.JSONDecodeError as e:
        raise RecipeError(f"recipe is not valid JSON: {e}") from None
    return Recipe.from_json(d)


def load(path: str | Path) -> Recipe:
    p = Path(path)
    try:
        text = p.read_text(encoding="utf-8")
    except OSError as e:
        raise RecipeError(f"cannot read recipe {p}: {e}") from None
    return loads(text)


def dumps(recipe: Recipe) -> str:
    return json.dumps(recipe.to_json(), sort_keys=True, indent=2, ensure_ascii=False) + "\n"


def dump(recipe: Recipe, path: str | Path) -> None:
    Path(path).write_text(dumps(recipe), encoding="utf-8", newline="\n")


def canonical_hash(d: Dict[str, Any] | Recipe) -> str:
    """Canonical recipe hash of a recipe object or a raw JSON dict."""
    r = d if isinstance(d, Recipe) else Recipe.from_json(d)
    return r.canonical_hash()
