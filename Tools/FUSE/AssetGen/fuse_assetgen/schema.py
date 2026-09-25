"""Recipe schema (version 1): loading and validation.

A recipe is a JSON object (or YAML mapping when PyYAML is importable):

    {
      "schema": 1,                              # required, == 1
      "id": "tex/test/noise_a",                 # required, <class>/<biome|style>/<name>[_<variant>]
      "generator": "noise_texture",             # required, a registered generator name
      "seed": 1234,                             # required, int in [0, 2^63-1]
      "params": {...},                          # generator parameters, validated by its PARAMS schema
      "licence": "LicenseRef-FUSE-Generated",   # optional, default shown; NC/ND identifiers rejected
      "derived_from": ["mat/rock/granite_01"],  # optional, licence-record ids of the inputs
      "description": "free text"                # optional
    }

Unknown top-level keys are errors (typos must not silently change nothing). Parameter schemas use a
small JSON-Schema subset: type (int/number/bool/string/array/object), minimum, maximum, enum,
items, min_items, max_items, required, properties, default.
"""

from __future__ import annotations

import copy
import json
import re
from pathlib import Path

SCHEMA_VERSION = 1
DEFAULT_LICENCE = "LicenseRef-FUSE-Generated"
ID_RE = re.compile(r"^[a-z0-9][a-z0-9_]*(/[a-z0-9][a-z0-9_]*){2,}$")
_FORBIDDEN_LICENCE_RE = re.compile(r"(^CC-BY-NC|^CC-BY-ND|NONCOMMERCIAL|^LicenseRef-(Editorial|RoyaltyFree|Research|Unknown))",
                                   re.IGNORECASE)
_TOP_KEYS = {"schema", "id", "generator", "seed", "params", "licence", "derived_from", "description"}
MAX_SEED = (1 << 63) - 1


class RecipeError(ValueError):
    """Raised with every problem found (one per line)."""

    def __init__(self, problems):
        self.problems = list(problems)
        super().__init__("\n".join(self.problems))


def _yaml_module():
    try:
        import yaml  # type: ignore
    except ImportError:
        return None
    return yaml


def yaml_available() -> bool:
    return _yaml_module() is not None


def load_recipe_file(path) -> dict:
    """Parses a .json / .yaml / .yml recipe (no validation)."""
    path = Path(path)
    text = path.read_text(encoding="utf-8")
    if path.suffix.lower() in (".yaml", ".yml"):
        yaml = _yaml_module()
        if yaml is None:
            raise RecipeError([f"{path}: YAML recipes need PyYAML (pip install pyyaml) or use JSON"])
        data = yaml.safe_load(text)
    else:
        try:
            data = json.loads(text)
        except json.JSONDecodeError as exc:
            raise RecipeError([f"{path}: invalid JSON: {exc}"]) from exc
    if not isinstance(data, dict):
        raise RecipeError([f"{path}: a recipe must be an object / mapping"])
    return data


def _type_ok(value, kind: str) -> bool:
    if kind == "int":
        return isinstance(value, int) and not isinstance(value, bool)
    if kind == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if kind == "bool":
        return isinstance(value, bool)
    if kind == "string":
        return isinstance(value, str)
    if kind == "array":
        return isinstance(value, list)
    if kind == "object":
        return isinstance(value, dict)
    raise ValueError(f"unknown schema type {kind!r}")


