"""Wave 3 gates: W3.1 CPU classifiers, W3.4 library matcher, W3.5 recipe rebuild with tolerance + fingerprint."""

import hashlib
import json
import random
import unittest

from tests.fuse_ai_support import TempDirCase

from fuse_ai import RECIPE_SCHEMA, classify, fimage, fingerprint, imaging, matcher, mock, providers
from fuse_ai.cache import Mode
from fuse_ai.errors import GateFailure
from fuse_ai.providers import Cost, Output, Request, Result, Task
from fuse_ai.recipe import Recipe
from fuse_ai.runner import run_recipe

GATE_SEED = 20261105  # thresholds were tuned on other seeds (1-13, 1000-4242)


class Classifiers(unittest.TestCase):
    """W3.1: labelled synthetic set (generated here), >= 90 % accuracy per detector."""

    @classmethod
    def setUpClass(cls):
        cls.result = classify.evaluate(GATE_SEED, per_class=20, size=64)

    def test_each_detector_at_least_90_percent(self):
        for name, r in self.result.items():
            with self.subTest(detector=name):
                self.assertEqual(r["n"], 40)
                self.assertGreaterEqual(r["accuracy"], 0.90, r["misses"])

    def test_overall_and_role(self):
        total = sum(r["accuracy"] * r["n"] for r in self.result.values()) / sum(r["n"] for r in self.result.values())
        self.assertGreaterEqual(total, 0.93)
        rng = random.Random(GATE_SEED + 1)
        cases = []
        for _ in range(8):
            cases += [(fimage.gen_normal_map(rng), "normal"), (fimage.gen_ui(rng), "ui"),
                      (fimage.gen_lightmap(rng), "lightmap"), (fimage.gen_albedo(rng)[0], "albedo")]
        ok = sum(classify.classify(img.quantised())["role"] == role for img, role in cases)
        self.assertGreaterEqual(ok / len(cases), 0.90)

    def test_png_round_trip_and_determinism(self):
        img = fimage.gen_normal_map(random.Random(3)).quantised()
        png = imaging.encode_png(img.to_image())
        again = fimage.FImage.from_image(imaging.decode_png(png))
        self.assertEqual(classify.classify(img), classify.classify(again))
        self.assertEqual(classify.evaluate(5, per_class=2), classify.evaluate(5, per_class=2))


class LibraryMatcher(unittest.TestCase):
    """W3.4: on a synthetic set, the top-3 contains the known match >= 80 % of the time."""

    def test_top3_rate(self):
        res = matcher.synthetic_benchmark(GATE_SEED, queries=40, top_k=3)
        self.assertEqual(res["library"], 36)  # 18 families x (normal, dark): same-colour confusers on purpose
        self.assertGreaterEqual(res["top_k_rate"], 0.80, [r for r in res["rows"] if not r["hit"]])
        self.assertGreaterEqual(res["colour_only_top_k_rate"], 0.70)  # colour statistics alone carry most of it

    def test_ranking_is_deterministic_and_tags_help(self):
        lib, specs = matcher.build_library(1)
        img, tags = matcher.legacy_query(random.Random(2), specs["mat/red_brick"])
        a = matcher.rank(img, tags, lib, top=5)
        self.assertEqual(a, matcher.rank(img, tags, lib, top=5))
        self.assertEqual(matcher.jaccard(["a", "b"], ["b", "c"]), 1 / 3)


class NoisyGpu:
    """A local provider that behaves like GPU inference: the same recipe gives slightly different pixels each
    run (±1 LSB), so the recorded sha256 cannot be reproduced exactly (§3.3 "Determinism")."""

    id = "test-gpu"
    cloud = False

    def __init__(self):
        self.calls = 0

    def capabilities(self):
        return {Task.UPSCALE}

    def vram_mb(self, req):
        return 2100

    def estimate(self, req):
        return Cost(seconds=2.0, gpu="rtx3090", peak_vram_mb=2100)

    def run(self, req):
        self.calls += 1
        base = mock.MockProvider().run(req).primary.data
        img = imaging.decode_png(base)
        rng = random.Random(1000 + self.calls)
        px = bytearray(img.rgba)
        for i in range(len(px)):
            if i % 4 != 3:
                px[i] = max(0, min(255, px[i] + rng.choice((-1, 0, 0, 1))))
        out = imaging.encode_png(imaging.Image(img.width, img.height, bytes(px)))
        return Result([Output("upscaled.png", "image/png", out)], {}, Cost(seconds=2.0, gpu="rtx3090"), self.id,
                      "test-upscaler")


