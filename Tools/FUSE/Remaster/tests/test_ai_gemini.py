"""Wave 4 gates: the gemini provider (W4.1) against hand-written recorded fixtures, prompts + JSON schemas (W4.2),
Nano Banana + SynthID in the lock (W4.4), cost report + budget preflight (W4.5), local-llm + routing (W4.6).

Every provider call goes to a ReplayTransport over tests/fixtures/http/*.json; the network is blocked for the
whole run (tests/__init__.py) and FUSE_AI_OFFLINE refuses live transports.
"""

import contextlib
import io
import json
import math
from pathlib import Path

from tests import NETWORK_ATTEMPTS
from tests.fuse_ai_support import TempDirCase

from fuse_ai import gemini, jsonschema, local_llm, lockrec, policy, prompts, providers, routing
from fuse_ai.__main__ import main as cli_main
from fuse_ai.budget import Budget, BudgetConfig
from fuse_ai.cache import Mode
from fuse_ai.errors import (BudgetExceeded, CloudOptInRequired, PolicyError, ProviderUnavailable, RecipeError,
                            ReplayMiss, SchemaViolation, TransportError)
from fuse_ai.providers import Output, Request, Result, Task
from fuse_ai.recipe import Recipe, load
from fuse_ai.runner import Runner
from fuse_ai.transport import ReplayTransport, live_transport

FX = Path(__file__).resolve().parent / "fixtures"
HTTP = FX / "http"
RECIPES = HTTP / "recipes"

OPT_IN = {"schema": "fuse.remaster-project/1", "game_id": "fixture-game", "cloud_upload": "opt-in",
          "cloud_upload_confirmed": {"by": "Fixture Author", "date": "2026-11-05", "game_id": "fixture-game"},
          "cloud_tier": {"gemini": "paid"}}


class Sleeps(list):
    def __call__(self, s):
        self.append(s)


class ProviderCase(TempDirCase):
    """Swaps in fixture-backed gemini / local-llm providers and restores the defaults afterwards."""

    def setUp(self):
        super().setUp()
        self._saved = {pid: providers.get(pid) for pid in ("gemini", "local-llm")}
        self.sleeps = Sleeps()
        self.gem_tx = ReplayTransport([], name="no-gemini-calls", require_headers=("x-goog-api-key",))
        self.llm_tx = ReplayTransport([], name="no-local-llm-calls")
        self.install()

    def tearDown(self):
        for p in self._saved.values():
            providers.register(p, replace=True)
        self.assertEqual(NETWORK_ATTEMPTS, [], "a test tried to open a socket")
        super().tearDown()

    def install(self, gemini_fixture=None, llm_fixture=None):
        """Fresh transports: the named fixtures, or empty ones (any call is then a contract failure)."""
        self.gem_tx = (ReplayTransport.load(HTTP / gemini_fixture, require_headers=("x-goog-api-key",))
                       if gemini_fixture else ReplayTransport([], name="no-gemini-calls"))
        self.llm_tx = ReplayTransport.load(HTTP / llm_fixture) if llm_fixture else ReplayTransport([], name="none")
        self.gem = gemini.GeminiProvider(self.gem_tx, sleep=self.sleeps, poll_interval_s=5.0)
        self.llm = local_llm.LocalLLMProvider(self.llm_tx, sleep=self.sleeps)
        providers.register(self.gem, replace=True)
        providers.register(self.llm, replace=True)

    def runner(self, mode=Mode.RECORD, project=None, budget=None, cache=None):
        proj = policy.ProjectPolicy.from_json(project, self.tmp / "remaster.project.json") if project else None
        return Runner(mode, cache_dir=cache or self.cache_dir, project=proj, budget=budget, clock=self.clock)

    def run_file(self, name, mode=Mode.RECORD, base=RECIPES, **kw):
        return self.runner(mode, **kw).run(load(base / name), base_dir=base)


