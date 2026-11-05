"""The ``local-llm`` provider: llama.cpp ``llama-server`` client (plan §3.1, §3.4.1, §3.5; W4.6).

The offline fallback for TAG_IMAGE / INFER_MATERIAL / SUMMARISE_CODE / PORT_SCRIPT, used when a project has
no cloud opt-in (A3), no key, or no network. It uses **the same prompt files and JSON schemas** as the
``gemini`` provider (:mod:`fuse_ai.prompts`), sent through llama-server's OpenAI-compatible
``POST /v1/chat/completions`` with ``response_format: {"type": "json_schema", ...}`` (llama.cpp turns the
schema into a grammar). vLLM and Ollama expose the same endpoint.

* The server URL (``FUSE_AI_LOCAL_LLM_URL``, default ``http://127.0.0.1:8080``) must be a loopback address
  (§3.5: no local endpoint is exposed to the network); anything else is a :class:`PolicyError`.
* Pinned models (§3.4.1, Apache-2.0 GGUF builds): Qwen3-VL-8B-Instruct Q8_0 for the image tasks,
  Qwen3-Coder-30B-A3B-Instruct Q4_K_M for the code tasks. The recipe's ``model.id`` must be one of them and
  the server's reported ``model`` must match it.
* Cost is local: GPU seconds from llama-server's ``timings`` (prompt_ms + predicted_ms), kWh at the 3090
  load figure (§7.4). No USD.
* Tests use :class:`~fuse_ai.transport.ReplayTransport` fixtures (``FUSE_AI_LOCAL_LLM_FIXTURES`` or an
  injected transport); the live transport is only built outside ``FUSE_AI_OFFLINE``.
"""

from __future__ import annotations

import base64
import os
import time
from typing import Any, Callable, Dict, List, Optional, Set, Tuple

from . import prompts
from .errors import PolicyError, ProviderNotImplemented, ProviderUnavailable, RecipeError, SchemaViolation, TransportError
from .imaging import is_png
from .providers import RTX3090_LOAD_KW, Cost, Output, Request, Result, Task
from .transport import HttpRequest, ReplayTransport, Transport, is_loopback, live_transport, send_with_retries

DEFAULT_URL = "http://127.0.0.1:8080"
PINNED_MODELS: Dict[str, Dict[str, Any]] = {
    "qwen3-vl-8b-instruct-q8_0": {"tasks": {Task.TAG_IMAGE, Task.INFER_MATERIAL}, "vram_mb": 9500,
                                  "licence": "Apache-2.0", "hf": "Qwen/Qwen3-VL-8B-Instruct (GGUF Q8_0)"},
    "qwen3-coder-30b-a3b-instruct-q4_k_m": {"tasks": {Task.SUMMARISE_CODE, Task.PORT_SCRIPT}, "vram_mb": 20500,
                                            "licence": "Apache-2.0",
                                            "hf": "Qwen/Qwen3-Coder-30B-A3B-Instruct (GGUF Q4_K_M)"},
}
DEFAULT_MODEL = {Task.TAG_IMAGE: "qwen3-vl-8b-instruct-q8_0", Task.INFER_MATERIAL: "qwen3-vl-8b-instruct-q8_0",
                 Task.SUMMARISE_CODE: "qwen3-coder-30b-a3b-instruct-q4_k_m",
                 Task.PORT_SCRIPT: "qwen3-coder-30b-a3b-instruct-q4_k_m"}
_SECONDS = {Task.TAG_IMAGE: 3.0, Task.INFER_MATERIAL: 3.0, Task.SUMMARISE_CODE: 120.0, Task.PORT_SCRIPT: 300.0}
OUTPUT_NAMES = {Task.TAG_IMAGE: "tags.json", Task.INFER_MATERIAL: "material.json",
                Task.SUMMARISE_CODE: "spec.json", Task.PORT_SCRIPT: "port.json"}


def pinned_model(req: Request) -> str:
    model = req.model_id or DEFAULT_MODEL.get(req.task, "")
    info = PINNED_MODELS.get(model)
    if info is None:
        raise RecipeError(f"local-llm model {model!r} is not pinned ({', '.join(sorted(PINNED_MODELS))})")
    if req.task not in info["tasks"]:
        raise RecipeError(f"local-llm model {model!r} does not serve {req.task.value}")
    return model


def build_chat_body(req: Request, prompt: prompts.Prompt, model: str) -> Dict[str, Any]:
    system = prompt.system
    if prompt.context:
        system = system + "\n\n" + prompt.context_text()
    user_text = prompt.render_user(dict(req.params.get("vars", {})))
    content: Any
    if prompt.attach == "image":
        content = [{"type": "text", "text": user_text}]
        for n, ref in enumerate(req.inputs):
            data = req.input_data[n] if n < len(req.input_data) else None
            if data is None:
                raise RecipeError(f"input {n} ({ref.content_key}) has no bytes")
            mime = ref.media_type or ("image/png" if is_png(data) else "application/octet-stream")
            content.append({"type": "image_url",
                            "image_url": {"url": f"data:{mime};base64,{base64.b64encode(data).decode('ascii')}"}})
    else:
        chunks = [user_text]
        if prompt.attach == "text":
            for n, ref in enumerate(req.inputs):
                data = req.input_data[n] if n < len(req.input_data) else None
                if data is None:
                    raise RecipeError(f"input {n} ({ref.content_key}) has no bytes")
                name = ref.name or ref.content_key or f"input{n}"
                chunks.append(f"<input name=\"{name}\" media_type=\"{ref.media_type or 'text/plain'}\">\n"
                              f"{data.decode('utf-8', errors='replace')}\n</input>")
        content = "\n\n".join(chunks)
    body: Dict[str, Any] = {
        "model": model,
        "messages": [{"role": "system", "content": system}, {"role": "user", "content": content}],
        "temperature": 0, "seed": int(req.seed), "stream": False,
        "max_tokens": int(req.params.get("max_output_tokens", prompt.max_output_tokens)),
    }
    if prompt.output_schema is not None:
        body["response_format"] = {"type": "json_schema",
                                   "json_schema": {"name": prompt.id, "strict": True, "schema": prompt.output_schema}}
    return body


