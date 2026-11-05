"""Authoring script for the W4 replay fixtures (plan §3.3: CI replays, never calls the network).

    python Tools/FUSE/Remaster/tests/fixtures/author_fixtures.py          # (re)write the fixtures
    python Tools/FUSE/Remaster/tests/fixtures/author_fixtures.py --check  # exit 1 if a committed file differs

The *responses* below are hand-written stand-ins for what the Gemini API / llama-server return, in their
documented REST shapes. The *requests* are built by the provider code from the recipes, so every fixture is a
contract: if a prompt, a recipe or the request builder changes, the request no longer matches the recorded
one, ``test_ai_gemini`` fails, and the fixture has to be re-authored here (or re-recorded on the authoring
machine with a key, through ``fuse_ai.transport.RecordingTransport``).

Nothing here touches the network; ``FUSE_AI_OFFLINE`` is forced on.
"""

from __future__ import annotations

import base64
import json
import os
import sys
from pathlib import Path
from typing import Any, Dict, List, Tuple

HERE = Path(__file__).resolve().parent
REMASTER = HERE.parent.parent
if str(REMASTER) not in sys.path:
    sys.path.insert(0, str(REMASTER))
os.environ["FUSE_AI_OFFLINE"] = "1"

from fuse_ai import RECIPE_SCHEMA, gemini, imaging, local_llm, portflow, prompts  # noqa: E402
from fuse_ai.cache import Mode  # noqa: E402
from fuse_ai.policy import ProjectPolicy  # noqa: E402
from fuse_ai.recipe import Recipe  # noqa: E402
from fuse_ai.runner import Runner  # noqa: E402
from fuse_ai.transport import FIXTURE_SCHEMA  # noqa: E402

HTTP = HERE / "http"
PORT = HERE / "port"
V1 = gemini.API_VERSION

# ---- hand-written model answers -----------------------------------------------------------------------------------

TAGS_BRICK = {"tags": ["brick", "masonry", "wall", "red", "weathered", "mortar"], "material_class": "brick",
              "physical_category": "masonry/brick", "texture_role": "albedo", "is_text_or_logo": False,
              "confidence": 0.86}
TAGS_PLANK = {"tags": ["wood", "planks", "floor", "worn"], "material_class": "wood",
              "physical_category": "wood/planks", "texture_role": "albedo", "is_text_or_logo": False,
              "confidence": 0.8}
TAGS_SIGN = {"tags": ["sign", "metal", "painted", "text"], "material_class": "metal/painted",
             "physical_category": "metal/painted", "texture_role": "decal", "is_text_or_logo": True,
             "confidence": 0.7}
TAGS_BAD = {"tags": ["Stone Wall"], "material_class": "marble-ish", "physical_category": "stone",
            "texture_role": "albedo", "is_text_or_logo": "no", "confidence": 1.4, "mood": "gloomy"}
MATERIAL_BRICK = {"material_class": "brick", "physical_category": "masonry/brick", "base_color_srgb": [148, 74, 58],
                  "metallic": 0, "roughness": 0.82, "normal_strength": 1.1, "height_scale_m": 0.012,
                  "notes": "Mortar lines read as height; no baked lighting visible."}

SOURCE_SHA = None  # filled in from the fixture file


