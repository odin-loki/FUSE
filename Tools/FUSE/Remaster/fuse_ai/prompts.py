"""Versioned prompts with JSON-schema-constrained outputs (plan §4.1 step 2, §4.5, W4.2 / W4.3 / W4.6).

Prompt files live in ``Content/recipes/ai/prompts/<id>.prompt.json`` (override the directory with
``FUSE_AI_PROMPTS_DIR``)::

    {"schema": "fuse.ai-prompt/1", "id": "tag_image", "version": 1, "task": "TAG_IMAGE",
     "system": "...", "user": "... {name} ...",          # str.format_map over the recipe's params.vars
     "attach": "image" | "text",                           # how the recipe inputs are attached
     "context": ["context/fuse_lua_api.md"],               # optional shared context (Gemini context caching)
     "max_output_tokens": 1024,
     "output_schema": {...JSON schema (fuse_ai.jsonschema subset)...}}

A recipe binds a prompt with ``params.prompt = "<id>"`` and ``params.prompt_sha256 = <sha256 of the
canonical prompt JSON>`` (:func:`bind`). The sha256 is part of the recipe hash, so editing a prompt
invalidates the cache instead of silently replaying answers to the old prompt; a provider refuses a
recipe whose pinned sha256 does not match the file. The same prompt and schema serve every provider that
supports the task (``gemini``, ``local-llm``).

:func:`parse_output` is the one place a model answer becomes data: it must be UTF-8 JSON (one optional
```json fence is tolerated, for local models) and must validate against ``output_schema``; anything else is
a :class:`SchemaViolation`. The runner applies it to live *and* replayed outputs.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import string
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional

from . import jsonschema
from .errors import RecipeError, SchemaViolation
from .providers import Task

PROMPT_SCHEMA = "fuse.ai-prompt/1"
_KEYS = {"schema", "id", "version", "task", "system", "user", "attach", "context", "max_output_tokens",
         "output_schema", "description", "notes"}
_REPO_ROOT = Path(__file__).resolve().parents[4]


def prompts_dir() -> Path:
    env = os.environ.get("FUSE_AI_PROMPTS_DIR")
    return Path(env) if env else _REPO_ROOT / "Content" / "recipes" / "ai" / "prompts"


def canonical_bytes(d: Dict[str, Any]) -> bytes:
    return json.dumps(d, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


@dataclass
class Prompt:
    id: str
    version: int
    task: Task
    system: str
    user: str
    attach: str
    context: List[str]
    max_output_tokens: int
    output_schema: Optional[Dict[str, Any]]
    sha256: str
    path: Optional[Path] = None

    def render_user(self, variables: Dict[str, Any]) -> str:
        fields = {f for _, f, _, _ in string.Formatter().parse(self.user) if f}
        missing = sorted(f for f in fields if f not in variables)
        if missing:
            raise RecipeError(f"prompt {self.id!r} needs params.vars {missing}")
        return self.user.format_map({k: str(v) for k, v in variables.items()})

    def context_text(self) -> str:
        parts = []
        for rel in self.context:
            p = prompts_dir() / rel
            try:
                parts.append(f"<context file=\"{rel}\">\n{p.read_text(encoding='utf-8')}\n</context>")
            except OSError as e:
                raise RecipeError(f"prompt {self.id!r}: cannot read context {p}: {e}") from None
        return "\n".join(parts)

    @property
    def context_sha256(self) -> str:
        return hashlib.sha256(self.context_text().encode("utf-8")).hexdigest() if self.context else ""


def from_json(d: Any, path: Optional[Path] = None) -> Prompt:
    errs = []
    if not isinstance(d, dict):
        raise RecipeError(f"prompt {path}: must be a JSON object")
    if d.get("schema") != PROMPT_SCHEMA:
        errs.append(f"schema must be {PROMPT_SCHEMA!r}")
    for k in d:
        if k not in _KEYS:
            errs.append(f"unknown key {k!r}")
    for k in ("id", "system", "user"):
        if not isinstance(d.get(k), str) or not d.get(k):
            errs.append(f"{k} must be a non-empty string")
    if not isinstance(d.get("version"), int):
        errs.append("version must be an integer")
    if d.get("task") not in Task.__members__:
        errs.append(f"task must be a Task, got {d.get('task')!r}")
    if d.get("attach", "text") not in ("image", "text", "none"):
        errs.append("attach must be image, text or none")
    schema = d.get("output_schema")
    if schema is not None:
        errs += [f"output_schema: {e}" for e in jsonschema.check_schema(schema)]
    if errs:
        raise RecipeError(f"invalid prompt {path or d.get('id')}: " + "; ".join(errs))
    return Prompt(id=d["id"], version=d["version"], task=Task(d["task"]), system=d["system"], user=d["user"],
                  attach=d.get("attach", "text"), context=list(d.get("context", [])),
                  max_output_tokens=int(d.get("max_output_tokens", 2048)), output_schema=schema,
                  sha256=hashlib.sha256(canonical_bytes(d)).hexdigest(), path=path)


_ID = re.compile(r"^[a-z0-9_]+$")


def load(prompt_id: str) -> Prompt:
    if not _ID.match(prompt_id or ""):
        raise RecipeError(f"bad prompt id {prompt_id!r}")
    p = prompts_dir() / f"{prompt_id}.prompt.json"
    try:
        d = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        raise RecipeError(f"cannot read prompt {p}: {e}") from None
    pr = from_json(d, p)
    if pr.id != prompt_id:
        raise RecipeError(f"prompt file {p} has id {pr.id!r}")
    return pr


PROMPT_TASKS = (Task.TAG_IMAGE, Task.INFER_MATERIAL, Task.SUMMARISE_CODE, Task.PORT_SCRIPT)


def for_params(params: Dict[str, Any], task: Task) -> Optional[Prompt]:
    """The prompt a recipe's params bind (None when the recipe uses no prompt file). Only the JSON agent tasks
    bind prompt files; for EDIT_IMAGE / GEN_IMAGE ``params.prompt`` is our own image prompt text."""
    pid = params.get("prompt")
    if not pid or task not in PROMPT_TASKS:
        return None
    pr = load(str(pid))
    if pr.task is not task:
        raise RecipeError(f"prompt {pr.id!r} is for {pr.task.value}, the recipe task is {task.value}")
    pinned = params.get("prompt_sha256")
    if pinned != pr.sha256:
        raise RecipeError(f"prompt {pr.id!r} changed: recipe pins sha256 {pinned}, file is {pr.sha256}; "
                          "re-bind the recipe (fuse_ai.prompts.bind) and re-record")
    return pr


def bind(recipe_json: Dict[str, Any], prompt_id: str, **variables: Any) -> Dict[str, Any]:
    """Return a copy of a recipe dict with ``params.prompt`` / ``prompt_sha256`` / ``vars`` set."""
    pr = load(prompt_id)
    d = json.loads(json.dumps(recipe_json))
    d.setdefault("params", {})
    d["params"]["prompt"] = pr.id
    d["params"]["prompt_sha256"] = pr.sha256
    if variables:
        d["params"]["vars"] = dict(variables)
    d["task"] = pr.task.value
    return d


_FENCE = re.compile(r"^\s*```(?:json)?\s*\n(.*)\n```\s*$", re.DOTALL)


def parse_output(prompt: Prompt, data: bytes) -> Any:
    """Model answer bytes -> validated JSON value; :class:`SchemaViolation` otherwise."""
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        raise SchemaViolation(f"prompt {prompt.id!r}: output is not UTF-8") from None
    m = _FENCE.match(text)
    if m:
        text = m.group(1)
    try:
        value = json.loads(text)
    except json.JSONDecodeError as e:
        raise SchemaViolation(f"prompt {prompt.id!r}: output is not JSON: {e}") from None
    if prompt.output_schema is not None:
        errs = jsonschema.validate(value, prompt.output_schema)
        if errs:
            raise SchemaViolation(f"prompt {prompt.id!r}: output violates its schema: " + "; ".join(errs[:8]))
    return value


def canonical_output(value: Any) -> bytes:
    """The bytes a provider stores for a validated JSON answer (canonical, so replays are byte-stable)."""
    return (json.dumps(value, sort_keys=True, indent=1, ensure_ascii=False) + "\n").encode("utf-8")
