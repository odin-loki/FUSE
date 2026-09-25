#!/usr/bin/env python3
"""asset_determinism gate (FUSE_ASSET_PLAN §5.3, Wave 0 W0.9).

  * same recipe + seed -> byte-identical outputs: two fresh caches, the uncached path, and fresh
    interpreters with different PYTHONHASHSEED values; outputs also match pinned sha256s, so a change
    of the bytes (platform drift, or a generator edit without a version bump) fails here;
  * a different seed -> different bytes for every output;
  * seed plumbing: named sub-streams are independent and stable;
  * cache: hits reuse verified blobs, corrupted blobs are detected and regenerated, the key tracks
    seed / params / generator version but not description / licence;
  * `verify` (the drift check) passes on a clean cache and reports tampered outputs;
  * recipe schema: JSON and YAML load to the same recipe; malformed recipes are rejected;
  * licence records follow the Content/licences.lock.json format of §2.4 (W0.5 lint rules 3, 5, 7)
    and merge idempotently into a lock file;
  * generator sources use no ambient randomness (random, numpy.random, time, hash(), os.environ).

Python stdlib only. Usage: test_asset_determinism.py [--work DIR]
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
PKG_ROOT = HERE.parent
sys.path.insert(0, str(PKG_ROOT))

from fuse_assetgen import pngio  # noqa: E402
from fuse_assetgen.cache import Cache  # noqa: E402
from fuse_assetgen.licence import make_record, merge_into_lock  # noqa: E402
from fuse_assetgen.pipeline import cache_key, generate, run_generator  # noqa: E402
from fuse_assetgen.registry import load_builtin_generators  # noqa: E402
from fuse_assetgen.schema import RecipeError, load_recipe_file, validate_recipe, yaml_available  # noqa: E402
from fuse_assetgen.seed import Pcg32, SeedStream, derive_seed  # noqa: E402

EXAMPLES = PKG_ROOT / "recipes" / "examples"
RECIPES = sorted(EXAMPLES.glob("*.json"))
REG = load_builtin_generators()
FAKE_REV = "0123456789abcdef0123456789abcdef01234567"

# Pinned output digests of the example recipes (update together with a generator version bump).
PINNED = {
    "noise_rock_a_height.png": "f2058932e9b99c7ebd850d783a81e6e8c98563bdd63fa4b982699a63d20ad965",
    "noise_rock_a_albedo.png": "f4894c2474b046cc48dedfa306ce61578710a0f13f0829e2bff5649b52bf1482",
    "meadow_a_scatter.json": "b93d8f7ea8742eb7dd494e0be5a388f3d21ebc944cc8590765686c862fea9560",
}

WORK = None


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load(path) -> dict:
    return validate_recipe(load_recipe_file(path), REG, str(path))


class Workdir(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix=self.id().rsplit(".", 1)[-1] + "-", dir=WORK))

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)


class Determinism(Workdir):
    def test_examples_present(self):
        self.assertGreaterEqual(len(RECIPES), 2, "example recipes missing")

    def test_same_seed_identical_bytes(self):
        for path in RECIPES:
            a = generate(path, self.dir / "a", git_rev=FAKE_REV)
            b = generate(path, self.dir / "b", git_rev=FAKE_REV)
            c = run_generator(load(path), REG)
            self.assertFalse(a.cache_hit or b.cache_hit)
            self.assertEqual(a.outputs, b.outputs, f"{path.name}: two fresh caches differ")
            self.assertEqual(a.outputs, c, f"{path.name}: cached vs uncached path differ")
            for p, (name, data) in zip(a.paths, a.outputs):
                self.assertEqual(p.read_bytes(), data, f"{name}: materialised copy differs")

    def test_pinned_digests(self):
        seen = {}
        for path in RECIPES:
            for name, data in run_generator(load(path), REG):
                seen[name] = sha(data)
        self.assertEqual(seen, PINNED, "output bytes drifted from the pinned sha256s (platform drift, or a "
                                       "generator change without a version bump + PINNED update)")

    def test_fresh_interpreters_hashseed(self):
        digests = set()
        for hashseed in ("0", "1", "4242"):
            env = dict(os.environ, PYTHONHASHSEED=hashseed, PYTHONDONTWRITEBYTECODE="1",
                       PYTHONPATH=str(PKG_ROOT) + os.pathsep + os.environ.get("PYTHONPATH", ""))
            out = subprocess.run([sys.executable, "-m", "fuse_assetgen", "hash", *map(str, RECIPES)], env=env,
                                 cwd=str(self.dir), capture_output=True, text=True, timeout=300)
            self.assertEqual(out.returncode, 0, out.stderr)
            digests.add(out.stdout)
        self.assertEqual(len(digests), 1, "output depends on PYTHONHASHSEED (unordered iteration?)")

    def test_different_seed_different_bytes(self):
        for path in RECIPES:
            r = load(path)
            base = dict(run_generator(r, REG))
            for delta in (1, 7919, 1 << 40):
                other = dict(r, seed=(r["seed"] + delta) % (1 << 63))
                alt = dict(run_generator(validate_recipe(other, REG), REG))
                self.assertEqual(sorted(base), sorted(alt))
                for name in base:
                    self.assertNotEqual(base[name], alt[name], f"{path.name}: {name} ignores the seed (+{delta})")

    def test_seed_override_matches_recipe_edit(self):
        path = RECIPES[0]
        via_cli = generate(path, self.dir / "a", seed=99, git_rev=FAKE_REV)
        edited = dict(load_recipe_file(path), seed=99)
        via_edit = generate(edited, self.dir / "b", git_rev=FAKE_REV)
        self.assertEqual(via_cli.outputs, via_edit.outputs)
        self.assertEqual(via_cli.record["seed"], 99)


class Seeds(unittest.TestCase):
    def test_streams(self):
        s = SeedStream(1234)
        self.assertEqual(s.derive("octave", 1).seed, SeedStream(1234).derive("octave", 1).seed)
        seeds = {s.derive("octave", i).seed for i in range(64)} | {s.derive("octave", str(i)).seed for i in range(64)}
        self.assertEqual(len(seeds), 128, "sub-streams collide (int vs str path elements must differ)")
        self.assertNotEqual(SeedStream(1234).seed, SeedStream(1235).seed)
        # Adding a consumer does not shift another stream.
        a = [s.derive("position").rng().next_u32() for _ in range(1)]
        s.derive("new_consumer").rng().next_u32()
        self.assertEqual(a, [s.derive("position").rng().next_u32()])

    def test_known_answers(self):
        # Fixed integer algorithms: these values never change on any platform.
        self.assertEqual(derive_seed(0), int.from_bytes(hashlib.sha256(b"fuse-assetgen-seed\0" + bytes(8)).digest()[:8], "little"))
        self.assertEqual(derive_seed(1234, "octave", 3), 12449410366490938202)
        r = Pcg32(42)
        self.assertEqual([r.next_u32() for _ in range(4)], [3270867926, 1795671209, 1924641435, 1143034755])
        u = Pcg32(7)
        vals = [u.uniform() for _ in range(2000)]
        self.assertTrue(all(0.0 <= v < 1.0 for v in vals))
        self.assertAlmostEqual(sum(vals) / len(vals), 0.5, delta=0.03)
        b = Pcg32(9)
        self.assertEqual(set(b.below(10) for _ in range(500)), set(range(10)))

    def test_bad_seeds(self):
        for bad in (-1, 1 << 63, 1.5, True, "1"):
            with self.assertRaises((ValueError, TypeError)):
                derive_seed(bad)


class CacheBehaviour(Workdir):
    def test_hit_and_corruption(self):
        path = RECIPES[0]
        first = generate(path, self.dir, git_rev=FAKE_REV)
        second = generate(path, self.dir, git_rev=FAKE_REV)
        self.assertFalse(first.cache_hit)
        self.assertTrue(second.cache_hit)
        self.assertEqual(first.outputs, second.outputs)
        cache = Cache(self.dir)
        blob = cache.blob_path(sha(first.outputs[0][1]))
        blob.write_bytes(b"corrupted")
        third = generate(path, self.dir, git_rev=FAKE_REV)
        self.assertFalse(third.cache_hit, "corrupted blob was trusted")
        self.assertEqual(third.outputs, first.outputs)
        self.assertEqual(blob.read_bytes(), first.outputs[0][1], "corrupted blob not repaired")

    def test_key_inputs(self):
        r = load(RECIPES[0])
        gen = REG.get(r["generator"])
        k0, _ = cache_key(r, gen)
        self.assertNotEqual(k0, cache_key(dict(r, seed=r["seed"] + 1), gen)[0])
        params = copy.deepcopy(r["params"])
        params["octaves"] = params["octaves"] - 1
        self.assertNotEqual(k0, cache_key(dict(r, params=params), gen)[0])
        self.assertEqual(k0, cache_key(dict(r, description="other", licence="CC0-1.0"), gen)[0])
        bumped = copy.copy(gen)
        bumped.version += 1
        self.assertNotEqual(k0, cache_key(r, bumped)[0])

    def test_cli_generate_verify_and_drift(self):
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", FUSE_ASSETGEN_GIT_REV=FAKE_REV,
                   PYTHONPATH=str(PKG_ROOT) + os.pathsep + os.environ.get("PYTHONPATH", ""))
        lock = self.dir / "licences.lock.json"
        run = lambda *a: subprocess.run([sys.executable, "-m", "fuse_assetgen", *a], env=env, capture_output=True,
                                        text=True, timeout=300)
        out = run("generate", *map(str, RECIPES), "--cache", str(self.dir / "c"), "--lock", str(lock),
                  "--out", str(self.dir / "out"))
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual(len(json.loads(lock.read_text())["assets"]), len(RECIPES))
        out = run("verify", *map(str, RECIPES), "--cache", str(self.dir / "c"))
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        # Tamper: point one action at a valid but different blob -> verify must report drift.
        cache = Cache(self.dir / "c")
        r = load(RECIPES[0])
        key, _ = cache_key(r, REG.get(r["generator"]))
        rec = json.loads(cache.action_path(key).read_text())
        fake = b"not what the generator makes"
        rec["outputs"][0].update(sha256=cache.put_blob(fake), size=len(fake))
        cache.action_path(key).write_text(json.dumps(rec))
        out = run("verify", *map(str, RECIPES), "--cache", str(self.dir / "c"))
        self.assertEqual(out.returncode, 1, "verify missed a drifted cache entry")
        self.assertIn("DRIFT", out.stderr)
        out = run("validate", *map(str, RECIPES))
        self.assertEqual(out.returncode, 0, out.stderr)


class Schema(unittest.TestCase):
    def test_yaml_equals_json(self):
        if not yaml_available():
            self.skipTest("PyYAML not importable (YAML recipes optional)")
        self.assertEqual(load(EXAMPLES / "noise_rock_a.yaml"), load(EXAMPLES / "noise_rock_a.json"))

    def test_defaults_filled(self):
        r = validate_recipe({"schema": 1, "id": "tex/test/defaults_a", "generator": "noise_texture", "seed": 1}, REG)
        self.assertEqual(r["params"]["size"], 64)
        self.assertEqual(r["licence"], "LicenseRef-FUSE-Generated")

    def test_rejections(self):
        good = load_recipe_file(RECIPES[0])
        cases = {
            "missing seed": {k: v for k, v in good.items() if k != "seed"},
            "negative seed": dict(good, seed=-3),
            "bool seed": dict(good, seed=True),
            "float seed": dict(good, seed=1.0),
            "schema 2": dict(good, schema=2),
            "bad id": dict(good, id="Tex/Test"),
            "two-segment id": dict(good, id="tex/noise_a"),
            "unknown generator": dict(good, generator="nope"),
            "unknown top key": dict(good, sed=1),
            "unknown param": dict(good, params=dict(good["params"], octave=3)),
            "param below min": dict(good, params=dict(good["params"], octaves=0)),
            "param enum": dict(good, params=dict(good["params"], size=100)),
            "param type": dict(good, params=dict(good["params"], gain="0.5")),
            "nested range": dict(good, params=dict(good["params"], albedo={"low": [0.0, 0.1, 0.1]})),
            "array length": dict(good, params=dict(good["params"], albedo={"high": [0.5, 0.5]})),
            "NC licence": dict(good, licence="CC-BY-NC-4.0"),
            "ND licence": dict(good, licence="CC-BY-ND-4.0"),
            "derived_from type": dict(good, derived_from="mat/rock/x"),
        }
        for name, data in cases.items():
            with self.assertRaises(RecipeError, msg=name):
                validate_recipe(data, REG, name)


class Licence(Workdir):
    GEN_RE = re.compile(r"^\S+@[0-9a-f]{7,40}$")

    def test_record_format(self):
        for path in RECIPES:
            res = generate(path, self.dir, git_rev=FAKE_REV)
            rec = res.record
            for key in ("id", "origin", "source", "author", "generator", "seed", "licence", "retrieval",
                        "derived_from", "files", "review"):
                self.assertIn(key, rec)
            self.assertEqual(rec["origin"], "generated")
            self.assertEqual(rec["retrieval"], "generator")
            self.assertEqual(rec["licence"], "LicenseRef-FUSE-Generated")
            self.assertRegex(rec["generator"], self.GEN_RE)
            self.assertTrue(rec["generator"].startswith("Tools/FUSE/AssetGen/fuse_assetgen/generators/"))
            self.assertIsInstance(rec["seed"], int)
            self.assertTrue(re.match(r"^[a-z0-9][a-z0-9_./-]*$", rec["id"]))
            self.assertFalse(rec["review"]["required"])
            self.assertEqual(len(rec["files"]), len(res.outputs))
            for f in rec["files"]:
                self.assertEqual(f["location"], "cache")
                self.assertRegex(f["sha256"], r"^[0-9a-f]{64}$")
                self.assertEqual(sha((self.dir / f["path"]).read_bytes()), f["sha256"])
            self.assertEqual(rec["recipe"], path.relative_to(PKG_ROOT.parents[2]).as_posix())

    def test_lock_merge(self):
        lock = self.dir / "lock.json"
        lock.write_text(json.dumps({"schema": 1, "assets": [{"id": "zz/other/keep_a", "origin": "sourced"}]}))
        recs = [generate(p, self.dir / "c", git_rev=FAKE_REV).record for p in RECIPES]
        merge_into_lock(lock, recs)
        first = lock.read_bytes()
        merge_into_lock(lock, recs)
        self.assertEqual(first, lock.read_bytes(), "merge is not idempotent")
        ids = [a["id"] for a in json.loads(first)["assets"]]
        self.assertEqual(ids, sorted(ids))
        self.assertIn("zz/other/keep_a", ids)
        self.assertEqual(len(ids), len(RECIPES) + 1)

    def test_derived_from_and_licence_carried(self):
        data = dict(load_recipe_file(RECIPES[0]), derived_from=["mat/rock/granite_01", "mat/rock/granite_01"],
                    licence="CC0-1.0")
        rec = generate(data, self.dir, git_rev=FAKE_REV).record
        self.assertEqual(rec["derived_from"], ["mat/rock/granite_01"])
        self.assertEqual(rec["licence"], "CC0-1.0")
        self.assertNotIn("recipe", rec)
        _ = make_record  # public API used by other tools


class Hygiene(unittest.TestCase):
    BANNED = re.compile(r"^\s*(import random|from random|import time|from time|import datetime|from datetime)\b|"
                        r"numpy\.random|np\.random|\bhash\(|os\.environ|os\.urandom|uuid", re.MULTILINE)

    def test_generators_have_no_ambient_randomness(self):
        gen_dir = PKG_ROOT / "fuse_assetgen" / "generators"
        for src in sorted(gen_dir.glob("*.py")):
            m = self.BANNED.search(src.read_text(encoding="utf-8"))
            self.assertIsNone(m, f"{src.name}: ambient nondeterminism '{m.group(0).strip() if m else ''}'")

    def test_png_roundtrip(self):
        for ch, depth in ((1, 8), (1, 16), (3, 8), (4, 8)):
            n = 5 * 3 * ch
            px = [(i * 37) % (1 << depth) for i in range(n)]
            data = pngio.encode_png(5, 3, ch, px, bit_depth=depth)
            self.assertEqual(pngio.decode_png(data), (5, 3, ch, depth, px))
            self.assertEqual(data, pngio.encode_png(5, 3, ch, px, bit_depth=depth))


def main() -> int:
    global WORK
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", help="scratch directory (default: a temp dir)")
    args, rest = ap.parse_known_args()
    if args.work:
        WORK = str(Path(args.work).resolve())
        Path(WORK).mkdir(parents=True, exist_ok=True)
    prog = unittest.main(argv=[sys.argv[0], "-v", *rest], exit=False)
    return 0 if prog.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
