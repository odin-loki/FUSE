"""A small, strict JSON-schema validator (stdlib only) for model outputs (plan §4.1 step 2, W4.2).

Supports the subset the prompt schemas under ``Content/recipes/ai/prompts/`` use (draft 2020-12 keywords):
``type`` (string or list), ``enum``, ``const``, ``properties``, ``required``, ``additionalProperties``
(bool or schema), ``items``, ``minItems``, ``maxItems``, ``uniqueItems``, ``minimum``, ``maximum``,
``minLength``, ``maxLength``, ``pattern``, ``$defs`` + local ``$ref`` (``#/$defs/name``), ``anyOf``,
``description`` / ``title`` / ``$schema`` / ``$id`` (ignored).

Unknown keywords are an error in :func:`check_schema`, so a prompt cannot silently rely on a keyword this
validator ignores. ``integer`` rejects booleans and floats; ``number`` rejects booleans.
"""

from __future__ import annotations

import re
from typing import Any, Dict, List

KNOWN_KEYWORDS = {
    "type", "enum", "const", "properties", "required", "additionalProperties", "items", "minItems", "maxItems",
    "uniqueItems", "minimum", "maximum", "minLength", "maxLength", "pattern", "$defs", "$ref", "anyOf",
    "description", "title", "$schema", "$id",
}
_TYPES = ("object", "array", "string", "integer", "number", "boolean", "null")


def _type_ok(t: str, v: Any) -> bool:
    if t == "object":
        return isinstance(v, dict)
    if t == "array":
        return isinstance(v, list)
    if t == "string":
        return isinstance(v, str)
    if t == "integer":
        return isinstance(v, int) and not isinstance(v, bool)
    if t == "number":
        return isinstance(v, (int, float)) and not isinstance(v, bool)
    if t == "boolean":
        return isinstance(v, bool)
    if t == "null":
        return v is None
    return False


def check_schema(schema: Any, where: str = "#") -> List[str]:
    """Problems with the schema itself (unknown keywords, bad types)."""
    errs: List[str] = []
    if not isinstance(schema, dict):
        return [f"{where}: a schema must be an object"]
    for k in schema:
        if k not in KNOWN_KEYWORDS:
            errs.append(f"{where}: unsupported keyword {k!r}")
    t = schema.get("type")
    for tt in ([t] if isinstance(t, str) else (t or [])):
        if tt not in _TYPES:
            errs.append(f"{where}: unknown type {tt!r}")
    for name, sub in (schema.get("properties") or {}).items():
        errs += check_schema(sub, f"{where}/properties/{name}")
    for name, sub in (schema.get("$defs") or {}).items():
        errs += check_schema(sub, f"{where}/$defs/{name}")
    if isinstance(schema.get("items"), dict):
        errs += check_schema(schema["items"], f"{where}/items")
    if isinstance(schema.get("additionalProperties"), dict):
        errs += check_schema(schema["additionalProperties"], f"{where}/additionalProperties")
    for i, sub in enumerate(schema.get("anyOf") or []):
        errs += check_schema(sub, f"{where}/anyOf/{i}")
    if "pattern" in schema:
        try:
            re.compile(schema["pattern"])
        except re.error as e:
            errs.append(f"{where}: bad pattern: {e}")
    return errs


def validate(value: Any, schema: Dict[str, Any]) -> List[str]:
    """Every violation of ``value`` against ``schema`` (empty list == valid)."""
    errs: List[str] = []
    _validate(value, schema, schema, "$", errs)
    return errs


def _resolve(ref: str, root: Dict[str, Any]) -> Dict[str, Any]:
    if not ref.startswith("#/$defs/"):
        raise ValueError(f"only local #/$defs/ refs are supported, got {ref!r}")
    return root["$defs"][ref[len("#/$defs/"):]]


def _validate(v: Any, s: Dict[str, Any], root: Dict[str, Any], path: str, errs: List[str]) -> None:
    if "$ref" in s:
        _validate(v, _resolve(s["$ref"], root), root, path, errs)
        return
    if "anyOf" in s:
        if not any(not validate_sub(v, sub, root) for sub in s["anyOf"]):
            errs.append(f"{path}: matches none of anyOf")
            return
    t = s.get("type")
    if t is not None:
        types = [t] if isinstance(t, str) else list(t)
        if not any(_type_ok(tt, v) for tt in types):
            errs.append(f"{path}: expected {'/'.join(types)}, got {type(v).__name__}")
            return
    if "const" in s and v != s["const"]:
        errs.append(f"{path}: must be {s['const']!r}")
    if "enum" in s and v not in s["enum"]:
        errs.append(f"{path}: {v!r} is not one of {s['enum']}")
    if isinstance(v, str):
        if "minLength" in s and len(v) < s["minLength"]:
            errs.append(f"{path}: shorter than {s['minLength']}")
        if "maxLength" in s and len(v) > s["maxLength"]:
            errs.append(f"{path}: longer than {s['maxLength']}")
        if "pattern" in s and not re.search(s["pattern"], v):
            errs.append(f"{path}: does not match {s['pattern']!r}")
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        if "minimum" in s and v < s["minimum"]:
            errs.append(f"{path}: {v} < minimum {s['minimum']}")
        if "maximum" in s and v > s["maximum"]:
            errs.append(f"{path}: {v} > maximum {s['maximum']}")
    if isinstance(v, list):
        if "minItems" in s and len(v) < s["minItems"]:
            errs.append(f"{path}: fewer than {s['minItems']} items")
        if "maxItems" in s and len(v) > s["maxItems"]:
            errs.append(f"{path}: more than {s['maxItems']} items")
        if s.get("uniqueItems"):
            seen = []
            for item in v:
                if item in seen:
                    errs.append(f"{path}: duplicate item {item!r}")
                    break
                seen.append(item)
        if isinstance(s.get("items"), dict):
            for i, item in enumerate(v):
                _validate(item, s["items"], root, f"{path}[{i}]", errs)
    if isinstance(v, dict):
        props = s.get("properties", {})
        for req in s.get("required", []):
            if req not in v:
                errs.append(f"{path}: missing required {req!r}")
        addl = s.get("additionalProperties", True)
        for k, item in v.items():
            if k in props:
                _validate(item, props[k], root, f"{path}.{k}", errs)
            elif addl is False:
                errs.append(f"{path}: unexpected property {k!r}")
            elif isinstance(addl, dict):
                _validate(item, addl, root, f"{path}.{k}", errs)


def validate_sub(v: Any, s: Dict[str, Any], root: Dict[str, Any]) -> List[str]:
    errs: List[str] = []
    _validate(v, s, root, "$", errs)
    return errs
