"""Wave 5 gates: W5.2 collision vs render, W5.3 light calibration, W5.4 look LUT, W5.6 emissive proxy lights."""

import contextlib
import io
import json
import math
import shutil
import tempfile
import unittest
from pathlib import Path

from tests import NETWORK_ATTEMPTS  # noqa: F401  (blocks the network)

from fuse_ai import collision, emissive, lightcal, look, poco_types
from fuse_ai.errors import GateFailure, RecipeError
from fuse_ai.tools import main as tool_main

FX = Path(__file__).resolve().parent / "fixtures"


def tool(*argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = tool_main([str(a) for a in argv])
    return code, out.getvalue(), err.getvalue()


class CollisionVsRender(unittest.TestCase):
    """W5.2: render-mesh vs collision-hull Hausdorff distance within a tolerance; a failing fixture is detected."""

    def test_matching_hull_passes(self):
        res = collision.gate(FX / "collision/crate_render.obj", FX / "collision/crate_hull_ok.obj", tolerance=0.05)
        self.assertTrue(res["ok"])
        self.assertAlmostEqual(res["render_to_hull"], 0.04, places=6)  # lid-covered top face to the hull top
        self.assertLess(res["hull_to_render"], 0.03)

    def test_invisible_wall_fails(self):
        with self.assertRaises(GateFailure) as cm:
            collision.gate(FX / "collision/crate_render.obj", FX / "collision/crate_hull_invisible_wall.obj",
                           tolerance=0.05)
        self.assertIn("collision hull where nothing is drawn", str(cm.exception))

    def test_render_outside_hull_fails(self):
        res = collision.check_files(FX / "collision/crate_render_pipe.obj", FX / "collision/crate_hull_ok.obj",
                                    tolerance=0.05)
        self.assertFalse(res["ok"])
        self.assertAlmostEqual(res["render_to_hull"], 0.38, places=6)  # the pipe end, 0.38 m past the hull
        with self.assertRaises(GateFailure) as cm:
            collision.gate(FX / "collision/crate_render_pipe.obj", FX / "collision/crate_hull_ok.obj", tolerance=0.05)
        self.assertIn("render mesh outside the collision hull", str(cm.exception))

    def test_closest_point_regions(self):
        a, b, c = (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)
        self.assertEqual(collision.closest_point_on_triangle((-1.0, -1.0, 0.0), a, b, c), a)
        self.assertEqual(collision.closest_point_on_triangle((0.25, 0.25, 3.0), a, b, c), (0.25, 0.25, 0.0))
        p = collision.closest_point_on_triangle((1.0, 1.0, 0.0), a, b, c)
        self.assertAlmostEqual(p[0], 0.5)
        self.assertAlmostEqual(p[1], 0.5)

    def test_cli_exit_codes(self):
        code, out, _ = tool("collision", FX / "collision/crate_render.obj", FX / "collision/crate_hull_ok.obj",
                            "--tolerance", "0.05")
        self.assertEqual(code, 0)
        self.assertTrue(json.loads(out)["ok"])
        code, _, err = tool("collision", FX / "collision/crate_render.obj",
                            FX / "collision/crate_hull_invisible_wall.obj", "--tolerance", "0.05")
        self.assertEqual(code, 11, err)


class LightCalibration(unittest.TestCase):
    """W5.3: a synthetic scene with a known scale -> the solver recovers it within 5 %."""

    TRUE = {"point": 1.3, "spot": 0.8, "sun": 1.1, "ambient": 0.6}

    def test_recovers_known_scale(self):
        for seed in (20261105, 7, 31):
            contrib, capture, truth = lightcal.synthetic_scene(seed, scale=0.7, multipliers=self.TRUE)
            res = lightcal.calibrate(contrib, capture, curve="srgb")
            err = abs(res["global_scale"] / truth["scale"] - 1.0)
            with self.subTest(seed=seed):
                self.assertLess(err, 0.05, res)
                for t, m in res["multipliers"].items():
                    self.assertLess(abs(m / truth["multipliers"][t] - 1.0), 0.10, (t, res))
                self.assertGreater(res["regions_skipped"], 0)  # clipped / black regions were excluded
                self.assertLess(res["rms_relative_error"], 0.05)

    def test_other_scales_and_curve(self):
        contrib, capture, truth = lightcal.synthetic_scene(3, scale=0.35, multipliers=self.TRUE)
        res = lightcal.calibrate(contrib, capture)
        self.assertLess(abs(res["global_scale"] / truth["scale"] - 1.0), 0.05)
        wrong = lightcal.calibrate(contrib, capture, curve="linear")  # forgetting the tone curve is visibly worse
        self.assertGreater(wrong["rms_relative_error"], res["rms_relative_error"] * 2)
        with self.assertRaises(RecipeError):
            lightcal.calibrate(contrib, capture, curve="log")

    def test_cli(self):
        contrib, capture, _ = lightcal.synthetic_scene(5, scale=0.6, multipliers=self.TRUE)
        tmp = Path(tempfile.mkdtemp(prefix="fuse_ai_light_"))
        try:
            p = tmp / "scene.json"
            p.write_text(json.dumps({"contrib": contrib, "capture": capture}), encoding="utf-8")
            code, out, err = tool("light-calibrate", p)
            self.assertEqual(code, 0, err)
            self.assertEqual(json.loads(out)["schema"], lightcal.CAL_SCHEMA)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class LookPresets(unittest.TestCase):
    """W5.4: a LUT fitted to captures reproduces a synthetic grade with mean ΔE*ab < 2; .fuselook + .cube emitted."""

    @classmethod
    def setUpClass(cls):
        cls.pairs = look.chart_pairs(12, 1500, seed=1)
        cls.lut = look.fit_lut(cls.pairs, size=17, title="classic test")

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="fuse_ai_look_"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_fitted_lut_reproduces_the_grade(self):
        res = look.evaluate(self.lut, n=2000, seed=99)  # held-out colours
        self.assertLess(res["mean_de76"], 2.0, res)
        self.assertLess(res["p95_de76"], 3.0, res)
        self.assertGreater(look.identity_error(), 5.0)  # the grade is far from identity: the fit did the work

    def test_tetrahedral_port_is_exact_for_affine_luts(self):
        lut = look.Lut3D(5)
        for node in lut.data:
            node[0], node[1], node[2] = 0.5 * node[0] + 0.1, node[1], 1.0 - node[2]
        for c in ((0.13, 0.77, 0.41), (0.9, 0.2, 0.6), (0.0, 1.0, 0.5)):
            got = look.sample_tetrahedral(lut, c)
            self.assertAlmostEqual(got[0], 0.5 * c[0] + 0.1, places=9)
            self.assertAlmostEqual(got[1], c[1], places=9)
            self.assertAlmostEqual(got[2], 1.0 - c[2], places=9)

    def test_emit_classic_fuselook_and_cube(self):
        written = look.emit("classic", self.tmp, lut=self.lut, level="town")
        doc = json.loads(Path(written["fuselook"]).read_text(encoding="utf-8"))
        self.assertEqual(doc["kind"], "fuse.look")
        self.assertEqual(doc["schemaVersion"], 1)
        self.assertEqual(doc["base"]["colorGrade"]["lut"], "luts/classic_town.cube")
        self.assertEqual(look.validate_fuselook(doc, self.tmp), [])
        cube = look.parse_cube(Path(written["cube"]).read_text(encoding="utf-8"))
        self.assertEqual(cube.size, 17)
        self.assertEqual(cube.title, "classic test")
        res = look.evaluate(cube, n=500, seed=5)  # the written text round-trips the fit
        self.assertLess(res["mean_de76"], 2.0)

    def test_presets(self):
        for name in ("remastered", "stylised"):
            written = look.emit(name, self.tmp)
            doc = json.loads(Path(written["fuselook"]).read_text(encoding="utf-8"))
            self.assertEqual(look.validate_fuselook(doc, self.tmp), [], name)
            self.assertNotIn("cube", written)
        with self.assertRaises(RecipeError):
            look.emit("classic", self.tmp)  # Classic is defined by the fitted LUT
        ops = {look.load_preset(n)["base"]["toneMap"]["operator"] for n in ("classic", "remastered", "stylised")}
        self.assertEqual(ops, {"reinhard", "filmic", "aces"})

    def test_cube_reader_rules(self):
        good = look.write_cube(look.Lut3D(2, title="t"))
        self.assertEqual(look.parse_cube(good).size, 2)
        with self.assertRaises(RecipeError):
            look.parse_cube(good + "LUT_3D_SIZE 2\n")  # keyword after data
        with self.assertRaises(RecipeError):
            look.parse_cube(good.replace("LUT_3D_SIZE 2", "LUT_3D_SIZE 3"))

    def test_cli_look_fit(self):
        pairs = self.tmp / "pairs.json"
        pairs.write_text(json.dumps([[list(a), list(b)] for a, b in self.pairs]), encoding="utf-8")
        code, out, err = tool("look-fit", pairs, "--preset", "classic", "--out", self.tmp / "out")
        self.assertEqual(code, 0, err)
        self.assertTrue(Path(json.loads(out)["fuselook"]).is_file())


class EmissiveProxyLights(unittest.TestCase):
    """W5.6: a fixture emissive surface yields a light (and dim / non-emissive surfaces do not)."""

    def setUp(self):
        self.verts, self.groups = emissive.load_obj_groups(FX / "emissive/lamp_room.obj")
        self.mats = json.loads((FX / "emissive/lamp_room.materials.json").read_text(encoding="utf-8"))["materials"]

    def test_fixture_yields_lights(self):
        recs = emissive.proxy_lights(self.verts, self.groups, self.mats, source_id="lamp_room")
        lights = {r["id"]: r for r in recs if r["kind"] == "light"}
        ents = {r["id"]: r for r in recs if r["kind"] == "entity"}
        self.assertEqual(set(lights), {"light/proxy/lamp_room/panel_light", "light/proxy/lamp_room/globe_light"})
        self.assertEqual(len(ents), 2)
        panel = lights["light/proxy/lamp_room/panel_light"]["payload"]
        self.assertEqual(panel["type"], "Rect")
        self.assertEqual(panel["intensity"], 2000.0)  # nits, the POCO unit for area lights
        self.assertEqual(panel["size"], [0.6, 0.6])
        t = ents["entity/proxy/lamp_room/panel_light"]["payload"]["transform"]
        self.assertEqual(t[12:15], [0.0, 2.9, 0.0])
        self.assertEqual(t[8:11], [0.0, -1.0, 0.0])  # +Z = emission direction: down, out of the ceiling
        globe = lights["light/proxy/lamp_room/globe_light"]["payload"]
        self.assertEqual(globe["type"], "Point")
        area = emissive.surface_stats(self.verts, self.groups["globe_light"])["area"]
        self.assertAlmostEqual(globe["intensity"], math.pi * 5000.0 * area, places=3)  # lumens
        self.assertEqual(globe["colorLinear"], [1.0, 0.8, 0.5])
        for r in recs:
            rec = poco_types.load_record(r)  # valid fuse.poco/1 records
            self.assertEqual(rec.provenance.origin, "generated")

    def test_thresholds_and_distribution(self):
        recs = emissive.proxy_lights(self.verts, self.groups, self.mats, min_lumens=5000.0)
        self.assertEqual(recs, [])  # both proxies are below 5000 lm
        recs = emissive.proxy_lights(self.verts, self.groups, self.mats, min_nits=1.0, min_lumens=1.0,
                                     source_distribution="recipe-only")
        self.assertIn("light/proxy/mesh/exit_sign_led", {r["id"] for r in recs})
        self.assertTrue(all(r["distribution"] == "recipe-only" for r in recs))

    def test_cli(self):
        code, out, err = tool("emissive-lights", FX / "emissive/lamp_room.obj", FX / "emissive/lamp_room.materials.json")
        self.assertEqual(code, 0, err)
        self.assertEqual(len(json.loads(out)), 4)


if __name__ == "__main__":
    unittest.main()