def validate_value(value, spec: dict, where: str, problems: list) -> object:
    """Validates `value` against `spec`; returns the value with object defaults filled in."""
    kind = spec.get("type")
    if kind and not _type_ok(value, kind):
        problems.append(f"{where}: expected {kind}, got {type(value).__name__} {value!r}")
        return value
    if isinstance(value, float) and value != value:
        problems.append(f"{where}: NaN is not allowed")
    if "enum" in spec and value not in spec["enum"]:
        problems.append(f"{where}: {value!r} not one of {spec['enum']}")
    if "minimum" in spec and _type_ok(value, "number") and value < spec["minimum"]:
        problems.append(f"{where}: {value} < minimum {spec['minimum']}")
    if "maximum" in spec and _type_ok(value, "number") and value > spec["maximum"]:
        problems.append(f"{where}: {value} > maximum {spec['maximum']}")
    if kind == "array":
        if "min_items" in spec and len(value) < spec["min_items"]:
            problems.append(f"{where}: needs at least {spec['min_items']} item(s)")
        if "max_items" in spec and len(value) > spec["max_items"]:
            problems.append(f"{where}: at most {spec['max_items']} item(s)")
        if "items" in spec:
            value = [validate_value(v, spec["items"], f"{where}[{i}]", problems) for i, v in enumerate(value)]
    if kind == "object":
        props = spec.get("properties", {})
        out = {}
        for key in sorted(value):
            if key not in props:
                if not spec.get("additional", False):
                    problems.append(f"{where}: unknown key {key!r} (allowed: {', '.join(sorted(props))})")
                    continue
                out[key] = value[key]
                continue
            out[key] = validate_value(value[key], props[key], f"{where}.{key}", problems)
        for key in sorted(props):
            if key in out:
                continue
            if key in spec.get("required", []):
                problems.append(f"{where}: missing required key {key!r}")
            elif "default" in props[key]:
                out[key] = copy.deepcopy(props[key]["default"])
        value = out
    return value


def validate_recipe(data: dict, registry=None, source: str = "<recipe>") -> dict:
    """Returns the normalised recipe (defaults filled, keys sorted) or raises RecipeError."""
    if registry is None:
        from . import registry as registry_mod
        registry = registry_mod.REGISTRY
    problems = []
    if not isinstance(data, dict):
        raise RecipeError([f"{source}: a recipe must be an object"])
    for key in sorted(set(data) - _TOP_KEYS):
        problems.append(f"{source}: unknown top-level key {key!r}")
    if data.get("schema") != SCHEMA_VERSION or isinstance(data.get("schema"), bool):
        problems.append(f"{source}: 'schema' must be {SCHEMA_VERSION}")
    rid = data.get("id")
    if not isinstance(rid, str) or not ID_RE.match(rid):
        problems.append(f"{source}: 'id' {rid!r} must look like <class>/<biome|style>/<name>[_<variant>] "
                        "(lower-case [a-z0-9_] segments, at least three)")
    seed = data.get("seed")
    if not isinstance(seed, int) or isinstance(seed, bool) or not 0 <= seed <= MAX_SEED:
        problems.append(f"{source}: 'seed' {seed!r} must be an integer in [0, 2^63-1]")
    licence = data.get("licence", DEFAULT_LICENCE)
    if not isinstance(licence, str) or not licence:
        problems.append(f"{source}: 'licence' must be a non-empty SPDX id or LicenseRef")
    elif _FORBIDDEN_LICENCE_RE.search(licence):
        problems.append(f"{source}: licence {licence!r} is forbidden (FUSE_ASSET_PLAN §2.3 / §2.4 rule 5)")
    derived = data.get("derived_from", [])
    if not isinstance(derived, list) or not all(isinstance(d, str) and d for d in derived):
        problems.append(f"{source}: 'derived_from' must be a list of asset ids")
        derived = []
    if "description" in data and not isinstance(data["description"], str):
        problems.append(f"{source}: 'description' must be a string")
    gen_name = data.get("generator")
    gen = registry.get(gen_name) if isinstance(gen_name, str) else None
    if gen is None:
        problems.append(f"{source}: unknown generator {gen_name!r} (known: {', '.join(registry.names())})")
    params = data.get("params", {})
    if not isinstance(params, dict):
        problems.append(f"{source}: 'params' must be an object")
        params = {}
    if gen is not None:
        spec = {"type": "object", "properties": gen.params, "required": gen.required}
        params = validate_value(params, spec, f"{source}: params", problems)
    if problems:
        raise RecipeError(problems)
    out = {
        "schema": SCHEMA_VERSION,
        "id": rid,
        "generator": gen_name,
        "seed": seed,
        "params": params,
        "licence": licence,
        "derived_from": sorted(set(derived)),
    }
    if "description" in data:
        out["description"] = data["description"]
    return out


def load_and_validate(path, registry=None, seed_override=None) -> dict:
    data = load_recipe_file(path)
    if seed_override is not None:
        data = dict(data)
        data["seed"] = seed_override
    return validate_recipe(data, registry, str(path))


def canonical_json(value) -> bytes:
    """Canonical encoding for hashing: sorted keys, no whitespace, ASCII, repr floats."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode("ascii")
