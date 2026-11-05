"""W4.3: the spec-first SUMMARISE_CODE -> PORT_SCRIPT flow (plan §4.5 steps 2-5).

1. **Inventory** - ``fuse_remaster_tscript_dump <file>`` (W1.6, C++) parses the TorqueScript module; the
   ``diagnostics`` + ``analysis`` parts (functions, call graph, datablocks, handlers) are the call-graph
   context. :func:`tscript_context` runs the tool when ``FUSE_REMASTER_TSCRIPT_DUMP`` names it, else reads a
   committed snapshot next to the source (and the tests check the two agree when the tool is built).
2. **Summarise** - :func:`summarise_recipe`: inputs are the module source + the context, prompt
   ``summarise_code`` (JSON-schema spec), the shared engine API description context-cached on Gemini.
3. **Port** - :func:`port_recipe`: the *spec* is the only input (prompt ``port_script``), so the Lua is a
   reimplementation from the spec, never a translation of the original text (§4.5 step 4).
4. **Check** - :func:`structural_checks` (always): sandbox-forbidden names, a callback for every spec event,
   ``function``/``end`` balance, no global other than the callbacks, and no line of the original source
   copied into the Lua or the spec; :func:`run_parity` (when ``FUSE_REMASTER_LUA_PARITY`` names the harness
   ``fuse_remaster_lua_parity``): the Lua runs in the engine's Lua under ScriptVM's sandbox against a recorded
   event trace and must reproduce the expected event log.

Routing follows A3/W4.6: when the module is an *original* (a user's game script) and the project has no cloud
opt-in, the recipes name no provider and :mod:`fuse_ai.routing` sends them to ``local-llm``.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
from pathlib import Path
from typing import Any, Dict, List, Optional

from . import RECIPE_SCHEMA, prompts
from .errors import GateFailure, RecipeError

SANDBOX_FORBIDDEN = ("io", "os", "require", "load", "loadfile", "dofile", "loadstring", "debug", "package",
                     "collectgarbage", "module")
ALLOWED_GLOBAL_TABLES = ("Entity", "Physics", "Scene", "Audio", "Input", "CVar", "Remaster", "math", "string", "table")


def tscript_context(source: Path, *, exe: Optional[str] = None) -> Dict[str, Any]:
    """{diagnostics, analysis} of a TorqueScript file: from the C++ dump tool, else the committed snapshot."""
    exe = exe or os.environ.get("FUSE_REMASTER_TSCRIPT_DUMP")
    if exe and Path(exe).is_file():
        proc = subprocess.run([exe, str(source)], capture_output=True, timeout=60)
        if proc.returncode not in (0, 1):
            raise RecipeError(f"{exe} failed on {source}: {proc.stderr.decode(errors='replace')}")
        full = json.loads(proc.stdout.decode("utf-8"))
    else:
        snap = source.with_suffix(".dump.json")
        try:
            full = json.loads(snap.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as e:
            raise RecipeError(f"no fuse_remaster_tscript_dump and no snapshot {snap}: {e}") from None
    return {"diagnostics": full.get("diagnostics", []), "analysis": full["analysis"]}


def context_bytes(ctx: Dict[str, Any]) -> bytes:
    return (json.dumps(ctx, sort_keys=True, indent=1) + "\n").encode("utf-8")


def _input(path: Optional[str], data: bytes, name: str, media_type: str, origin: str) -> Dict[str, Any]:
    d: Dict[str, Any] = {"sha256": hashlib.sha256(data).hexdigest(), "name": name, "media_type": media_type,
                         "origin": origin}
    if path:
        d["path"] = path
    if origin == "derived":
        d["derived_from_original"] = False
    return d


def summarise_recipe(module: str, source_rel: str, source: bytes, context_rel: str, context: bytes, *,
                     origin: str = "library", provider: Optional[str] = "gemini",
                     model: Optional[str] = "gemini-3.1-pro-preview", context_cache: bool = True) -> Dict[str, Any]:
    """The SUMMARISE_CODE recipe. ``origin`` is the source's provenance: ``library`` for in-tree MIT template
    code, ``original`` for a user's game script (then the context is derived from an original too)."""
    ctx_origin = "derived"
    d: Dict[str, Any] = {
        "schema": RECIPE_SCHEMA, "id": f"port/{module}/spec", "task": "SUMMARISE_CODE", "seed": 0,
        "purpose": f"behaviour spec for {module} (plan §4.5 step 3)",
        "params": {"context_cache": context_cache},
        "inputs": [_input(source_rel, source, f"{module}.tscript", "text/x-torquescript", origin),
                   _input(context_rel, context, f"{module}.callgraph.json", "application/json", ctx_origin)],
    }
    if origin == "original":
        d["inputs"][1]["derived_from_original"] = True
    if provider:
        d["provider"] = provider
    if model:
        d["model"] = {"id": model}
    return prompts.bind(d, "summarise_code", module=module, language="TorqueScript")