class Fixtures(TempDirCase):
    def test_fixtures_are_up_to_date(self):
        """The recorded requests are what the provider builds today (else: re-author the fixtures)."""
        import importlib.util

        spec = importlib.util.spec_from_file_location("author_fixtures", FX / "author_fixtures.py")
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(mod.main(["--check"]), 0, out.getvalue())

    def test_no_credentials_in_fixtures(self):
        for p in HTTP.glob("*.json"):
            text = p.read_text(encoding="utf-8")
            self.assertNotIn("x-goog-api-key", text, p)
            self.assertNotIn("AIza", text, p)  # the Google API key prefix
            self.assertNotIn("authorization", text.lower(), p)


class Prompts(TempDirCase):
    """W4.2: prompt files with JSON-schema-constrained outputs."""

    def test_all_prompts_load_and_schemas_are_supported(self):
        ids = sorted(p.name.split(".")[0] for p in prompts.prompts_dir().glob("*.prompt.json"))
        self.assertEqual(ids, ["infer_material", "port_script", "summarise_code", "tag_image"])
        for pid in ids:
            pr = prompts.load(pid)
            self.assertIsNotNone(pr.output_schema)
            self.assertEqual(jsonschema.check_schema(pr.output_schema), [])

    def test_bound_prompt_sha_is_pinned(self):
        d = load(RECIPES / "tag_brick.json").to_json()
        self.assertEqual(prompts.for_params(d["params"], Task.TAG_IMAGE).id, "tag_image")
        d["params"]["prompt_sha256"] = "0" * 64
        with self.assertRaises(RecipeError):
            prompts.for_params(d["params"], Task.TAG_IMAGE)
        with self.assertRaises(RecipeError):
            prompts.for_params(d["params"] | {"prompt": "port_script"}, Task.TAG_IMAGE)

    def test_schema_validation(self):
        pr = prompts.load("tag_image")
        good = {"tags": ["brick"], "material_class": "brick", "physical_category": "masonry/brick",
                "texture_role": "albedo", "is_text_or_logo": False, "confidence": 0.5}
        self.assertEqual(prompts.parse_output(pr, json.dumps(good).encode()), good)
        self.assertEqual(prompts.parse_output(pr, ("```json\n" + json.dumps(good) + "\n```").encode()), good)
        for bad in (dict(good, material_class="marble-ish"), dict(good, confidence=1.5), dict(good, extra=1),
                    dict(good, tags=[]), dict(good, tags=["Has Space"]), dict(good, is_text_or_logo="no"),
                    {k: v for k, v in good.items() if k != "texture_role"}):
            with self.subTest(bad=bad), self.assertRaises(SchemaViolation):
                prompts.parse_output(pr, json.dumps(bad).encode())
        with self.assertRaises(SchemaViolation):
            prompts.parse_output(pr, b"Sure! Here are the tags: brick")
        self.assertIn("unsupported keyword", jsonschema.check_schema({"type": "object", "oneOf": []})[0])


