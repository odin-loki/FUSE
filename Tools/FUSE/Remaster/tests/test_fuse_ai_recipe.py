"""Recipe schema ``fuse.ai-recipe/1`` and its canonical hash (plan §3.3)."""

import copy
import hashlib
import json
import unittest

from tests.fuse_ai_support import recipe_dict

from fuse_ai import recipe as R
from fuse_ai.errors import RecipeError


class RecipeHash(unittest.TestCase):
    def test_hash_ignores_output_and_review(self):
        base = recipe_dict()
        h = R.canonical_hash(base)
        with_extra = copy.deepcopy(base)
        with_extra["output"] = {"sha256": "a" * 64, "tolerance": {"metric": "flip", "max_mean": 0.01}}
        with_extra["review"] = {"state": "approved", "by": "Reviewer", "date": "2026-11-05"}
        with_extra["cost"] = {"usd": 1.25, "seconds": 3.2}
        self.assertEqual(R.canonical_hash(with_extra), h)

    def test_hash_ignores_key_order_whitespace_and_input_path(self):
        base = recipe_dict()
        shuffled = json.loads(json.dumps(base, sort_keys=False, indent=7))
        shuffled = dict(reversed(list(shuffled.items())))
        self.assertEqual(R.canonical_hash(shuffled), R.canonical_hash(base))
        moved = copy.deepcopy(base)
        moved["inputs"][0]["path"] = "somewhere/else.png"
        self.assertEqual(R.canonical_hash(moved), R.canonical_hash(base))

    def test_hash_covers_params_seed_model_provider_and_provenance(self):
        base = recipe_dict()
        h = R.canonical_hash(base)
        for mutate in (lambda d: d["params"].update(scale=4), lambda d: d.update(seed=8),
                       lambda d: d["model"].update(id="other"), lambda d: d.update(provider="local-onnx"),
                       lambda d: d["inputs"][0].update(origin="original"),
                       lambda d: d["inputs"][0].update(canonical_sha256="2" * 64),
                       lambda d: d.update(purpose="something else")):
            d = copy.deepcopy(base)
            mutate(d)
            self.assertNotEqual(R.canonical_hash(d), h, d)

    def test_hash_is_sha256_of_canonical_json(self):
        r = R.Recipe.from_json(recipe_dict())
        expected = hashlib.sha256(json.dumps(r.hash_payload(), sort_keys=True, separators=(",", ":"),
                                             ensure_ascii=False).encode("utf-8")).hexdigest()
        self.assertEqual(r.canonical_hash(), expected)

    def test_missing_origin_normalises_to_original(self):
        d = recipe_dict()
        del d["inputs"][0]["origin"]
        r = R.Recipe.from_json(d)
        self.assertEqual(r.inputs[0].origin, "original")
        self.assertTrue(r.inputs[0].is_original_content)

    def test_path_only_input_needs_resolution_before_hashing(self):
        d = recipe_dict()
        d["inputs"] = [{"path": "a.png", "origin": "library"}]
        with self.assertRaises(RecipeError):
            R.canonical_hash(d)


class RecipeValidation(unittest.TestCase):
    def assertInvalid(self, d, fragment):
        with self.assertRaises(RecipeError) as cm:
            R.Recipe.from_json(d)
        self.assertIn(fragment, str(cm.exception))

    def test_valid_round_trip(self):
        d = recipe_dict(output={"sha256": "b" * 64}, review={"state": "draft"}, **{"x-note": "kept"})
        r = R.Recipe.from_json(d)
        again = R.loads(R.dumps(r))
        self.assertEqual(again.to_json(), r.to_json())
        self.assertEqual(again.extensions, {"x-note": "kept"})

    def test_rejections(self):
        cases = [
            (dict(recipe_dict(), schema="fuse.ai-recipe/2"), "schema"),
            (dict(recipe_dict(), task="DANCE"), "task"),
            (dict(recipe_dict(), seed="7"), "seed"),
            (dict(recipe_dict(), surprise=1), "unknown key"),
            (dict(recipe_dict(), review={"state": "maybe"}), "review.state"),
            (dict(recipe_dict(), output={"sha256": "XYZ"}), "output.sha256"),
        ]
        for d, frag in cases:
            with self.subTest(frag=frag):
                self.assertInvalid(d, frag)
        d = recipe_dict()
        d["inputs"][0]["origin"] = "stolen"
        self.assertInvalid(d, "origin")
        d = recipe_dict(origin="derived")
        del d["inputs"][0]["derived_from_original"]
        self.assertInvalid(d, "derived_from_original")
        d = recipe_dict()
        d["inputs"] = [{"origin": "library"}]
        self.assertInvalid(d, "needs canonical_sha256")

    def test_not_json(self):
        with self.assertRaises(RecipeError):
            R.loads("{not json")


if __name__ == "__main__":
    unittest.main()