def port_recipe(module: str, spec_rel: str, spec: bytes, *, spec_from_original: bool = False,
                provider: Optional[str] = "gemini", model: Optional[str] = "gemini-3.1-pro-preview",
                context_cache: bool = True) -> Dict[str, Any]:
    """The PORT_SCRIPT recipe: the reviewed spec is the only input (reimplementation from the spec)."""
    inp = _input(spec_rel, spec, f"{module}.spec.json", "application/json", "derived")
    inp["derived_from_original"] = bool(spec_from_original)
    d: Dict[str, Any] = {
        "schema": RECIPE_SCHEMA, "id": f"port/{module}/lua", "task": "PORT_SCRIPT", "seed": 0,
        "purpose": f"FUSE Lua port of {module} from its spec (plan §4.5 step 4)",
        "params": {"context_cache": context_cache}, "inputs": [inp],
    }
    if provider:
        d["provider"] = provider
    if model:
        d["model"] = {"id": model}
    return prompts.bind(d, "port_script", behaviour=module)


# ---- checks -------------------------------------------------------------------------------------------------------

_COMMENT = re.compile(r"--\[(=*)\[.*?\]\1\]|--[^\n]*", re.DOTALL)
_STRING = re.compile(r"\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'")


def _code_only(lua: str) -> str:
    return _STRING.sub('""', _COMMENT.sub("", lua))


def _normalise(line: str) -> str:
    return re.sub(r"\s+", " ", line).strip()


def copied_lines(original: str, *texts: str, min_len: int = 24) -> List[str]:
    """Lines of the original source (normalised, at least ``min_len`` characters) found in any text."""
    hay = [_normalise(t) for t in texts]
    hits = []
    for raw in original.splitlines():
        line = _normalise(raw)
        if len(line) < min_len or line.startswith("//"):
            continue
        if any(line in h for h in hay):
            hits.append(line)
    return hits


def structural_checks(lua: str, spec: Dict[str, Any], *, original_source: Optional[str] = None,
                      spec_text: str = "") -> List[str]:
    problems: List[str] = []
    code = _code_only(lua)
    for name in SANDBOX_FORBIDDEN:
        if re.search(rf"(?<![\w.]){name}\s*[.(\[\"']", code) or re.search(rf"(?<![\w.]){name}\s*$", code, re.M):
            problems.append(f"uses sandbox-forbidden name {name!r}")
    handlers = set(re.findall(r"^\s*function\s+(on_[a-z_]+)\s*\(\s*self\b", code, re.M))
    for ev in spec.get("events", []):
        if ev["name"] not in handlers:
            problems.append(f"no callback for spec event {ev['name']}")
    globals_defined = set(re.findall(r"^\s*function\s+([A-Za-z_][\w.:]*)\s*\(", code, re.M))
    for g in sorted(globals_defined - handlers):
        problems.append(f"defines global function {g!r} (helpers must be local)")
    for m in re.finditer(r"^\s*([A-Za-z_]\w*)\s*=[^=]", code, re.M):
        problems.append(f"assigns global {m.group(1)!r}")
    for m in re.finditer(r"(?<![\w.])([A-Z][A-Za-z]*)\.[a-z_]+\s*\(", code):
        if m.group(1) not in ALLOWED_GLOBAL_TABLES:
            problems.append(f"calls unknown API table {m.group(1)!r}")
    # 'end' closes function, if and do (for/while loops open with 'do'); 'repeat' closes with 'until'
    opens = len(re.findall(r"\b(function|if|do)\b", code))
    ends = len(re.findall(r"\bend\b", code))
    if opens != ends:
        problems.append(f"block structure: {opens} openers (function/if/do) vs {ends} 'end'")
    if original_source:
        for line in copied_lines(original_source, lua, spec_text):
            problems.append(f"original script text copied: {line!r}")
    return problems


def run_parity(lua_path: Path, trace: Path, expected: Path, *, exe: Optional[str] = None) -> Optional[Dict[str, Any]]:
    """Run the Lua through fuse_remaster_lua_parity; None when the harness is not built."""
    exe = exe or os.environ.get("FUSE_REMASTER_LUA_PARITY")
    if not exe or not Path(exe).is_file():
        return None
    proc = subprocess.run([exe, str(lua_path), str(trace), "--expect", str(expected)], capture_output=True,
                          timeout=60)
    res = {"rc": proc.returncode, "log": proc.stdout.decode("utf-8", errors="replace"),
           "stderr": proc.stderr.decode("utf-8", errors="replace")}
    if proc.returncode != 0:
        raise GateFailure(f"parity harness: rc {proc.returncode}\n{res['stderr']}")
    return res