class GeminiContract(ProviderCase):
    """W4.1: REST shape, pinned ids, retries, cost accounting - against the recorded fixtures (replay)."""

    def test_request_shape(self):
        doc = json.loads((HTTP / "gemini_tag_material.json").read_text(encoding="utf-8"))
        req = doc["exchanges"][0]["request"]
        self.assertEqual(req["method"], "POST")
        self.assertEqual(req["path"], "/v1beta/models/gemini-3.8-flash:generateContent")
        body = req["body"]
        self.assertEqual(sorted(body), ["contents", "generationConfig", "systemInstruction"])
        parts = body["contents"][0]["parts"]
        self.assertEqual(body["contents"][0]["role"], "user")
        self.assertIn("brick_wall_01", parts[0]["text"])
        self.assertEqual(parts[1]["inlineData"]["mimeType"], "image/png")
        gen = body["generationConfig"]
        self.assertEqual((gen["temperature"], gen["seed"], gen["candidateCount"]), (0, 7, 1))
        self.assertEqual(gen["responseMimeType"], "application/json")
        self.assertEqual(gen["responseJsonSchema"], prompts.load("tag_image").output_schema)
        self.assertEqual(gen["maxOutputTokens"], 512)

    def test_tag_image_with_retries_then_replay(self):
        self.install("gemini_tag_material.json")
        out = self.run_file("tag_brick.json")
        self.assertEqual(out.provider_id, "gemini")
        self.assertEqual(json.loads(out.result.primary.data)["material_class"], "brick")
        self.assertEqual(self.sleeps, [2.0, 2.0])  # Retry-After 2 s, then the 2nd backoff step (1 s * 2)
        self.assertEqual([e["status"] for e in self.gem.http_log], [429, 503, 200])
        self.assertAlmostEqual(out.result.cost.usd, (1290 * 0.75 + 61 * 3.75) / 1e6, places=12)
        self.assertEqual((out.result.cost.tokens_in, out.result.cost.tokens_out), (1290, 61))
        self.assertEqual(out.result.meta["model_version"], "gemini-3.8-flash")
        self.assertEqual(out.recipe.output["distribution"], "ship")
        # replay: no transport at all, same bytes, still schema-checked
        self.install()
        again = self.run_file("tag_brick.json", mode=Mode.REPLAY)
        self.assertTrue(again.result.from_cache)
        self.assertEqual(again.result.primary.data, out.result.primary.data)
        self.assertEqual(self.gem_tx.calls, [])

    def test_infer_material(self):
        self.install("gemini_tag_material.json")
        self.run_file("tag_brick.json")
        out = self.run_file("material_brick.json")
        mat = json.loads(out.result.primary.data)
        self.assertEqual(mat["base_color_srgb"], [148, 74, 58])
        self.assertEqual(mat["metallic"], 0)
        self.assertEqual(self.gem_tx.unused(), [])

    def test_retries_exhausted_and_hard_errors(self):
        body = {"contents": []}
        ex429 = {"request": {"method": "POST", "path": "/x", "body": body}, "response": {"status": 429, "body": {}}}
        tx = ReplayTransport([ex429] * 5)
        from fuse_ai.transport import HttpRequest, send_with_retries

        sleeps = Sleeps()
        with self.assertRaises(TransportError):
            send_with_retries(tx, HttpRequest("POST", "/x", body), max_retries=4, sleep=sleeps)
        self.assertEqual(sleeps, [1.0, 2.0, 4.0, 8.0])
        ex400 = {"request": {"method": "POST", "path": "/x", "body": body}, "response": {"status": 400, "body": {}}}
        tx = ReplayTransport([ex400, ex400])
        self.assertEqual(send_with_retries(tx, HttpRequest("POST", "/x", body), sleep=sleeps).status, 400)
        self.assertEqual(len(tx.calls), 1)  # a 400 is not retried

    def test_request_drift_is_a_contract_failure(self):
        self.install("gemini_tag_material.json")
        d = load(RECIPES / "tag_brick.json").to_json()
        d["seed"] = 8  # a different request than the recorded one
        with self.assertRaises(TransportError) as cm:
            self.runner().run(Recipe.from_json(d), base_dir=RECIPES)
        self.assertIn("generationConfig.seed", str(cm.exception))

    def test_model_drift_and_pinning(self):
        self.install("gemini_model_drift.json")
        with self.assertRaises(TransportError) as cm:
            self.run_file("tag_drift.json")
        self.assertIn("model drift", str(cm.exception))
        d = load(RECIPES / "tag_brick.json").to_json()
        d["model"] = {"id": "gemini-flash-latest"}
        with self.assertRaises(RecipeError):
            self.runner().run(Recipe.from_json(d), base_dir=RECIPES)

    def test_schema_violation_live_is_not_cached(self):
        self.install("gemini_schema_violation.json")
        with self.assertRaises(SchemaViolation):
            self.run_file("tag_bad.json")
        with self.assertRaises(ReplayMiss):
            self.run_file("tag_bad.json", mode=Mode.REPLAY)

    def test_schema_violation_replayed_is_rejected(self):
        runner = self.runner(Mode.REPLAY)
        p = runner.prepare(load(RECIPES / "tag_brick.json"), RECIPES)
        bad = Result([Output("tags.json", "application/json", b'{"tags": ["brick"], "material_class": "lava"}')],
                     {"prompt": "tag_image"}, provider_id="gemini", model_id="gemini-3.8-flash")
        runner.cache.store(p.recipe_hash, p.recipe.hash_payload(), bad)
        with self.assertRaises(SchemaViolation):
            runner.run(load(RECIPES / "tag_brick.json"), base_dir=RECIPES)

    def test_batch_mode(self):
        self.install("gemini_batch.json")
        runner = self.runner()
        outs = runner.run_many([(load(RECIPES / n), RECIPES) for n in ("batch_plank.json", "batch_sign.json")])
        self.assertEqual([json.loads(o.result.primary.data)["material_class"] for o in outs], ["wood", "metal/painted"])
        self.assertEqual([(c.method, c.path.rsplit(":", 1)[-1]) for c in self.gem_tx.calls],
                         [("POST", "batchGenerateContent"), ("GET", "/v1beta/batches/fixture-batch-1"),
                          ("GET", "/v1beta/batches/fixture-batch-1")])
        self.assertEqual(self.sleeps, [5.0, 5.0])  # two polls
        self.assertAlmostEqual(outs[0].result.cost.usd, (1290 * 0.75 + 52 * 3.75) / 1e6 * 0.5, places=12)
        self.assertTrue(all(o.result.meta["batch"] for o in outs))
        self.assertEqual(self.gem_tx.unused(), [])

    def test_cost_accounting(self):
        c = gemini.token_cost("gemini-3.1-pro-preview", {"promptTokenCount": 2310, "cachedContentTokenCount": 905,
                                                        "candidatesTokenCount": 702, "thoughtsTokenCount": 100})
        self.assertAlmostEqual(c.usd, ((2310 - 905) * 2.0 + 905 * 2.0 * 0.2 + 802 * 12.0) / 1e6, places=12)
        long = gemini.token_cost("gemini-3.1-pro-preview", {"promptTokenCount": 250_000, "candidatesTokenCount": 10})
        self.assertAlmostEqual(long.usd, (250_000 * 4.0 + 10 * 18.0) / 1e6, places=12)  # >200k tier
        self.assertAlmostEqual(gemini.image_cost("gemini-3.1-flash-image", "2K", 3).usd, 0.303)
        self.assertAlmostEqual(gemini.image_cost("gemini-3-pro-image", "4K", 1, batch=True).usd, 0.12)
        with self.assertRaises(RecipeError):
            gemini.image_cost("gemini-3-pro-image", "0.5K", 1)

    def test_guard_still_applies(self):
        """An original input without the opt-in never reaches the transport, even with a fixture configured."""
        self.install("gemini_tag_material.json")
        d = load(RECIPES / "tag_brick.json").to_json()
        d["inputs"][0]["origin"] = "original"
        with self.assertRaises(CloudOptInRequired):
            self.runner().run(Recipe.from_json(d), base_dir=RECIPES)
        rec = load(RECIPES / "tag_brick.json")
        req = Request(task=rec.task, provider_id="gemini", inputs=rec.inputs, params=rec.params,
                      model=rec.model, recipe_hash="x")
        with self.assertRaises(PolicyError):
            self.gem.run(req)  # direct call without check_request
        self.assertEqual(self.gem_tx.calls, [])

    def test_unconfigured_and_offline(self):
        providers.register(gemini.GeminiProvider(), replace=True)
        with self.assertRaises(ProviderUnavailable):
            self.run_file("tag_brick.json")
        with self.assertRaises(ProviderUnavailable):
            live_transport("https://generativelanguage.googleapis.com")  # FUSE_AI_OFFLINE=1 in tests