class LocalLLMProvider:
    id = "local-llm"
    cloud = False

    def __init__(self, transport: Optional[Transport] = None, *, base_url: Optional[str] = None,
                 sleep: Callable[[float], None] = time.sleep, max_retries: int = 2):
        self._transport_obj = transport
        self.base_url = base_url or os.environ.get("FUSE_AI_LOCAL_LLM_URL", DEFAULT_URL)
        self.sleep = sleep
        self.max_retries = max_retries
        self.http_log: List[Dict[str, Any]] = []

    def capabilities(self) -> Set[Task]:
        return set(DEFAULT_MODEL)

    def vram_mb(self, req: Request) -> int:
        try:
            return int(PINNED_MODELS[pinned_model(req)]["vram_mb"])
        except RecipeError:
            return 0

    def estimate(self, req: Request) -> Cost:
        seconds = float(req.params.get("est_seconds", _SECONDS.get(req.task, 60.0)))
        return Cost(seconds=seconds, gpu="rtx3090", peak_vram_mb=self.vram_mb(req),
                    kwh=seconds / 3600.0 * RTX3090_LOAD_KW)

    def _transport(self) -> Transport:
        if not is_loopback(self.base_url):
            raise PolicyError(f"local-llm URL {self.base_url!r} is not a loopback address (§3.5)")
        if self._transport_obj is not None:
            return self._transport_obj
        fixtures = os.environ.get("FUSE_AI_LOCAL_LLM_FIXTURES")
        if fixtures:
            self._transport_obj = ReplayTransport.load(*[p for p in fixtures.split(os.pathsep) if p])
        else:
            if os.environ.get("FUSE_AI_OFFLINE", "") not in ("", "0"):
                raise ProviderUnavailable("local-llm: FUSE_AI_OFFLINE is set and no fixtures are configured "
                                          "(FUSE_AI_LOCAL_LLM_FIXTURES)")
            self._transport_obj = live_transport(self.base_url, local_only=True)
        return self._transport_obj

    def run(self, req: Request) -> Result:
        transport = self._transport()
        if req.task not in DEFAULT_MODEL:
            raise ProviderNotImplemented(f"local-llm does not serve {req.task.value}")
        model = pinned_model(req)
        pr = prompts.for_params(req.params, req.task)
        if pr is None:
            raise RecipeError(f"local-llm {req.task.value} needs params.prompt (the same prompt files as gemini)")
        body = build_chat_body(req, pr, model)
        resp = send_with_retries(transport, HttpRequest("POST", "/v1/chat/completions", body), sleep=self.sleep,
                                 max_retries=self.max_retries, log=self.http_log)
        if not resp.ok:
            raise TransportError(f"local-llm /v1/chat/completions: HTTP {resp.status}")
        content, usage, timings = parse_chat_response(model, resp.body)
        value = prompts.parse_output(pr, content.encode("utf-8"))
        seconds = (float(timings.get("prompt_ms", 0.0)) + float(timings.get("predicted_ms", 0.0))) / 1000.0
        cost = Cost(tokens_in=int(usage.get("prompt_tokens", 0)), tokens_out=int(usage.get("completion_tokens", 0)),
                    seconds=seconds, gpu="rtx3090", peak_vram_mb=self.vram_mb(req),
                    kwh=seconds / 3600.0 * RTX3090_LOAD_KW)
        outputs = [Output(OUTPUT_NAMES[req.task], "application/json", prompts.canonical_output(value))]
        if req.task is Task.PORT_SCRIPT:
            lua = value.get("lua") if isinstance(value, dict) else None
            if not isinstance(lua, str):
                raise SchemaViolation("PORT_SCRIPT answer has no 'lua' string")
            outputs.append(Output("port.lua", "text/x-lua", (lua.rstrip("\n") + "\n").encode("utf-8")))
        meta = {"prompt": pr.id, "prompt_sha256": pr.sha256, "usage": usage, "timings": timings,
                "licence": PINNED_MODELS[model]["licence"], "server": "llama.cpp llama-server (OpenAI-compatible)"}
        return Result(outputs=outputs, meta=meta, cost=cost, provider_id=self.id, model_id=model)


def parse_chat_response(model: str, body: Any) -> Tuple[str, Dict[str, Any], Dict[str, Any]]:
    if not isinstance(body, dict) or not isinstance(body.get("choices"), list) or len(body["choices"]) != 1:
        raise SchemaViolation("chat completion response must have exactly one choice")
    served = str(body.get("model", model))
    if served != model and not served.endswith("/" + model) and not served.startswith(model):
        raise TransportError(f"model drift: recipe pins {model!r}, llama-server reports {served!r}")
    choice = body["choices"][0]
    if choice.get("finish_reason") != "stop":
        raise SchemaViolation(f"chat completion finished with {choice.get('finish_reason')!r} (not 'stop')")
    content = (choice.get("message") or {}).get("content")
    if not isinstance(content, str):
        raise SchemaViolation("chat completion has no message content")
    return content, dict(body.get("usage") or {}), dict(body.get("timings") or {})