class RebuildWithTolerance(TempDirCase):
    """W3.5: a rebuild on a clean cache reproduces within tolerance; the fingerprint verifies it without the image."""

    def setUp(self):
        super().setUp()
        self.gpu = NoisyGpu()
        providers.register(self.gpu, replace=True)
        src = fimage.gen_albedo(random.Random(9), 32, kind="bricks")[0].quantised()
        (self.tmp / "legacy.png").write_bytes(imaging.encode_png(src.to_image()))

    def tearDown(self):
        providers.unregister("test-gpu")
        super().tearDown()

    def recipe(self, name="legacy.png"):
        return Recipe.from_json({"schema": RECIPE_SCHEMA, "id": "tex/legacy/upscale", "task": "UPSCALE",
                                 "provider": "test-gpu", "model": {"id": "test-upscaler"}, "seed": 7,
                                 "params": {"scale": 2}, "inputs": [{"path": name, "origin": "original"}]})

    def test_clean_cache_rebuild_within_tolerance(self):
        author = run_recipe(self.recipe(), Mode.RECORD, cache_dir=self.tmp / "author-cache", base_dir=self.tmp,
                            clock=self.clock)
        ref = author.result.primary.data
        out = dict(author.recipe.output)
        out["tolerance"] = {"metric": "flip", "max_mean": 0.02}
        out["fingerprint"] = fingerprint.fingerprint(fimage.FImage.from_image(imaging.decode_png(ref)))
        self.assertEqual(out["distribution"], "recipe-only")  # the input is an original: ship the recipe only

        user = run_recipe(self.recipe(), Mode.RECORD, cache_dir=self.tmp / "clean-cache", base_dir=self.tmp,
                          clock=self.clock)
        self.assertEqual(user.recipe_hash, author.recipe_hash)
        rebuilt = user.result.primary.data
        self.assertNotEqual(hashlib.sha256(rebuilt).hexdigest(), out["sha256"])  # not bit-stable, as on a GPU

        with_ref = fingerprint.verify_rebuild(out, rebuilt, reference=ref)
        self.assertEqual(with_ref["method"], "tolerance")
        self.assertLess(with_ref["value"], 0.02)
        shipped = fingerprint.verify_rebuild(out, rebuilt)  # the user has no reference image, only the patch
        self.assertEqual(shipped["method"], "fingerprint")
        self.assertEqual(fingerprint.verify_rebuild(out, ref)["method"], "exact")
        for metric, lim in (("psnr", {"min": 40.0}), ("ssim", {"min": 0.98}), ("mae", {"max_mean": 0.005})):
            res = fingerprint.within_tolerance(fimage.FImage.from_image(imaging.decode_png(rebuilt)),
                                               fimage.FImage.from_image(imaging.decode_png(ref)),
                                               dict(lim, metric=metric))
            self.assertTrue(res["ok"], res)

    def test_wrong_rebuild_is_rejected(self):
        author = run_recipe(self.recipe(), Mode.RECORD, cache_dir=self.tmp / "a", base_dir=self.tmp, clock=self.clock)
        ref = author.result.primary.data
        out = dict(author.recipe.output, tolerance={"metric": "flip", "max_mean": 0.02},
                   fingerprint=fingerprint.fingerprint(fimage.FImage.from_image(imaging.decode_png(ref))))
        other = fimage.gen_albedo(random.Random(10), 32, kind="planks")[0].quantised()
        (self.tmp / "other.png").write_bytes(imaging.encode_png(other.to_image()))
        wrong = run_recipe(self.recipe("other.png"), Mode.RECORD, cache_dir=self.tmp / "b", base_dir=self.tmp,
                           clock=self.clock).result.primary.data
        with self.assertRaises(GateFailure):
            fingerprint.verify_rebuild(out, wrong, reference=ref)
        with self.assertRaises(GateFailure):
            fingerprint.verify_rebuild(out, wrong)
        with self.assertRaises(GateFailure):
            fingerprint.verify_rebuild({"sha256": out["sha256"]}, wrong)  # no tolerance, no fingerprint


class FingerprintDoesNotReconstruct(unittest.TestCase):
    """W3.5: the fingerprint cannot reconstruct the output: PSNR vs the original < 15 dB at full resolution."""

    @classmethod
    def setUpClass(cls):
        cls.textures = fingerprint.legacy_texture_set(3) + fingerprint.legacy_texture_set(4)
        cls.attacks = [fingerprint.attack_psnr(img) for img in cls.textures]

    def test_attack_psnr_below_15db(self):
        tested = 0
        for n, (img, attacks) in enumerate(zip(self.textures, self.attacks)):
            best = max(attacks.values())
            with self.subTest(texture=n, attacks={k: round(v, 2) for k, v in attacks.items()}):
                self.assertEqual((img.w, img.h), (256, 256))
                # nothing in the fingerprint beats its unavoidable mean colour (no layout information)
                self.assertLessEqual(best, attacks["mean_colour"] + 0.5)
                if attacks["mean_colour"] < 15.0:  # textures with real contrast (the floor itself is below 15 dB)
                    self.assertLess(best, 15.0)
                    tested += 1
        self.assertGreaterEqual(tested, 10)

    def test_plan_sketch_thumbnail_leaks_more(self):
        """The §3.3 sketch (16x16 thumbnail) reconstructs far better than the fingerprint: why it was replaced."""
        gains = [fingerprint.thumbnail16_psnr(img) - max(a.values()) for img, a in zip(self.textures, self.attacks)]
        self.assertGreater(sum(gains) / len(gains), 3.0)

    def test_fingerprint_is_small_and_stable(self):
        fp = fingerprint.fingerprint(self.textures[0])
        self.assertEqual(fp["schema"], fingerprint.FINGERPRINT_SCHEMA)
        self.assertLess(len(json.dumps(fp)), 700)
        self.assertEqual(fingerprint.compare(fp, fp),
                         {"spectrum_db": 0.0, "orient_l1": 0.0, "hist_l1": 0.0, "lab_mean_de": 0.0})
        self.assertNotIn("thumb", json.dumps(fp))


if __name__ == "__main__":
    unittest.main()
