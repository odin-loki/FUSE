"""W0.5 exit criteria: replay miss fails; record -> replay round trip (plan §3.3 "Cache")."""

import json
import unittest

from tests.fuse_ai_support import TempDirCase, make_recipe, recipe_dict, tiny_png

from fuse_ai import recipe as R
from fuse_ai.cache import Mode, ResultCache
from fuse_ai.errors import CacheCorrupt, ProviderNotImplemented, ReplayMiss
from fuse_ai.runner import run_recipe


class CacheModes(TempDirCase):
    def test_replay_miss_is_a_hard_error(self):
        with self.assertRaises(ReplayMiss) as cm:
            run_recipe(make_recipe(), Mode.REPLAY, cache_dir=self.cache_dir, clock=self.clock)
        self.assertIn(make_recipe().canonical_hash(), str(cm.exception))
        self.assertFalse((self.cache_dir / "entries").exists())

    def test_record_then_replay_round_trip(self):
        rec = make_recipe()  # provider "mock": record mode calls it and stores the result
        first = run_recipe(rec, Mode.RECORD, cache_dir=self.cache_dir, clock=self.clock)
        self.assertFalse(first.result.from_cache)
        self.assertTrue((self.cache_dir / "entries" / f"{first.recipe_hash}.json").is_file())

        again = run_recipe(rec, Mode.REPLAY, cache_dir=self.cache_dir, clock=self.clock)
        self.assertTrue(again.result.from_cache)
        self.assertEqual(again.recipe_hash, first.recipe_hash)
        self.assertEqual([(o.name, o.data) for o in again.result.outputs],
                         [(o.name, o.data) for o in first.result.outputs])

        # The updated recipe (output + cost written back) still hashes the same and replays.
        updated = R.loads(R.dumps(first.recipe))
        self.assertEqual(updated.output["sha256"], first.summary()["outputs"][0]["sha256"])
        self.assertIsNotNone(updated.cost)
        third = run_recipe(updated, Mode.REPLAY, cache_dir=self.cache_dir, clock=self.clock)
        self.assertTrue(third.output_matches)

    def test_record_hit_reuses_cache_unless_forced(self):
        rec = make_recipe()
        run_recipe(rec, Mode.RECORD, cache_dir=self.cache_dir, clock=self.clock)
        self.assertTrue(run_recipe(rec, Mode.RECORD, cache_dir=self.cache_dir, clock=self.clock).result.from_cache)
        forced = run_recipe(rec, Mode.RECORD, cache_dir=self.cache_dir, clock=self.clock, force=True)
        self.assertFalse(forced.result.from_cache)

    def test_mock_mode_routes_to_mock_and_never_touches_the_cache(self):
        rec = make_recipe(provider="local-onnx")
        out = run_recipe(rec, Mode.MOCK, cache_dir=self.cache_dir, clock=self.clock)
        self.assertEqual(out.provider_id, "mock")
        self.assertEqual(out.recipe_provider_id, "local-onnx")
        self.assertFalse(self.cache_dir.exists())

    def test_default_provider_from_the_task(self):
        out = run_recipe(make_recipe(provider=None), Mode.MOCK, cache_dir=self.cache_dir, clock=self.clock)
        self.assertEqual(out.recipe_provider_id, "local-onnx")  # §3.1: UPSCALE defaults to local-onnx

    def test_stub_provider_refuses_in_record_and_caches_nothing(self):
        with self.assertRaises(ProviderNotImplemented) as cm:
            run_recipe(make_recipe(provider="local-onnx"), Mode.RECORD, cache_dir=self.cache_dir, clock=self.clock)
        self.assertIn("not implemented in W0", str(cm.exception))
        self.assertFalse((self.cache_dir / "entries").exists())

    def test_replay_works_for_any_provider_once_recorded(self):
        # Simulate a result recorded on the 3090 for local-onnx: store it directly in the cache.
        rec = make_recipe(provider="local-onnx")
        mocked = run_recipe(rec, Mode.MOCK, cache_dir=self.cache_dir, clock=self.clock)
        ResultCache(self.cache_dir).store(rec.canonical_hash(), rec.hash_payload(), mocked.result)
        out = run_recipe(rec, Mode.REPLAY, cache_dir=self.cache_dir, clock=self.clock)
        self.assertTrue(out.result.from_cache)

    def test_corrupt_blob_is_detected(self):
        first = run_recipe(make_recipe(), Mode.RECORD, cache_dir=self.cache_dir, clock=self.clock)
        sha = first.summary()["outputs"][0]["sha256"]
        ResultCache(self.cache_dir).blob_path(sha).write_bytes(b"tampered")
        with self.assertRaises(CacheCorrupt):
            run_recipe(make_recipe(), Mode.REPLAY, cache_dir=self.cache_dir, clock=self.clock)

    def test_path_inputs_are_read_and_hashed(self):
        png = tiny_png(b"file", 4)
        (self.tmp / "in.png").write_bytes(png)
        d = recipe_dict()
        d["inputs"] = [{"path": "in.png", "origin": "library"}]
        rec = R.Recipe.from_json(d)
        out = run_recipe(rec, Mode.RECORD, cache_dir=self.cache_dir, base_dir=self.tmp, clock=self.clock)
        self.assertEqual(out.recipe.inputs[0].sha256, __import__("hashlib").sha256(png).hexdigest())
        replay = run_recipe(rec, Mode.REPLAY, cache_dir=self.cache_dir, base_dir=self.tmp, clock=self.clock)
        self.assertEqual(replay.recipe_hash, out.recipe_hash)
        entry = json.loads((self.cache_dir / "entries" / f"{out.recipe_hash}.json").read_text(encoding="utf-8"))
        self.assertNotIn("path", entry["recipe"]["inputs"][0])


if __name__ == "__main__":
    unittest.main()