def spec_answer(source_sha: str) -> Dict[str, Any]:
    return {
        "module": "shapebase_impact",
        "summary": ("Impact damage for shape datablocks. When an object that still exists and is not destroyed "
                    "is hit (a physics collision or a scripted impact), it takes 'impact' damage proportional to "
                    "the hit speed divided by the datablock's minimum impact speed, applied at the object's "
                    "position offset back along the hit vector. Collisions and impacts differ only in their "
                    "damage multiplier."),
        "source_sha256": source_sha,
        "state": [
            {"name": "min_impact_speed", "type": "number", "description": "datablock: hit speed that deals 1x the multiplier"},
            {"name": "collision_mul", "type": "number", "description": "datablock: damage multiplier for collisions"},
            {"name": "impact_mul", "type": "number", "description": "datablock: damage multiplier for impacts"},
            {"name": "damage_state", "type": "string", "description": "per object, read only here: Enabled, Disabled or Destroyed"},
        ],
        "events": [
            {"name": "on_collision", "legacy_handler": "ShapeBaseData::onCollision",
             "params": ["other: the object collided with", "vec: this object's velocity at contact",
                        "len: the length of vec"],
             "guards": ["the object exists", "its damage_state is not Destroyed"],
             "effects": ["emit damage with target = this object, source = other, position = own position minus vec, "
                         "amount = len / min_impact_speed * collision_mul, kind = impact"]},
            {"name": "on_impact", "legacy_handler": "ShapeBaseData::onImpact",
             "params": ["other: the impacting object", "vec: the impact vector", "len: the impact speed"],
             "guards": ["the object exists", "its damage_state is not Destroyed"],
             "effects": ["emit damage with target = this object, source = other, position = own position minus vec, "
                         "amount = len / min_impact_speed * impact_mul, kind = impact"]},
        ],
        "timers": [],
        "dependencies": ["datablock damage dispatch (ShapeBaseData::damage)", "vector subtraction",
                         "object existence and damage-state queries"],
        "invariants": ["no damage is applied to a missing or destroyed object",
                       "damage grows linearly with the hit speed", "the damage kind is always impact",
                       "collision and impact use the same formula with different multipliers"],
        "engine_api": ["Entity.alive", "Entity.get_position", "Physics.get_velocity", "Remaster.get", "Remaster.emit"],
    }


def port_answer() -> Dict[str, Any]:
    return {"behaviour": "shapebase_impact",
            "lua": (PORT / "shapebase_impact.port.lua").read_text(encoding="utf-8"),
            "handlers": ["on_collision", "on_impact"],
            "uses_api": ["Entity.alive", "Entity.get_position", "Physics.get_velocity", "Remaster.get", "Remaster.emit",
                         "math.sqrt"],
            "notes": ["FUSE's collision callback has no velocity argument: vec is read with Physics.get_velocity(self).",
                      "Datablock fields become behaviour properties: min_impact_speed, collision_mul, impact_mul.",
                      "damage_state is read through the Remaster port bridge until a damage component exists."]}


# ---- REST response shapes -----------------------------------------------------------------------------------------


def gen_response(model: str, text: str, prompt_t: int, out_t: int, cached_t: int = 0, version: str = "") -> Dict[str, Any]:
    usage: Dict[str, Any] = {"promptTokenCount": prompt_t, "candidatesTokenCount": out_t,
                             "totalTokenCount": prompt_t + out_t}
    if cached_t:
        usage["cachedContentTokenCount"] = cached_t
    return {"candidates": [{"content": {"role": "model", "parts": [{"text": text}]}, "finishReason": "STOP",
                            "index": 0}],
            "usageMetadata": usage, "modelVersion": version or model, "responseId": "fixture"}


def image_response(model: str, png: bytes) -> Dict[str, Any]:
    return {"candidates": [{"content": {"role": "model", "parts": [
        {"inlineData": {"mimeType": "image/png", "data": base64.b64encode(png).decode("ascii")}}]},
        "finishReason": "STOP", "index": 0}],
        "usageMetadata": {"promptTokenCount": 40, "candidatesTokenCount": 1290, "totalTokenCount": 1330},
        "modelVersion": model}


def api_error(code: int, status: str, message: str) -> Dict[str, Any]:
    return {"error": {"code": code, "status": status, "message": message}}


def chat_response(model: str, content: str, prompt_t: int, out_t: int, prompt_ms: float, predicted_ms: float) -> Dict[str, Any]:
    return {"id": "chatcmpl-fixture", "object": "chat.completion", "created": 1762344000, "model": model,
            "choices": [{"index": 0, "message": {"role": "assistant", "content": content}, "finish_reason": "stop"}],
            "usage": {"prompt_tokens": prompt_t, "completion_tokens": out_t, "total_tokens": prompt_t + out_t},
            "timings": {"prompt_n": prompt_t, "prompt_ms": prompt_ms, "predicted_n": out_t, "predicted_ms": predicted_ms}}


