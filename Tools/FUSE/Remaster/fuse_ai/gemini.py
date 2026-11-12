"""The ``gemini`` provider (plan §3.1, §3.2; W4.1 REST client, W4.2 schema prompts, W4.4 Nano Banana images).

Talks to the Gemini API REST surface through a :mod:`fuse_ai.transport` transport; it never opens a socket
itself. In CI and in the tests the transport is a :class:`~fuse_ai.transport.ReplayTransport` over
hand-written fixtures (``FUSE_AI_GEMINI_FIXTURES`` or an injected transport); a live transport is only built
with ``GEMINI_API_KEY`` set and ``FUSE_AI_OFFLINE`` unset (:func:`fuse_ai.transport.live_transport`).

What it implements (REST shapes per the Gemini API docs [M1]; the batch and image-config field names are
**[VERIFY against the live API when recording the first fixtures]**):

* ``POST /v1beta/models/{model}:generateContent`` for TAG_IMAGE / INFER_MATERIAL / SUMMARISE_CODE /
  PORT_SCRIPT with ``generationConfig.responseMimeType = application/json`` and ``responseJsonSchema`` set
  from the prompt's ``output_schema`` (W4.2), ``temperature: 0`` and the recipe seed (§3.3).
* EDIT_IMAGE / GEN_IMAGE through the same endpoint with ``responseModalities: ["IMAGE"]`` and
  ``imageConfig.imageSize`` (Nano Banana). Every output is marked ``watermark: synthid`` (§0.1 table, [M5]).
* **Pinned model ids**: a recipe must name one of the §3.2 ids exactly (no ``-latest`` aliases); a response
  whose ``modelVersion`` is not that id (or a dated build of it) is refused as model drift.
* **Batch mode** (:meth:`GeminiProvider.run_batch`): ``POST .../{model}:batchGenerateContent`` with inlined
  requests keyed by recipe hash, then ``GET /v1beta/batches/{id}`` until a terminal state; 50 % price.
* **Context caching**: a prompt with ``context`` files and ``params.context_cache: true`` creates one
  ``cachedContents`` entry per (model, prompt, context sha256) and references it with ``cachedContent``;
  cached tokens are billed at :data:`CACHE_READ_FACTOR` of the input price (§3.2), creation at full price.
* **Retries**: 408/429/5xx with exponential backoff and ``Retry-After`` (:func:`send_with_retries`).
* **Cost accounting** from ``usageMetadata`` (prompt, cached, candidates and thoughts tokens) with the §3.2
  prices, the Pro >200k-token tier, the batch discount and per-image prices.

The W0.8 guard still applies: ``run()`` and ``run_batch()`` call :func:`fuse_ai.policy.verify_clearance` before
anything else, so a request that did not pass :func:`fuse_ai.policy.check_request` never reaches a transport.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import time
from typing import Any, Callable, Dict, List, Optional, Tuple

from . import policy, prompts
from .errors import ProviderNotImplemented, ProviderUnavailable, RecipeError, SchemaViolation, TransportError
from .imaging import is_png
from .providers import (GEMINI_DEFAULT_MODEL, GEMINI_IMAGE_PRICES, GEMINI_TOKEN_PRICES, PROVIDER_TABLE,
                        VEO_PRICE_PER_SECOND, Cost, GeminiStub, Output, Request, Result, Task)
from .transport import HttpRequest, HttpResponse, ReplayTransport, Transport, live_transport, send_with_retries

BASE_URL = "https://generativelanguage.googleapis.com"
API_VERSION = "v1beta"
CACHE_READ_FACTOR = 0.20          # §3.2: context-cache reads cost about 20 % of input
BATCH_FACTOR = 0.50               # §3.2: batch mode is 50 % off
PRO_LONG_CONTEXT_TOKENS = 200_000  # §3.2: gemini-3.1-pro-preview is $4 / $18 above 200k context
PRO_LONG_PRICES = {"gemini-3.1-pro-preview": (4.00, 18.00)}
PINNED_MODELS = frozenset(GEMINI_TOKEN_PRICES) | frozenset(GEMINI_IMAGE_PRICES) | frozenset(VEO_PRICE_PER_SECOND)
JSON_TASKS = (Task.TAG_IMAGE, Task.INFER_MATERIAL, Task.SUMMARISE_CODE, Task.PORT_SCRIPT)
IMAGE_TASKS = (Task.EDIT_IMAGE, Task.GEN_IMAGE)
OUTPUT_NAMES = {Task.TAG_IMAGE: "tags.json", Task.INFER_MATERIAL: "material.json",
                Task.SUMMARISE_CODE: "spec.json", Task.PORT_SCRIPT: "port.json"}
IMAGE_LICENCE = "LicenseRef-AI-Gemini-Image"
TEXT_LICENCE = "LicenseRef-AI-Gemini"
TERMINAL_BATCH_STATES = ("BATCH_STATE_SUCCEEDED", "BATCH_STATE_FAILED", "BATCH_STATE_CANCELLED",
                         "BATCH_STATE_EXPIRED")


# ---- pure helpers (also used by the contract tests and the fixture authoring script) ---------------------------


def pinned_model(req: Request) -> str:
    model = req.model_id
    if not model:
        model = GEMINI_DEFAULT_MODEL.get(req.task, "gemini-3.8-flash")
    if model not in PINNED_MODELS:
        raise RecipeError(f"gemini model {model!r} is not a pinned §3.2 id ({', '.join(sorted(PINNED_MODELS))}); "
                          "recipes pin exact model ids, never aliases")
    if req.task in IMAGE_TASKS and model not in GEMINI_IMAGE_PRICES:
        raise RecipeError(f"{req.task.value} needs an image model ({', '.join(sorted(GEMINI_IMAGE_PRICES))})")
    if req.task in JSON_TASKS and model not in GEMINI_TOKEN_PRICES:
        raise RecipeError(f"{req.task.value} needs a text model ({', '.join(sorted(GEMINI_TOKEN_PRICES))})")
    return model


def _input_parts(req: Request, attach: str) -> List[Dict[str, Any]]:
    parts: List[Dict[str, Any]] = []
    if attach == "none":
        return parts
    for n, ref in enumerate(req.inputs):
        data = req.input_data[n] if n < len(req.input_data) else None
        if data is None:
            raise RecipeError(f"input {n} ({ref.content_key}) has no bytes: resolve it from a path or the cache")
        if attach == "image":
            mime = ref.media_type or ("image/png" if is_png(data) else "application/octet-stream")
            parts.append({"inlineData": {"mimeType": mime, "data": base64.b64encode(data).decode("ascii")}})
        else:
            name = ref.name or ref.content_key or f"input{n}"
            mime = ref.media_type or "text/plain"
            parts.append({"text": f"<input name=\"{name}\" media_type=\"{mime}\">\n"
                                  f"{data.decode('utf-8', errors='replace')}\n</input>"})
    return parts


def build_generate_body(req: Request, prompt: prompts.Prompt, *, cached_content: Optional[str] = None
                        ) -> Dict[str, Any]:
    """The generateContent body for a JSON-schema prompt (W4.1 / W4.2)."""
    user = prompt.render_user(dict(req.params.get("vars", {})))
    parts = [{"text": user}] + _input_parts(req, prompt.attach)
    body: Dict[str, Any] = {"contents": [{"role": "user", "parts": parts}]}
    if cached_content:
        body["cachedContent"] = cached_content  # system instruction + context live in the cache
    else:
        system = prompt.system
        if prompt.context:
            system = system + "\n\n" + prompt.context_text()
        body["systemInstruction"] = {"parts": [{"text": system}]}
    gen: Dict[str, Any] = {"temperature": 0, "seed": int(req.seed), "candidateCount": 1,
                           "maxOutputTokens": int(req.params.get("max_output_tokens", prompt.max_output_tokens))}
    if prompt.output_schema is not None:
        gen["responseMimeType"] = "application/json"
        gen["responseJsonSchema"] = prompt.output_schema
    body["generationConfig"] = gen
    return body


def build_image_body(req: Request) -> Dict[str, Any]:
    """The generateContent body for Nano Banana EDIT_IMAGE / GEN_IMAGE (W4.4) [VERIFY imageConfig names]."""
    text = str(req.params.get("prompt", "")).strip()
    if not text:
        raise RecipeError(f"{req.task.value} needs params.prompt (our own words, §4.2)")
    parts: List[Dict[str, Any]] = [{"text": text}]
    if req.task is Task.EDIT_IMAGE:
        if not req.inputs:
            raise RecipeError("EDIT_IMAGE needs an input image")
        parts += _input_parts(req, "image")
    gen = {"responseModalities": ["IMAGE"], "seed": int(req.seed), "candidateCount": 1,
           "imageConfig": {"imageSize": str(req.params.get("size", "1K")),
                           "aspectRatio": str(req.params.get("aspect_ratio", "1:1"))}}
    return {"contents": [{"role": "user", "parts": parts}], "generationConfig": gen}


def build_cache_body(model: str, prompt: prompts.Prompt, ttl_s: int = 3600) -> Dict[str, Any]:
    return {"model": f"models/{model}",
            "displayName": f"fuse-ai-{prompt.id}-{prompt.context_sha256[:12]}",
            "systemInstruction": {"parts": [{"text": prompt.system}]},
            "contents": [{"role": "user", "parts": [{"text": prompt.context_text()}]}],
            "ttl": f"{int(ttl_s)}s"}


def token_cost(model: str, usage: Dict[str, Any], *, batch: bool = False) -> Cost:
    prompt_t = int(usage.get("promptTokenCount", 0))
    cached_t = int(usage.get("cachedContentTokenCount", 0))
    out_t = int(usage.get("candidatesTokenCount", 0)) + int(usage.get("thoughtsTokenCount", 0))
    p_in, p_out = GEMINI_TOKEN_PRICES[model]
    if model in PRO_LONG_PRICES and prompt_t > PRO_LONG_CONTEXT_TOKENS:
        p_in, p_out = PRO_LONG_PRICES[model]
    usd = ((prompt_t - cached_t) * p_in + cached_t * p_in * CACHE_READ_FACTOR + out_t * p_out) / 1e6
    if batch:
        usd *= BATCH_FACTOR
    return Cost(usd=usd, tokens_in=prompt_t, tokens_out=out_t)


def image_cost(model: str, size: str, n: int, *, batch: bool = False) -> Cost:
    table = GEMINI_IMAGE_PRICES[model]
    if size not in table:
        raise RecipeError(f"{model} has no price for size {size!r} (sizes: {', '.join(table)})")
    return Cost(usd=table[size] * n * (BATCH_FACTOR if batch else 1.0))


def _check_model_version(model: str, body: Dict[str, Any]) -> str:
    served = str(body.get("modelVersion") or model)
    if not (served == model or served.startswith(model + "-")):
        raise TransportError(f"model drift: recipe pins {model!r} but the API served {served!r}")
    return served


def parse_generate_response(model: str, body: Any) -> Tuple[List[Dict[str, Any]], Dict[str, Any], str, str]:
    """(parts, usageMetadata, finishReason, modelVersion) of a GenerateContentResponse; refuses blocks."""
    if not isinstance(body, dict):
        raise SchemaViolation("generateContent response is not a JSON object")
    fb = body.get("promptFeedback") or {}
    if fb.get("blockReason"):
        raise SchemaViolation(f"prompt blocked by the API: {fb['blockReason']}")
    cands = body.get("candidates") or []
    if len(cands) != 1:
        raise SchemaViolation(f"expected exactly one candidate, got {len(cands)}")
    cand = cands[0]
    finish = str(cand.get("finishReason", ""))
    if finish != "STOP":
        raise SchemaViolation(f"candidate finished with {finish or 'no finishReason'} (not STOP)")
    parts = (cand.get("content") or {}).get("parts") or []
    served = _check_model_version(model, body)
    return parts, dict(body.get("usageMetadata") or {}), finish, served


def _api_error(resp: HttpResponse, what: str) -> TransportError:
    msg = ""
    if isinstance(resp.body, dict) and isinstance(resp.body.get("error"), dict):
        msg = f": {resp.body['error'].get('status', '')} {resp.body['error'].get('message', '')}".rstrip()
    return TransportError(f"{what}: HTTP {resp.status}{msg}")


# ---- the provider -------------------------------------------------------------------------------------------------


class GeminiProvider(GeminiStub):
    """W4.1 provider. ``estimate()`` and ``vram_mb()`` come from the W0 stub (the §3.2 price list)."""

    def __init__(self, transport: Optional[Transport] = None, *, api_key: Optional[str] = None,
                 sleep: Callable[[float], None] = time.sleep, max_retries: int = 4, poll_interval_s: float = 10.0,
                 max_polls: int = 360):
        super().__init__(PROVIDER_TABLE["gemini"])
        self._transport_obj = transport
        self._api_key = api_key
        self.sleep = sleep
        self.max_retries = max_retries
        self.poll_interval_s = poll_interval_s
        self.max_polls = max_polls
        self._caches: Dict[Tuple[str, str, str], str] = {}
        self.http_log: List[Dict[str, Any]] = []

    def estimate(self, req: Request) -> Cost:
        """The §3.2 price list (W0 stub arithmetic), with the bound prompt's output cap and text size."""
        if req.task in JSON_TASKS and req.params.get("prompt") and "max_output_tokens" not in req.params:
            try:
                pr = prompts.for_params(req.params, req.task)
            except RecipeError:
                pr = None
            if pr is not None:
                params = dict(req.params, max_output_tokens=pr.max_output_tokens)
                extra = len((pr.system + pr.user + pr.context_text()).encode("utf-8")) // 4
                params.setdefault("est_tokens_in", sum(len(d) // 4 for d in req.input_data if d) + extra + 64)
                req = Request(task=req.task, provider_id=req.provider_id, model=req.model, params=params, seed=req.seed,
                              inputs=req.inputs, input_data=req.input_data, purpose=req.purpose,
                              recipe_hash=req.recipe_hash)
        return super().estimate(req)

    # ---- configuration --------------------------------------------------------------------------------------

    def _transport(self) -> Tuple[Transport, str]:
        if self._transport_obj is not None:
            return self._transport_obj, self._api_key or "replay"
        fixtures = os.environ.get("FUSE_AI_GEMINI_FIXTURES")
        if fixtures:
            self._transport_obj = ReplayTransport.load(*[p for p in fixtures.split(os.pathsep) if p],
                                                       require_headers=("x-goog-api-key",))
            return self._transport_obj, "replay"
        key = self._api_key or os.environ.get("GEMINI_API_KEY")
        if not key:
            raise ProviderUnavailable("gemini: no GEMINI_API_KEY and no recorded fixtures (FUSE_AI_GEMINI_FIXTURES); "
                                      "use --mode replay against a recorded cache, or --mode mock")
        self._transport_obj = live_transport(BASE_URL)
        self._api_key = key
        return self._transport_obj, key

    def _send(self, method: str, path: str, body: Optional[Dict[str, Any]] = None) -> HttpResponse:
        transport, key = self._transport()
        req = HttpRequest(method, path, body, {"x-goog-api-key": key})
        return send_with_retries(transport, req, max_retries=self.max_retries, sleep=self.sleep, log=self.http_log)

    # ---- context caching ------------------------------------------------------------------------------------

    def _ensure_cache(self, model: str, prompt: prompts.Prompt, ttl_s: int) -> Tuple[str, int]:
        key = (model, prompt.sha256, prompt.context_sha256)
        if key in self._caches:
            return self._caches[key], 0
        resp = self._send("POST", f"/{API_VERSION}/cachedContents", build_cache_body(model, prompt, ttl_s))
        if not resp.ok:
            raise _api_error(resp, "create cachedContents")
        name = (resp.body or {}).get("name", "")
        if not str(name).startswith("cachedContents/"):
            raise SchemaViolation(f"cachedContents response has no name: {resp.body!r}")
        created = int(((resp.body or {}).get("usageMetadata") or {}).get("totalTokenCount", 0))
        self._caches[key] = name
        return name, created

    # ---- run ------------------------------------------------------------------------------------------------

    def run(self, req: Request) -> Result:
        policy.verify_clearance(req)  # the W0.8 guard ran and the request was not changed after it
        self._transport()             # unconfigured -> ProviderUnavailable before any validation noise
        model = pinned_model(req)
        if req.task in JSON_TASKS:
            return self._run_json(req, model)
        if req.task in IMAGE_TASKS:
            return self._run_image(req, model)
        raise ProviderNotImplemented(f"gemini {req.task.value} is not implemented in Wave 4 (W4.1-W4.4 cover "
                                     "TAG_IMAGE, INFER_MATERIAL, SUMMARISE_CODE, PORT_SCRIPT, EDIT_IMAGE, GEN_IMAGE)")

    def _prompt(self, req: Request) -> prompts.Prompt:
        pr = prompts.for_params(req.params, req.task)
        if pr is None:
            raise RecipeError(f"gemini {req.task.value} needs params.prompt (a Content/recipes/ai/prompts file)")
        return pr

    def _json_body(self, req: Request, model: str, pr: prompts.Prompt) -> Tuple[Dict[str, Any], Optional[str], int]:
        cached, created = None, 0
        if pr.context and req.params.get("context_cache"):
            cached, created = self._ensure_cache(model, pr, int(req.params.get("cache_ttl_s", 3600)))
        return build_generate_body(req, pr, cached_content=cached), cached, created

    def _json_result(self, req: Request, model: str, pr: prompts.Prompt, body: Any, *, batch: bool,
                     cached: Optional[str], created_tokens: int) -> Result:
        parts, usage, finish, served = parse_generate_response(model, body)
        text = "".join(p.get("text", "") for p in parts if isinstance(p, dict))
        value = prompts.parse_output(pr, text.encode("utf-8"))
        cost = token_cost(model, usage, batch=batch)
        if created_tokens:
            p_in = GEMINI_TOKEN_PRICES[model][0]
            cost.usd += created_tokens * p_in / 1e6
            cost.tokens_in += created_tokens
        outputs = [Output(OUTPUT_NAMES[req.task], "application/json", prompts.canonical_output(value))]
        if req.task is Task.PORT_SCRIPT:
            outputs.append(Output("port.lua", "text/x-lua", _lua_text(value)))
        meta = {"model_version": served, "finish_reason": finish, "usage": usage, "prompt": pr.id,
                "prompt_sha256": pr.sha256, "batch": batch, "licence": TEXT_LICENCE}
        if cached:
            meta["cached_content"] = cached
        return Result(outputs=outputs, meta=meta, cost=cost, provider_id=self.id, model_id=model)

    def _run_json(self, req: Request, model: str) -> Result:
        pr = self._prompt(req)
        body, cached, created = self._json_body(req, model, pr)
        resp = self._send("POST", f"/{API_VERSION}/models/{model}:generateContent", body)
        if not resp.ok:
            raise _api_error(resp, f"{model}:generateContent")
        return self._json_result(req, model, pr, resp.body, batch=False, cached=cached, created_tokens=created)

    def _run_image(self, req: Request, model: str) -> Result:
        body = build_image_body(req)
        resp = self._send("POST", f"/{API_VERSION}/models/{model}:generateContent", body)
        if not resp.ok:
            raise _api_error(resp, f"{model}:generateContent")
        parts, usage, finish, served = parse_generate_response(model, resp.body)
        images = []
        for p in parts:
            inline = p.get("inlineData") if isinstance(p, dict) else None
            if inline and str(inline.get("mimeType", "")).startswith("image/"):
                try:
                    images.append((inline["mimeType"], base64.b64decode(inline["data"], validate=True)))
                except (KeyError, ValueError):
                    raise SchemaViolation("image part has invalid base64 data") from None
        if not images:
            raise SchemaViolation(f"{req.task.value}: the response has no image part")
        stem = "edited" if req.task is Task.EDIT_IMAGE else "image"
        outputs = [Output(f"{stem}.png" if i == 0 else f"{stem}_{i}.png", mime, data)
                   for i, (mime, data) in enumerate(images)]
        size = str(req.params.get("size", "1K"))
        cost = image_cost(model, size, len(images))
        meta = {"model_version": served, "finish_reason": finish, "usage": usage, "watermark": "synthid",
                "licence": IMAGE_LICENCE, "size": size}
        return Result(outputs=outputs, meta=meta, cost=cost, provider_id=self.id, model_id=model)

    # ---- batch mode -----------------------------------------------------------------------------------------

    def run_batch(self, reqs: List[Request]) -> List[Result]:
        """One batchGenerateContent job for JSON-schema requests that share a model; 50 % price."""
        if not reqs:
            return []
        for r in reqs:
            policy.verify_clearance(r)
        self._transport()
        models = {pinned_model(r) for r in reqs}
        if len(models) != 1:
            raise RecipeError(f"a gemini batch needs one model, got {sorted(models)}")
        model = models.pop()
        items, ctx = [], {}
        for r in reqs:
            if r.task not in JSON_TASKS:
                raise RecipeError(f"batch mode covers JSON tasks only, not {r.task.value}")
            pr = self._prompt(r)
            body, cached, created = self._json_body(r, model, pr)
            items.append({"request": body, "metadata": {"key": r.recipe_hash}})
            ctx[r.recipe_hash] = (r, pr, cached, created)
        name_seed = hashlib.sha256("".join(sorted(ctx)).encode()).hexdigest()[:16]
        job = {"batch": {"displayName": f"fuse-ai-{name_seed}",
                         "inputConfig": {"requests": {"requests": items}}}}
        resp = self._send("POST", f"/{API_VERSION}/models/{model}:batchGenerateContent", job)
        if not resp.ok:
            raise _api_error(resp, f"{model}:batchGenerateContent")
        name = str((resp.body or {}).get("name", ""))
        if not name.startswith("batches/"):
            raise SchemaViolation(f"batchGenerateContent returned no batch name: {resp.body!r}")
        op = resp.body
        for _ in range(self.max_polls):
            state = ((op or {}).get("metadata") or {}).get("state")
            if state in TERMINAL_BATCH_STATES:
                break
            self.sleep(self.poll_interval_s)
            poll = self._send("GET", f"/{API_VERSION}/{name}")
            if not poll.ok:
                raise _api_error(poll, f"GET {name}")
            op = poll.body
        else:
            raise TransportError(f"{name} did not finish after {self.max_polls} polls")
        state = op["metadata"]["state"]
        if state != "BATCH_STATE_SUCCEEDED":
            raise TransportError(f"{name} ended in {state}")
        inlined = (((op["metadata"].get("output") or {}).get("inlinedResponses") or {}).get("inlinedResponses") or [])
        by_key: Dict[str, Any] = {}
        for item in inlined:
            key = (item.get("metadata") or {}).get("key")
            if "error" in item:
                raise TransportError(f"{name}: request {key} failed: {item['error']}")
            by_key[key] = item.get("response")
        results = []
        for r in reqs:
            if r.recipe_hash not in by_key:
                raise SchemaViolation(f"{name}: no response for request {r.recipe_hash}")
            _, pr, cached, created = ctx[r.recipe_hash]
            res = self._json_result(r, model, pr, by_key[r.recipe_hash], batch=True, cached=cached,
                                    created_tokens=created)
            res.meta["batch_name"] = name
            results.append(res)
        return results


def _lua_text(value: Any) -> bytes:
    lua = value.get("lua") if isinstance(value, dict) else None
    if not isinstance(lua, str):
        raise SchemaViolation("PORT_SCRIPT answer has no 'lua' string")
    return (lua.rstrip("\n") + "\n").encode("utf-8")


def fixture_json(obj: Any) -> str:
    return json.dumps(obj, sort_keys=True, indent=1, ensure_ascii=False)
