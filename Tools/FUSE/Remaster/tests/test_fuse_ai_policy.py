"""W0.8: the cloud opt-in guard (plan §0.1.5, §7 Wave 0 row W0.8) and the upload audit log.

The whole test run has the network blocked (tests/__init__.py); these tests also assert that no
socket was even attempted while a cloud recipe went all the way through the guard to the W0 stub.
"""

import socket
import unittest

import tests
from tests.fuse_ai_support import TempDirCase, make_recipe, project, recipe_dict

from fuse_ai import policy, providers
from fuse_ai.audit import AuditLog
from fuse_ai.cache import Mode
from fuse_ai.errors import CloudOptInRequired, PaidTierRequired, PolicyError, ProviderNotImplemented, RecipeError
from fuse_ai.providers import Request, Task
from fuse_ai.recipe import Recipe
from fuse_ai.runner import run_recipe


def cloud_recipe(origin="original", **kw):
    return make_recipe(task="TAG_IMAGE", provider="gemini", origin=origin, model={"id": "gemini-3.8-flash"}, **kw)


class NetworkIsBlocked(unittest.TestCase):
    def test_socket_is_blocked_for_the_whole_run(self):
        with self.assertRaises(tests.NetworkBlocked):
            socket.socket()
        with self.assertRaises(tests.NetworkBlocked):
            socket.create_connection(("example.com", 443))
        tests.NETWORK_ATTEMPTS.clear()


class Guard(TempDirCase):
    def run_cloud(self, proj, mode=Mode.RECORD, rec=None):
        return run_recipe(rec or cloud_recipe(), mode, cache_dir=self.cache_dir, project=proj, clock=self.clock)

    def tearDown(self):
        self.assertEqual(tests.NETWORK_ATTEMPTS, [], "fuse_ai attempted network access")
        super().tearDown()

    # ---- the W0.8 matrix ------------------------------------------------------------------------------------

    def test_original_to_cloud_without_opt_in_is_a_hard_error(self):
        with self.assertRaises(CloudOptInRequired) as cm:
            self.run_cloud(project("none"))
        self.assertIn("cloud_upload", str(cm.exception))

    def test_no_project_file_means_no_opt_in(self):
        with self.assertRaises(CloudOptInRequired):
            run_recipe(cloud_recipe(), Mode.RECORD, cache_dir=self.cache_dir, clock=self.clock)

    def test_opt_in_without_confirmation_is_an_error(self):
        with self.assertRaises(CloudOptInRequired) as cm:
            self.run_cloud(project("opt-in", confirmed=False, tier="paid"))
        self.assertIn("confirmation", str(cm.exception))

    def test_confirmation_for_another_game_does_not_count(self):
        p = project("opt-in", confirmed=True, tier="paid")
        p.confirmed = dict(p.confirmed, game_id="some-other-game")
        with self.assertRaises(CloudOptInRequired):
            self.run_cloud(p)

    def test_opt_in_confirmed_free_tier_needs_paid(self):
        with self.assertRaises(PaidTierRequired):
            self.run_cloud(project("opt-in", confirmed=True, tier="free"))
        with self.assertRaises(PaidTierRequired):
            self.run_cloud(project("opt-in", confirmed=True, tier=None))

    def test_everything_set_passes_the_guard_then_hits_the_w0_stub(self):
        with self.assertRaises(ProviderNotImplemented) as cm:
            self.run_cloud(project("opt-in", confirmed=True, tier="paid"))
        self.assertIn("not implemented in W0", str(cm.exception))
        # tearDown asserts no socket was attempted

    def test_derived_from_original_counts_as_original(self):
        rec = cloud_recipe(origin="derived")
        rec.inputs[0].derived_from_original = True
        with self.assertRaises(CloudOptInRequired):
            self.run_cloud(project("none"), rec=rec)

    def test_own_content_goes_to_the_cloud_without_opt_in(self):
        for origin in ("library", "generated", "authored"):
            with self.subTest(origin=origin), self.assertRaises(ProviderNotImplemented):
                self.run_cloud(project("none"), rec=cloud_recipe(origin=origin))
        rec = cloud_recipe(origin="derived")  # derived_from_original false
        with self.assertRaises(ProviderNotImplemented):
            self.run_cloud(project("none"), rec=rec)

    def test_any_original_input_trips_the_guard(self):
        d = recipe_dict(task="TAG_IMAGE", provider="gemini", origin="library")
        d["inputs"].append({"canonical_sha256": "9" * 64, "origin": "original"})
        with self.assertRaises(CloudOptInRequired):
            self.run_cloud(project("none"), rec=Recipe.from_json(d))

    def test_local_providers_and_mock_never_need_opt_in(self):
        rec = make_recipe(task="UPSCALE", provider="local-onnx", origin="original")
        with self.assertRaises(ProviderNotImplemented):  # passes the guard, then the W0 stub
            run_recipe(rec, Mode.RECORD, cache_dir=self.cache_dir, project=project("none"), clock=self.clock)
        out = run_recipe(make_recipe(provider="mock", origin="original"), Mode.RECORD, cache_dir=self.cache_dir,
                         project=project("none"), clock=self.clock)
        self.assertFalse(out.decision.cloud)
        # mock mode routes a cloud recipe to the local mock: nothing leaves the machine, so no opt-in needed
        out = self.run_cloud(project("none"), mode=Mode.MOCK)
        self.assertEqual(out.provider_id, "mock")

    def test_reference_provider_never_gets_originals(self):
        info = providers.info("reference")
        req = Request(task=Task.TAG_IMAGE, provider_id="reference", inputs=cloud_recipe().inputs, recipe_hash="x")
        with self.assertRaises(CloudOptInRequired):
            policy.check_request(req, info, project("opt-in", confirmed=True, tier="paid"))

    # ---- impossible to bypass ---------------------------------------------------------------------------------

    def test_guard_runs_before_the_cache_lookup(self):
        # Replay of a cloud recipe with original inputs: refused by the guard, not reported as a replay miss.
        with self.assertRaises(CloudOptInRequired):
            self.run_cloud(project("none"), mode=Mode.REPLAY)

    def test_cloud_provider_refuses_uncleared_requests(self):
        rec = cloud_recipe(origin="library")
        req = Request(task=rec.task, provider_id="gemini", inputs=rec.inputs, recipe_hash=rec.canonical_hash())
        with self.assertRaises(PolicyError):
            providers.get("gemini").run(req)  # never went through check_request
        policy.check_request(req, providers.info("gemini"), project("none"))
        req.inputs[0].origin = "original"  # tampering after clearance invalidates it
        with self.assertRaises(PolicyError):
            providers.get("gemini").run(req)

    def test_runner_has_no_skip_switch(self):
        import inspect

        from fuse_ai import runner

        sig = inspect.signature(runner.run_recipe)
        self.assertFalse([p for p in sig.parameters if "policy" in p or "guard" in p or "skip" in p])

    # ---- project file ----------------------------------------------------------------------------------------

    def test_project_file_validation(self):
        with self.assertRaises(RecipeError):
            policy.ProjectPolicy.from_json({"cloud_upload": "yes please"})
        with self.assertRaises(RecipeError):
            policy.ProjectPolicy.from_json({"cloud_upload": "opt-in", "cloud_tier": {"gemini": "gold"}})
        p = self.write_json("remaster.project.json", {"schema": "fuse.remaster-project/1", "game_id": "g",
                                                      "cloud_upload": "none"})
        self.assertEqual(policy.ProjectPolicy.load(p).cloud_upload, "none")