def ex(method: str, path: str, body: Any, status: int, resp: Any, headers: Dict[str, str] | None = None) -> Dict[str, Any]:
    r: Dict[str, Any] = {"status": status, "body": resp}
    if headers:
        r["headers"] = headers
    return {"request": {"method": method, "path": path, "body": body}, "response": r}


# ---- inputs + recipes ---------------------------------------------------------------------------------------------


def png(seed: bytes) -> bytes:
    return imaging.encode_png(imaging.synthetic_image(seed, 8, 8))


INPUT_PNGS = {"brick_wall_01.png": b"brick_wall_01", "plank_floor_02.png": b"plank_floor_02",
              "shop_sign_03.png": b"shop_sign_03", "stone_wall_04.png": b"stone_wall_04",
              "gen_oak.png": b"gen_oak_result", "edited_sign.png": b"edited_sign"}


def tag_recipe(name: str, *, task: str = "TAG_IMAGE", origin: str = "library", batch: bool = False,
               provider: str | None = "gemini", model: str | None = "gemini-3.8-flash") -> Dict[str, Any]:
    d: Dict[str, Any] = {"schema": RECIPE_SCHEMA, "id": f"tex/{name}/{task.lower()}", "task": task, "seed": 7,
                         "params": {"batch": True} if batch else {},
                         "inputs": [{"path": f"../inputs/{name}.png", "name": name, "media_type": "image/png",
                                     "origin": origin}]}
    if provider:
        d["provider"] = provider
    if model:
        d["model"] = {"id": model}
    if task == "TAG_IMAGE":
        return prompts.bind(d, "tag_image", name=name, context="legacy texture, medieval town")
    return prompts.bind(d, "infer_material", name=name, tags="brick, masonry, wall")


def image_recipe(task: str) -> Dict[str, Any]:
    if task == "GEN_IMAGE":
        return {"schema": RECIPE_SCHEMA, "id": "lib/oak_planks/gen", "task": "GEN_IMAGE", "provider": "gemini",
                "model": {"id": "gemini-3.1-flash-image"}, "seed": 11,
                "params": {"prompt": "Weathered oak plank floor texture, hand-painted style, top-down, even lighting, tileable.",
                           "size": "1K", "aspect_ratio": "1:1"}, "inputs": []}
    return {"schema": RECIPE_SCHEMA, "id": "tex/shop_sign_03/edit", "task": "EDIT_IMAGE", "provider": "gemini",
            "model": {"id": "gemini-3.1-flash-image"}, "seed": 12,
            "params": {"prompt": "Repaint this sign in a clean hand-painted style; keep the layout, remove the grime.",
                       "size": "1K"},
            "inputs": [{"path": "../inputs/shop_sign_03.png", "name": "shop_sign_03", "media_type": "image/png",
                        "origin": "original"}]}


OPT_IN_PROJECT = {"schema": "fuse.remaster-project/1", "game_id": "fixture-game", "cloud_upload": "opt-in",
                  "cloud_upload_confirmed": {"by": "Fixture Author", "date": "2026-11-05", "game_id": "fixture-game"},
                  "cloud_tier": {"gemini": "paid"}}


def prepare(recipe_json: Dict[str, Any], base: Path, project: Dict[str, Any] | None = None):
    runner = Runner(Mode.RECORD, cache_dir=HERE / ".author-cache-unused",
                    project=ProjectPolicy.from_json(project) if project else None)
    return runner.prepare(Recipe.from_json(recipe_json), base)


def gen_request(recipe_json: Dict[str, Any], base: Path, project=None, cached: str | None = None) -> Tuple[str, Dict[str, Any]]:
    p = prepare(recipe_json, base, project)
    model = gemini.pinned_model(p.req)
    if p.req.task in gemini.IMAGE_TASKS:
        return f"/{V1}/models/{model}:generateContent", gemini.build_image_body(p.req)
    pr = prompts.for_params(p.req.params, p.req.task)
    return f"/{V1}/models/{model}:generateContent", gemini.build_generate_body(p.req, pr, cached_content=cached)


