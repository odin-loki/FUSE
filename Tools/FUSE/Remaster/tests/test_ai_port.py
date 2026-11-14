"""W4.3: SUMMARISE_CODE / PORT_SCRIPT spec-first flow (plan §4.5) in replay mode.

Fixture: ShapeBaseData::onCollision / ::onImpact from the MIT Torque3D template scripts
(tests/fixtures/port/shapebase_impact.tscript) -> fuse_remaster_tscript_dump (C++, W1.6) call-graph context ->
spec (gemini, recorded) -> Lua (gemini, recorded, from the spec only) -> structural checks + the parity harness
fuse_remaster_lua_parity (the engine's Lua 5.2.3 under ScriptVM's sandbox) when it is built.
"""

import hashlib
import json
import os
import unittest
from pathlib import Path

from tests.test_ai_gemini import ProviderCase

from fuse_ai import portflow, prompts
from fuse_ai.cache import Mode
from fuse_ai.errors import CloudOptInRequired, GateFailure
from fuse_ai.recipe import load

PORT = Path(__file__).resolve().parent / "fixtures" / "port"
SRC = PORT / "shapebase_impact.tscript"
DUMP_EXE = os.environ.get("FUSE_REMASTER_TSCRIPT_DUMP")
PARITY_EXE = os.environ.get("FUSE_REMASTER_LUA_PARITY")


class CallGraphContext(unittest.TestCase):
    def test_context_is_the_cpp_parser_output(self):
        ctx = portflow.tscript_context(SRC)  # the tool when built, else the committed snapshot
        self.assertEqual(ctx["diagnostics"], [])
        keys = [f["key"] for f in ctx["analysis"]["functions"]]
        self.assertEqual(keys, ["ShapeBaseData::onCollision", "ShapeBaseData::onImpact"])
        self.assertEqual(portflow.context_bytes(ctx), (PORT / "shapebase_impact.callgraph.json").read_bytes()
                         .replace(b"\r\n", b"\n"))

    @unittest.skipUnless(DUMP_EXE and Path(DUMP_EXE).is_file(), "fuse_remaster_tscript_dump not built")
    def test_snapshot_matches_the_tool(self):
        fresh = portflow.tscript_context(SRC, exe=DUMP_EXE)
        snap = json.loads((PORT / "shapebase_impact.dump.json").read_text(encoding="utf-8"))
        self.assertEqual(fresh["analysis"], snap["analysis"])


class SpecFirstPort(ProviderCase):
    def setUp(self):
        super().setUp()
        self.install("gemini_port.json")

    def run_port(self, name, mode=Mode.RECORD, **kw):
        return self.runner(mode, **kw).run(load(PORT / name), base_dir=PORT)

    def test_fixture_torquescript_to_spec_to_lua(self):
        spec_out = self.run_port("shapebase_impact.spec.recipe.json")
        self.assertEqual(spec_out.provider_id, "gemini")
        spec = prompts.parse_output(prompts.load("summarise_code"), spec_out.result.primary.data)
        self.assertEqual(spec["source_sha256"], hashlib.sha256(SRC.read_bytes()).hexdigest())
        self.assertEqual([e["name"] for e in spec["events"]], ["on_collision", "on_impact"])
        # the reviewed spec is committed; it is exactly the recorded answer
        self.assertEqual(spec_out.result.primary.data,
                         (PORT / "shapebase_impact.spec.json").read_bytes().replace(b"\r\n", b"\n"))
        # context caching: the shared engine API description lives in a cachedContents entry
        self.assertEqual(spec_out.result.meta["cached_content"], "cachedContents/fixture-summarise")
        self.assertAlmostEqual(spec_out.result.cost.usd,
                               ((2310 - 905) * 2.0 + 905 * 2.0 * 0.2 + 702 * 12.0 + 905 * 2.0) / 1e6, places=12)

        lua_out = self.run_port("shapebase_impact.lua.recipe.json")
        names = [o.name for o in lua_out.result.outputs]
        self.assertEqual(names, ["port.json", "port.lua"])
        lua = lua_out.result.outputs[1].data
        self.assertEqual(lua, (PORT / "shapebase_impact.port.lua").read_bytes().replace(b"\r\n", b"\n"))
        self.assertEqual(self.gem_tx.unused(), [])
        # spec-first: the port recipe's only input is the spec, never the original script
        rec = load(PORT / "shapebase_impact.lua.recipe.json")
        self.assertEqual([i.name for i in rec.inputs], ["shapebase_impact.spec.json"])

        problems = portflow.structural_checks(lua.decode("utf-8"), spec, original_source=SRC.read_text(encoding="utf-8"),
                                              spec_text=spec_out.result.primary.data.decode("utf-8"))
        self.assertEqual(problems, [])

        # replay mode: both recipes come from the cache, nothing is sent
        self.install()
        for name in ("shapebase_impact.spec.recipe.json", "shapebase_impact.lua.recipe.json"):
            self.assertTrue(self.run_port(name, mode=Mode.REPLAY).result.from_cache)
        self.assertEqual(self.gem_tx.calls, [])

    @unittest.skipUnless(PARITY_EXE and Path(PARITY_EXE).is_file(), "fuse_remaster_lua_parity not built "
                         "(structural checks still run in test_fixture_torquescript_to_spec_to_lua)")
    def test_lua_passes_the_parity_harness(self):
        self.run_port("shapebase_impact.spec.recipe.json")
        lua_out = self.run_port("shapebase_impact.lua.recipe.json")
        lua_path = self.tmp / "port.lua"
        lua_path.write_bytes(lua_out.result.outputs[1].data)
        res = portflow.run_parity(lua_path, PORT / "shapebase_impact.trace.json", PORT / "shapebase_impact.expected.log",
                                  exe=PARITY_EXE)
        self.assertEqual(res["rc"], 0)
        self.assertEqual(res["log"].count(" damage "), 3)
        with self.assertRaises(GateFailure):
            portflow.run_parity(PORT / "shapebase_impact.broken.lua", PORT / "shapebase_impact.trace.json",
                                PORT / "shapebase_impact.expected.log", exe=PARITY_EXE)

    def test_original_script_routes_to_local_llm(self):
        """A user's own game script (origin original) without the opt-in: same prompts, local-llm, never gemini."""
        self.install(llm_fixture="local_llm.json")
        with self.assertRaises(CloudOptInRequired):  # naming gemini explicitly is refused, not rerouted
            d = load(PORT / "original.spec.recipe.json").to_json()
            d["provider"] = "gemini"
            from fuse_ai.recipe import Recipe

            self.runner().run(Recipe.from_json(d), base_dir=PORT)
        self.llm_tx.used = [False] * len(self.llm_tx.used)
        spec_out = self.run_port("original.spec.recipe.json")
        lua_out = self.run_port("original.lua.recipe.json")
        self.assertEqual((spec_out.provider_id, lua_out.provider_id), ("local-llm", "local-llm"))
        self.assertEqual(spec_out.recipe.model["id"], "qwen3-coder-30b-a3b-instruct-q4_k_m")
        self.assertEqual(self.gem_tx.calls, [])
        self.assertEqual(lua_out.result.outputs[1].data,
                         (PORT / "shapebase_impact.port.lua").read_bytes().replace(b"\r\n", b"\n"))
        self.assertEqual(spec_out.recipe.output["distribution"], "recipe-only")  # derived from an original
        self.assertGreater(lua_out.result.cost.seconds, 30.0)
        self.assertEqual(lua_out.result.cost.usd, 0.0)