class UploadAudit(TempDirCase):
    def test_uploads_are_audited_before_the_call(self):
        proj_path = self.write_json("game/remaster.project.json", {})
        proj = project("opt-in", confirmed=True, tier="paid", path=proj_path)
        with self.assertRaises(ProviderNotImplemented):
            run_recipe(cloud_recipe(purpose="tag textures"), Mode.RECORD, cache_dir=self.cache_dir, project=proj,
                       clock=self.clock)
        log = AuditLog(self.tmp / "game" / "ai-audit.jsonl")
        uploads = log.entries("upload")
        self.assertEqual(len(uploads), 1)
        e = uploads[0]
        self.assertEqual(e["provider"], "gemini")
        self.assertEqual(e["model"], "gemini-3.8-flash")
        self.assertEqual(e["time"], "2026-11-05T12:00:00Z")
        self.assertEqual(e["purpose"], "tag textures")
        self.assertEqual(e["opt_in"], "opt-in")
        self.assertEqual(e["tier"], "paid")
        self.assertTrue(e["originals"])
        self.assertEqual(e["recipe_hash"], cloud_recipe(purpose="tag textures").canonical_hash())
        self.assertEqual(e["inputs"], [{"hash": "1" * 64, "origin": "original", "derived_from_original": False}])

    def test_refused_requests_upload_nothing_and_log_nothing(self):
        proj_path = self.write_json("game/remaster.project.json", {})
        with self.assertRaises(CloudOptInRequired):
            run_recipe(cloud_recipe(), Mode.RECORD, cache_dir=self.cache_dir, project=project("none", path=proj_path),
                       clock=self.clock)
        self.assertFalse((self.tmp / "game" / "ai-audit.jsonl").exists())

    def test_audit_log_is_append_only_and_deterministic(self):
        a = AuditLog(self.tmp / "a.jsonl", clock=self.clock)
        b = AuditLog(self.tmp / "b.jsonl", clock=self.clock)
        for log in (a, b):
            log.append("upload", provider="gemini", recipe_hash="h1")
            log.append("upload", provider="gemini", recipe_hash="h2")
        self.assertEqual((self.tmp / "a.jsonl").read_bytes(), (self.tmp / "b.jsonl").read_bytes())
        self.assertEqual([e["recipe_hash"] for e in a.entries()], ["h1", "h2"])


if __name__ == "__main__":
    unittest.main()