def write_inputs() -> Dict[str, bytes]:
    return {f"http/inputs/{n}": png(seed) for n, seed in INPUT_PNGS.items()}


def recipes() -> Dict[str, Dict[str, Any]]:
    src = (PORT / "shapebase_impact.tscript").read_bytes()
    ctx = (PORT / "shapebase_impact.callgraph.json").read_bytes()
    spec = prompts.canonical_output(spec_answer(_sha(src)))
    out = {
        "http/recipes/tag_brick.json": tag_recipe("brick_wall_01"),
        "http/recipes/material_brick.json": tag_recipe("brick_wall_01", task="INFER_MATERIAL"),
        "http/recipes/tag_bad.json": tag_recipe("stone_wall_04"),
        "http/recipes/tag_drift.json": tag_recipe("plank_floor_02", model="gemini-3.6-flash"),
        "http/recipes/batch_plank.json": tag_recipe("plank_floor_02", batch=True),
        "http/recipes/batch_sign.json": tag_recipe("shop_sign_03", batch=True),
        "http/recipes/gen_oak.json": image_recipe("GEN_IMAGE"),
        "http/recipes/edit_sign.json": image_recipe("EDIT_IMAGE"),
        # W4.6: no provider, original input -> routed (local-llm without the opt-in, gemini with it)
        "http/recipes/route_sign_tag.json": tag_recipe("shop_sign_03", origin="original", provider=None, model=None),
        "port/shapebase_impact.spec.recipe.json": portflow.summarise_recipe(
            "shapebase_impact", "shapebase_impact.tscript", src, "shapebase_impact.callgraph.json", ctx),
        "port/shapebase_impact.lua.recipe.json": portflow.port_recipe(
            "shapebase_impact", "shapebase_impact.spec.json", spec),
        # the same module treated as a user's original game script: no provider, routed by policy (W4.6)
        "port/original.spec.recipe.json": portflow.summarise_recipe(
            "shapebase_impact", "shapebase_impact.tscript", src, "shapebase_impact.callgraph.json", ctx,
            origin="original", provider=None, model=None),
        "port/original.lua.recipe.json": portflow.port_recipe(
            "shapebase_impact", "shapebase_impact.spec.json", spec, spec_from_original=True, provider=None,
            model=None),
    }
    out["port/original.spec.recipe.json"]["id"] = "port/original/shapebase_impact/spec"
    out["port/original.lua.recipe.json"]["id"] = "port/original/shapebase_impact/lua"
    return out


def _sha(b: bytes) -> str:
    import hashlib

    return hashlib.sha256(b).hexdigest()


def _doc(provider: str, description: str, exchanges: List[Dict[str, Any]]) -> Dict[str, Any]:
    return {"schema": FIXTURE_SCHEMA, "provider": provider, "description": description, "exchanges": exchanges}