class NanoBanana(ProviderCase):
    """W4.4: EDIT_IMAGE / GEN_IMAGE; the lock record carries watermark: synthid; original input -> recipe-only."""

    def test_generate_from_our_prompt_is_shippable(self):
        self.install("gemini_images.json")
        out = self.run_file("gen_oak.json")
        self.assertTrue(out.result.primary.data.startswith(b"\x89PNG"))
        self.assertEqual(out.result.meta["watermark"], "synthid")
        self.assertEqual(out.recipe.output["watermark"], "synthid")
        self.assertEqual(out.recipe.output["distribution"], "ship")
        self.assertAlmostEqual(out.result.cost.usd, 0.067)  # Nano Banana 2 at 1K
        lock = lockrec.lock_record(out, "lib/oak_planks", files_dir="Content/library/oak")
        self.assertEqual(lock["watermark"], "synthid")
        self.assertEqual(lock["licence"], "LicenseRef-AI-Gemini-Image")
        self.assertEqual(lock["origin"], "ai")
        self.assertEqual(lock["distribution"], "ship")
        self.assertEqual(lock["files"][0]["path"], "Content/library/oak/image.png")
        self.assertNotIn("recipe_only", lock)

    def test_edit_of_an_original_is_recipe_only(self):
        self.install("gemini_images.json")
        with self.assertRaises(CloudOptInRequired):
            self.run_file("edit_sign.json")  # no opt-in: refused before the transport
        self.assertEqual(self.gem_tx.calls, [])
        self.run_file("gen_oak.json")  # consume the first exchange
        out = self.run_file("edit_sign.json", project=OPT_IN)
        self.assertEqual(out.recipe.output["distribution"], "recipe-only")
        self.assertEqual(out.recipe.output["watermark"], "synthid")
        lock = lockrec.lock_record(out, "tex/shop_sign_03/edit", files_dir="x", game_id="fixture-game", tier="paid")
        self.assertEqual(lock["watermark"], "synthid")
        self.assertTrue(lock["recipe_only"])
        self.assertEqual(lock["files"], [])  # nothing shippable: the patch ships the recipe
        self.assertEqual(lock["distribution"], "recipe-only")
        self.assertEqual(lock["game_id"], "fixture-game")
        self.assertEqual(len(lock["cloud_uploads"]), 1)  # the audited upload of the original
        audit = (self.tmp / "ai-audit.jsonl").read_text(encoding="utf-8").splitlines()
        self.assertEqual(json.loads(audit[-1])["originals"], True)


