"""``python -m fuse_ai`` CLI: run / providers / hash / validate and the exit codes (plan §3.1, §3.3)."""

import contextlib
import io
import json
import os
import subprocess
import sys
import unittest
from pathlib import Path

from tests.fuse_ai_support import TempDirCase, recipe_dict

from fuse_ai.__main__ import main

REMASTER_DIR = Path(__file__).resolve().parent.parent


def cli(*argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = main([str(a) for a in argv])
    return code, out.getvalue(), err.getvalue()


class Cli(TempDirCase):
    def test_providers(self):
        code, out, _ = cli("providers", "--json")
        self.assertEqual(code, 0)
        rows = {r["id"]: r for r in json.loads(out)}
        self.assertEqual(set(rows), {"mock", "local-onnx", "local-torch", "worldmodel", "gemini", "local-llm",
                                     "comfyui", "remix-toolkit", "reference"})
        self.assertTrue(rows["gemini"]["cloud"])
        self.assertFalse(rows["local-onnx"]["cloud"])
        self.assertTrue(rows["mock"]["implemented"])
        code, out, _ = cli("providers")
        self.assertEqual(code, 0)
        self.assertIn("gemini", out)

    def test_run_record_replay_and_exit_codes(self):
        recipe = self.write_json("r.json", recipe_dict())
        cache = self.tmp / "cache"
        code, _, err = cli("run", recipe, "--mode", "replay", "--cache", cache)
        self.assertEqual(code, 3, err)
        self.assertIn("ReplayMiss", err)

        code, out, err = cli("run", recipe, "--mode", "record", "--cache", cache, "--out", self.tmp / "out",
                             "--write-recipe")
        self.assertEqual(code, 0, err)
        summary = json.loads(out.splitlines()[0])
        self.assertEqual(summary["provider"], "mock")
        self.assertTrue((self.tmp / "out" / "test_upscale" / "upscaled.png").is_file())
        written = json.loads(recipe.read_text(encoding="utf-8"))
        self.assertEqual(written["output"]["sha256"], summary["outputs"][0]["sha256"])
        self.assertTrue((cache / "ai-ledger.json").is_file())

        code, out, err = cli("run", recipe, "--cache", cache)  # replay is the default mode
        self.assertEqual(code, 0, err)
        self.assertTrue(json.loads(out)["from_cache"])
        self.assertTrue(json.loads(out)["output_matches"])

        code, out, _ = cli("hash", recipe)
        self.assertEqual(code, 0)
        self.assertTrue(out.startswith(summary["hash"]))

    def test_policy_and_budget_exit_codes(self):
        recipe = self.write_json("cloud.json", recipe_dict(task="TAG_IMAGE", provider="gemini", origin="original"))
        proj = self.write_json("remaster.project.json", {"schema": "fuse.remaster-project/1", "game_id": "g",
                                                         "cloud_upload": "none"})
        code, _, err = cli("run", recipe, "--mode", "record", "--project", proj, "--cache", self.tmp / "c")
        self.assertEqual(code, 5, err)
        self.assertIn("CloudOptInRequired", err)

        cheap = self.write_json("own.json", recipe_dict(task="TAG_IMAGE", provider="gemini", origin="library"))
        budget = self.write_json("ai-budget.json", {"schema": "fuse.ai-budget/1", "defaults": {"per_run_usd": 0.001}})
        code, _, err = cli("run", cheap, "--mode", "record", "--budget", budget, "--cache", self.tmp / "c")
        self.assertEqual(code, 4, err)
        code, _, err = cli("run", cheap, "--mode", "record", "--cache", self.tmp / "c")
        self.assertEqual(code, 6, err)  # passes guard + budget, then the W0 gemini stub

    def test_mock_mode(self):
        recipe = self.write_json("p.json", recipe_dict(task="PORT_SCRIPT", provider="gemini", origin="original",
                                                       params={"text": "function a(%x) {}"}))
        code, out, err = cli("run", recipe, "--mode", "mock", "--cache", self.tmp / "c", "--out", self.tmp / "o")
        self.assertEqual(code, 0, err)
        self.assertIn("function M.a(x)", (self.tmp / "o" / "test_port_script" / "port.lua").read_text())

    def test_invalid_recipe(self):
        bad = self.write_json("bad.json", {"schema": "nope"})
        code, _, err = cli("validate", bad)
        self.assertEqual(code, 2)
        self.assertIn("RecipeError", err)

    def test_module_entry_point_in_a_network_blocked_subprocess(self):
        blocker = ("import socket,runpy,sys\n"
                   "class S(socket.socket):\n"
                   "    def __init__(self,*a,**k): raise RuntimeError('network blocked')\n"
                   "socket.socket=S\n"
                   "sys.argv=['fuse_ai','providers']\n"
                   "runpy.run_module('fuse_ai',run_name='__main__')\n")
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        p = subprocess.run([sys.executable, "-B", "-c", blocker], cwd=str(REMASTER_DIR), env=env,
                           capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("local-onnx", p.stdout)


if __name__ == "__main__":
    unittest.main()