def stage1() -> Dict[str, bytes]:
    """Inputs, recipes and the reviewed spec: the files the requests are built from."""
    files: Dict[str, bytes] = {}
    files.update(write_inputs())
    for k, v in recipes().items():
        files[k] = (json.dumps(v, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode("utf-8")
    src = (PORT / "shapebase_impact.tscript").read_bytes()
    files["port/shapebase_impact.spec.json"] = prompts.canonical_output(spec_answer(_sha(src)))
    return files


def build_all() -> Dict[str, bytes]:
    """Stage 1 must be on disk (requests resolve their inputs from it); returns stage 1 + the HTTP fixtures."""
    files = stage1()
    rec = recipes()
    rbase = HTTP / "recipes"
    src = (PORT / "shapebase_impact.tscript").read_bytes()
    spec_value = spec_answer(_sha(src))
    docs: Dict[str, Dict[str, Any]] = {}

    # W4.1 / W4.2: TAG_IMAGE with two retryable failures first; INFER_MATERIAL
    path, body = gen_request(rec["http/recipes/tag_brick.json"], rbase)
    path_m, body_m = gen_request(rec["http/recipes/material_brick.json"], rbase)
    docs["http/gemini_tag_material.json"] = _doc("gemini", "TAG_IMAGE (429 and 503 first, then 200) and INFER_MATERIAL", [
        ex("POST", path, body, 429, api_error(429, "RESOURCE_EXHAUSTED", "Quota exceeded; retry later."),
           {"retry-after": "2"}),
        ex("POST", path, body, 503, api_error(503, "UNAVAILABLE", "The model is overloaded.")),
        ex("POST", path, body, 200, gen_response("gemini-3.8-flash", json.dumps(TAGS_BRICK), 1290, 61)),
        ex("POST", path_m, body_m, 200, gen_response("gemini-3.8-flash", json.dumps(MATERIAL_BRICK), 1302, 88)),
    ])
    # W4.2: a schema-violating answer
    path, body = gen_request(rec["http/recipes/tag_bad.json"], rbase)
    docs["http/gemini_schema_violation.json"] = _doc("gemini", "TAG_IMAGE answer that violates the schema", [
        ex("POST", path, body, 200, gen_response("gemini-3.8-flash", json.dumps(TAGS_BAD), 1290, 70))])
    # W4.1: model drift (served version is not the pinned id)
    path, body = gen_request(rec["http/recipes/tag_drift.json"], rbase)
    docs["http/gemini_model_drift.json"] = _doc("gemini", "the API serves another model than the pinned one", [
        ex("POST", path, body, 200, gen_response("gemini-3.6-flash", json.dumps(TAGS_PLANK), 1290, 50,
                                                 version="gemini-3.8-flash"))])
    # W4.1: batch mode
    pb = prepare(rec["http/recipes/batch_plank.json"], rbase)
    ps = prepare(rec["http/recipes/batch_sign.json"], rbase)
    items = []
    for p in (pb, ps):
        pr = prompts.for_params(p.req.params, p.req.task)
        items.append({"request": gemini.build_generate_body(p.req, pr), "metadata": {"key": p.recipe_hash}})
    import hashlib

    seed = hashlib.sha256("".join(sorted([pb.recipe_hash, ps.recipe_hash])).encode()).hexdigest()[:16]
    job = {"batch": {"displayName": f"fuse-ai-{seed}", "inputConfig": {"requests": {"requests": items}}}}
    name = "batches/fixture-batch-1"
    done = {"name": name, "done": True, "metadata": {
        "@type": "type.googleapis.com/google.ai.generativelanguage.v1main.GenerateContentBatch",
        "state": "BATCH_STATE_SUCCEEDED", "output": {"inlinedResponses": {"inlinedResponses": [
            {"metadata": {"key": ps.recipe_hash},
             "response": gen_response("gemini-3.8-flash", json.dumps(TAGS_SIGN), 1290, 58)},
            {"metadata": {"key": pb.recipe_hash},
             "response": gen_response("gemini-3.8-flash", json.dumps(TAGS_PLANK), 1290, 52)}]}}}}
    docs["http/gemini_batch.json"] = _doc("gemini", "batchGenerateContent: pending, running, succeeded", [
        ex("POST", f"/{V1}/models/gemini-3.8-flash:batchGenerateContent", job, 200,
           {"name": name, "metadata": {"state": "BATCH_STATE_PENDING"}}),
        ex("GET", f"/{V1}/{name}", None, 200, {"name": name, "metadata": {"state": "BATCH_STATE_RUNNING"}}),
        ex("GET", f"/{V1}/{name}", None, 200, done),
    ])
    # W4.4: Nano Banana generate (own prompt) and edit (original input, with the opt-in)
    path_g, body_g = gen_request(rec["http/recipes/gen_oak.json"], rbase)
    path_e, body_e = gen_request(rec["http/recipes/edit_sign.json"], rbase, project=OPT_IN_PROJECT)
    docs["http/gemini_images.json"] = _doc("gemini", "GEN_IMAGE and EDIT_IMAGE (Nano Banana 2)", [
        ex("POST", path_g, body_g, 200, image_response("gemini-3.1-flash-image", png(INPUT_PNGS["gen_oak.png"]))),
        ex("POST", path_e, body_e, 200, image_response("gemini-3.1-flash-image", png(INPUT_PNGS["edited_sign.png"]))),
    ])
    # W4.3: summarise (context cache) and port (context cache)
    sp = prepare(rec["port/shapebase_impact.spec.recipe.json"], PORT)
    lp = prepare(rec["port/shapebase_impact.lua.recipe.json"], PORT)
    pr_s = prompts.for_params(sp.req.params, sp.req.task)
    pr_l = prompts.for_params(lp.req.params, lp.req.task)
    model = "gemini-3.1-pro-preview"
    c1, c2 = "cachedContents/fixture-summarise", "cachedContents/fixture-port"
    docs["http/gemini_port.json"] = _doc("gemini", "SUMMARISE_CODE then PORT_SCRIPT, each with a context cache", [
        ex("POST", f"/{V1}/cachedContents", gemini.build_cache_body(model, pr_s), 200,
           {"name": c1, "model": f"models/{model}", "usageMetadata": {"totalTokenCount": 905}}),
        ex("POST", f"/{V1}/models/{model}:generateContent", gemini.build_generate_body(sp.req, pr_s, cached_content=c1),
           200, gen_response(model, json.dumps(spec_value), 2310, 702, cached_t=905)),
        ex("POST", f"/{V1}/cachedContents", gemini.build_cache_body(model, pr_l), 200,
           {"name": c2, "model": f"models/{model}", "usageMetadata": {"totalTokenCount": 921}}),
        ex("POST", f"/{V1}/models/{model}:generateContent", gemini.build_generate_body(lp.req, pr_l, cached_content=c2),
           200, gen_response(model, json.dumps(port_answer()), 1890, 640, cached_t=921)),
    ])
    # W4.6: local-llm (llama-server) answers for the routed recipes
    rt = prepare(rec["http/recipes/route_sign_tag.json"], rbase)
    so = prepare(rec["port/original.spec.recipe.json"], PORT)
    lo = prepare(rec["port/original.lua.recipe.json"], PORT)
    chats = []
    for p, answer, pt, ot, pms, gms in ((rt, TAGS_SIGN, 980, 60, 410.0, 1650.0),
                                        (so, spec_value, 2850, 720, 2900.0, 41000.0),
                                        (lo, port_answer(), 2400, 650, 2500.0, 38000.0)):
        m = local_llm.pinned_model(p.req)
        pr = prompts.for_params(p.req.params, p.req.task)
        chats.append(ex("POST", "/v1/chat/completions", local_llm.build_chat_body(p.req, pr, m), 200,
                        chat_response(m, json.dumps(answer), pt, ot, pms, gms)))
    docs["http/local_llm.json"] = _doc("local-llm", "llama-server /v1/chat/completions for the routed recipes", chats)

    for k, d in docs.items():
        files[k] = (json.dumps(d, indent=1, sort_keys=True, ensure_ascii=False) + "\n").encode("utf-8")
    return files


def same_file(p: Path, data: bytes) -> bool:
    """Byte-equal; text files also compare equal across a CRLF checkout (binary PNGs never are normalised)."""
    if not p.is_file():
        return False
    raw = p.read_bytes()
    return raw == data or (p.suffix != ".png" and raw.replace(b"\r\n", b"\n") == data)


def main(argv: List[str]) -> int:
    check = "--check" in argv
    first = stage1()
    for rel, data in sorted(first.items()):
        p = HERE / rel
        same = same_file(p, data)
        if check and not same:
            print(f"out of date: {rel}")
            return 1
        if not check and not same:
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
    files = build_all()
    bad = []
    for rel, data in sorted(files.items()):
        p = HERE / rel
        if check:
            if not same_file(p, data):
                bad.append(rel)
        else:
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
    if check and bad:
        print("out of date: " + ", ".join(bad))
        return 1
    print(("checked " if check else "wrote ") + f"{len(files)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