class CostAndBudget(ProviderCase):
    """W4.5: fuse_ai cost + end-to-end budget enforcement: a breach is refused before any call."""

    def cli(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cli_main([str(a) for a in argv])
        return code, out.getvalue(), err.getvalue()

    def test_plan_breach_refused_before_any_call(self):
        self.install("gemini_tag_material.json")
        budget = self.write_json("ai-budget.json", {"schema": "fuse.ai-budget/1", "defaults": {"per_run_usd": 0.004}})
        recipes = [RECIPES / "tag_brick.json", RECIPES / "material_brick.json"]
        ests = [self.gem.estimate(self.runner().prepare(load(r), RECIPES).req).usd for r in recipes]
        self.assertTrue(all(e < 0.004 for e in ests) and sum(ests) > 0.004, ests)  # each fits, together they do not
        code, out, err = self.cli("cost", *recipes, "--budget", budget, "--cache", self.cache_dir, "--json")
        self.assertEqual(code, 4, err)
        plan = json.loads(out)["plan"]
        self.assertFalse(plan["fits"])
        self.assertAlmostEqual(plan["total_usd"], sum(ests), places=5)
        code, _, err = self.cli("run", *recipes, "--mode", "record", "--budget", budget, "--cache", self.cache_dir)
        self.assertEqual(code, 4, err)
        self.assertIn("before any call", err)
        self.assertEqual(self.gem_tx.calls, [])
        self.assertFalse((self.cache_dir / "ai-ledger.json").exists())

    def test_spend_report_after_runs(self):
        self.install("gemini_tag_material.json", "local_llm.json")
        code, out, err = self.cli("run", RECIPES / "tag_brick.json", "--mode", "record", "--cache", self.cache_dir)
        self.assertEqual(code, 0, err)
        code, out, err = self.cli("run", RECIPES / "route_sign_tag.json", "--mode", "record", "--cache", self.cache_dir)
        self.assertEqual(code, 0, err)
        self.assertEqual(json.loads(out)["provider"], "local-llm")
        code, out, err = self.cli("cost", "--cache", self.cache_dir, "--json", "--month", "2026-11")
        rep = json.loads(out)
        spent = rep["spent"]["providers"]
        if not spent:  # the CLI ledger uses the wall clock month
            code, out, err = self.cli("cost", "--cache", self.cache_dir, "--json")
            spent = json.loads(out)["spent"]["providers"]
        self.assertAlmostEqual(spent["gemini"]["usd"], (1290 * 0.75 + 61 * 3.75) / 1e6, places=6)
        self.assertEqual(spent["gemini"]["requests"], 1)
        self.assertEqual(spent["local-llm"]["usd"], 0.0)
        self.assertAlmostEqual(spent["local-llm"]["gpu_hours"], (410.0 + 1650.0) / 1000 / 3600, places=6)
        self.assertGreater(spent["local-llm"]["aud_electricity"], 0.0)
        self.assertIn("monthly_left_usd", spent["gemini"])
        code, text, _ = self.cli("cost", "--cache", self.cache_dir)
        self.assertEqual(code, 0)
        self.assertIn("gemini", text)

    def test_cached_recipes_cost_nothing_in_the_plan(self):
        self.install("gemini_tag_material.json")
        self.run_file("tag_brick.json")
        from fuse_ai import costreport

        runner = self.runner()
        rep = costreport.report(ledger=None, budget_path=None, runner=runner,
                                items=[(load(RECIPES / "tag_brick.json"), RECIPES),
                                       (load(RECIPES / "material_brick.json"), RECIPES)])
        rows = {r["id"]: r for r in rep["plan"]["recipes"]}
        self.assertTrue(rows["tex/brick_wall_01/tag_image"]["cached"])
        self.assertEqual(rows["tex/brick_wall_01/tag_image"]["usd"], 0.0)
        self.assertFalse(rows["tex/brick_wall_01/infer_material"]["cached"])
        self.assertTrue(rep["plan"]["fits"])

    def test_check_plan_caps(self):
        b = Budget(BudgetConfig.from_json({"providers": {"gemini": {"monthly_usd": 0.001}}}), clock=self.clock)
        from fuse_ai.providers import Cost

        with self.assertRaises(BudgetExceeded):
            b.check_plan([(providers.info("gemini"), Cost(usd=0.0006)), (providers.info("gemini"), Cost(usd=0.0006))])
        b = Budget(BudgetConfig.from_json({"defaults": {"local_gpu_hours_per_run": 1}}), clock=self.clock)
        with self.assertRaises(BudgetExceeded):
            b.check_plan([(providers.info("local-llm"), Cost(seconds=2400, gpu="rtx3090"))] * 2)


class LocalLLMAndRouting(ProviderCase):
    """W4.6: llama-server client with the same prompts/schemas; originals without opt-in go to local-llm."""

    def test_original_routes_to_local_llm_never_gemini(self):
        self.install(llm_fixture="local_llm.json")  # gemini keeps its empty transport: any call would fail
        out = self.run_file("route_sign_tag.json")  # project: none (default)
        self.assertEqual(out.provider_id, "local-llm")
        self.assertEqual(out.recipe.provider, "local-llm")
        self.assertEqual(out.recipe.model["id"], "qwen3-vl-8b-instruct-q8_0")
        self.assertEqual(self.gem_tx.calls, [])
        tags = prompts.parse_output(prompts.load("tag_image"), out.result.primary.data)  # the same schema
        self.assertTrue(tags["is_text_or_logo"])
        self.assertEqual(out.result.cost.usd, 0.0)
        self.assertAlmostEqual(out.result.cost.seconds, 2.06)
        self.assertEqual(out.result.cost.gpu, "rtx3090")
        self.assertEqual(out.recipe.output["distribution"], "recipe-only")
        req = self.llm_tx.calls[0]
        self.assertEqual(req.path, "/v1/chat/completions")
        self.assertEqual(req.body["response_format"]["json_schema"]["schema"], prompts.load("tag_image").output_schema)
        self.assertEqual(req.body["temperature"], 0)
        self.assertTrue(req.body["messages"][1]["content"][1]["image_url"]["url"].startswith("data:image/png;base64,"))
        again = self.run_file("route_sign_tag.json", mode=Mode.REPLAY)  # replayed answer parses
        self.assertTrue(again.result.from_cache)

    def test_routing_table(self):
        rec = load(RECIPES / "route_sign_tag.json")
        none = policy.ProjectPolicy()
        opt = policy.ProjectPolicy.from_json(OPT_IN)
        free = policy.ProjectPolicy.from_json(dict(OPT_IN, cloud_tier={"gemini": "free"}))
        self.assertEqual(routing.route(Task.TAG_IMAGE, rec.inputs, none), "local-llm")
        self.assertEqual(routing.route(Task.TAG_IMAGE, rec.inputs, free), "local-llm")
        self.assertEqual(routing.route(Task.TAG_IMAGE, rec.inputs, opt), "gemini")
        self.assertEqual(routing.route(Task.PORT_SCRIPT, rec.inputs, none), "local-llm")
        self.assertEqual(routing.route(Task.EDIT_IMAGE, rec.inputs, none), "local-torch")
        own = load(RECIPES / "tag_brick.json").inputs
        self.assertEqual(routing.route(Task.TAG_IMAGE, own, none), "gemini")  # our own content goes to the agent
        self.assertEqual(routing.route(Task.UPSCALE, rec.inputs, none), "local-onnx")

    def test_explicit_gemini_is_refused_not_rerouted(self):
        d = load(RECIPES / "route_sign_tag.json").to_json()
        d["provider"], d["model"] = "gemini", {"id": "gemini-3.8-flash"}
        with self.assertRaises(CloudOptInRequired):
            self.runner().run(Recipe.from_json(d), base_dir=RECIPES)
        self.assertEqual(self.llm_tx.calls, [])

    def test_loopback_only_and_pins(self):
        bad = local_llm.LocalLLMProvider(self.llm_tx, base_url="http://192.168.1.20:8080")
        providers.register(bad, replace=True)
        with self.assertRaises(PolicyError):
            self.run_file("route_sign_tag.json")
        with self.assertRaises(PolicyError):
            live_transport("http://10.0.0.2:8080", local_only=True)
        d = load(RECIPES / "route_sign_tag.json").to_json()
        d["provider"], d["model"] = "local-llm", {"id": "llama-3.1-8b"}
        providers.register(self.llm, replace=True)
        with self.assertRaises(RecipeError):
            self.runner().run(Recipe.from_json(d), base_dir=RECIPES)
        self.assertEqual(local_llm.pinned_model(Request(task=Task.PORT_SCRIPT, provider_id="local-llm")),
                         "qwen3-coder-30b-a3b-instruct-q4_k_m")

    def test_vram_estimates_fit_the_3090(self):
        for task in (Task.TAG_IMAGE, Task.PORT_SCRIPT):
            v = self.llm.vram_mb(Request(task=task, provider_id="local-llm"))
            self.assertLess(v, 22 * 1024)
            self.assertGreater(v, 0)


if __name__ == "__main__":
    import unittest

    unittest.main()