class StructuralChecks(unittest.TestCase):
    def setUp(self):
        self.spec = json.loads((PORT / "shapebase_impact.spec.json").read_text(encoding="utf-8"))
        self.lua = (PORT / "shapebase_impact.port.lua").read_text(encoding="utf-8")
        self.src = SRC.read_text(encoding="utf-8")

    def test_fixture_port_is_clean(self):
        self.assertEqual(portflow.structural_checks(self.lua, self.spec, original_source=self.src), [])

    def test_violations_are_reported(self):
        sandbox = (PORT / "sandbox_io.lua").read_text(encoding="utf-8")
        probs = portflow.structural_checks(sandbox, self.spec)
        self.assertTrue(any("'io'" in p for p in probs), probs)
        self.assertTrue(any("on_impact" in p for p in probs), probs)
        leaky = self.lua + "\nfunction helper(x) return os.time() end\ncounter = 1\n"
        probs = portflow.structural_checks(leaky, self.spec)
        self.assertTrue(any("'os'" in p for p in probs), probs)
        self.assertTrue(any("global function 'helper'" in p for p in probs), probs)
        self.assertTrue(any("assigns global 'counter'" in p for p in probs), probs)
        unbalanced = self.lua.replace("    return\n  end", "    return\n", 1)
        self.assertTrue(any("block structure" in p for p in portflow.structural_checks(unbalanced, self.spec)))
        copied = self.lua + "\n-- %dmgAmt = %len/%this.minImpactSpeed * %this.collisionMul;\n"
        probs = portflow.structural_checks(copied, self.spec, original_source=self.src)
        self.assertTrue(any("original script text copied" in p for p in probs), probs)
        self.assertTrue(any("unknown API table 'Torque'" in p
                            for p in portflow.structural_checks(self.lua + "\nlocal x = Torque.echo(1)\n", self.spec)))

    def test_cli_port_check(self):
        import contextlib
        import io

        from fuse_ai.tools import main as tool_main

        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = tool_main(["port-check", str(PORT / "shapebase_impact.port.lua"),
                              str(PORT / "shapebase_impact.spec.json"), "--original", str(SRC),
                              "--trace", str(PORT / "shapebase_impact.trace.json"),
                              "--expected", str(PORT / "shapebase_impact.expected.log")])
        self.assertEqual(code, 0, err.getvalue())
        res = json.loads(out.getvalue())
        self.assertEqual(res["structural_problems"], [])
        self.assertEqual(res["parity"], "match" if PARITY_EXE and Path(PARITY_EXE).is_file() else "harness not built")


if __name__ == "__main__":
    unittest.main()
