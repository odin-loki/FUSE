"""W2.5: contract tests of the ``remix-toolkit`` provider against the in-process mock (network blocked)."""

import unittest

from tests import NETWORK_ATTEMPTS

from fuse_ai import providers
from fuse_ai.providers import Request, Task
from fuse_ai.remix_toolkit import (MANIFEST_SCHEMA, ROUTES, TEXTURE_INPUTS, MockRemixToolkit, RemixToolkitClient,
                                   RemixToolkitError, RemixToolkitProvider, check_base_url, is_loopback_host)

MESH = "/RootNode/meshes/mesh_0123456789ABCDEF"
MAT = "/RootNode/Looks/mat_FEDCBA9876543210"
MAT2 = "/RootNode/Looks/mat_00000000000000AA"


def manifest():
    return {
        "schema": MANIFEST_SCHEMA,
        "mod": "mod.usda",
        "replacements": [
            {"oaid": "b-oaid", "kind": "material", "remix_hash": "FEDCBA9876543210", "prim": MAT,
             "textures": {"normal": "textures/T_FEDCBA9876543210_n.dds", "albedo": "textures/T_FEDCBA9876543210_a.dds"}},
            {"oaid": "a-oaid", "kind": "mesh", "remix_hash": "0123456789ABCDEF", "prim": MESH,
             "mesh_layer": "meshes/mesh_0123456789ABCDEF.usda"},
        ],
    }


class LoopbackGuardTest(unittest.TestCase):
    def test_loopback_hosts(self):
        for h in ("127.0.0.1", "127.8.9.10", "::1", "[::1]", "localhost", "LOCALHOST", "::ffff:127.0.0.1"):
            self.assertTrue(is_loopback_host(h), h)
        for h in ("10.0.0.1", "192.168.1.2", "0.0.0.0", "example.com", "localhost.example.com", "::ffff:10.0.0.1", "",
                  None, "128.0.0.1"):
            self.assertFalse(is_loopback_host(h), h)

    def test_refused_base_urls(self):
        for url in ("http://10.0.0.1:8011", "http://example.com", "https://127.0.0.1:8011", "ftp://127.0.0.1",
                    "http://[::ffff:10.0.0.1]:8011", "http://user:pw@127.0.0.1:8011", "http://127.0.0.1:8011/?x=1",
                    "http://127.0.0.1:notaport", "127.0.0.1:8011"):
            with self.assertRaises(RemixToolkitError, msg=url):
                RemixToolkitClient(url, transport=MockRemixToolkit())

    def test_accepted_base_urls(self):
        self.assertEqual(check_base_url("http://127.0.0.1:8011/"), "http://127.0.0.1:8011")
        self.assertEqual(check_base_url("http://[::1]:8011"), "http://[::1]:8011")
        self.assertEqual(check_base_url("http://localhost:8011"), "http://localhost:8011")

    def test_default_transport_built_without_network(self):
        RemixToolkitClient()  # constructing the urllib transport opens nothing
        self.assertEqual(NETWORK_ATTEMPTS, [])


class RouteContractTest(unittest.TestCase):
    def setUp(self):
        self.mock = MockRemixToolkit(assets=[MESH, MAT, MAT2])
        self.c = RemixToolkitClient(transport=self.mock)

    def test_route_table_shape(self):
        self.assertEqual(len(ROUTES), 11)
        for name, (method, path) in ROUTES.items():
            self.assertIn(method, ("GET", "POST", "PUT"))
            self.assertTrue(path.startswith("/stagecraft/") or path.startswith("/ingestcraft/"), name)
            self.assertTrue(hasattr(MockRemixToolkit, "_h_" + name), name)

    def test_project_layers_edit_target(self):
        self.assertEqual(self.c.get_project(), {"layer_id": "project.usda"})
        self.assertEqual(self.c.list_layers(), ["project.usda", "capture/capture.usda"])
        lid = self.c.create_layer("mods/x.usda", "project.usda")
        self.assertEqual(lid, "mods/x.usda")
        self.assertEqual(self.mock.requests[-1][3],
                         {"layer_path": "mods/x.usda", "parent_layer_id": "project.usda", "set_edit_target": False,
                          "create_or_insert": True})
        self.c.set_edit_target(lid)
        self.assertEqual(self.c.get_edit_target(), "mods/x.usda")
        # path parameters are percent-encoded
        self.assertEqual(self.mock.requests[-2][1], "/stagecraft/layers/target/mods%2Fx.usda")
        self.c.save_layer(lid)
        self.assertEqual(self.mock.saved, ["mods/x.usda"])

    def test_capture_layer_cannot_be_edit_target(self):
        with self.assertRaisesRegex(RemixToolkitError, "HTTP 400"):
            self.c.set_edit_target("capture/capture.usda")

    def test_assets_query_flags(self):
        self.assertEqual(self.c.list_assets(), sorted([MESH, MAT, MAT2]))
        self.assertEqual(self.mock.requests[-1][2], {"selection": False, "filter_session_assets": False,
                                                     "asset_hashes": True})
        self.assertEqual(self.c.list_assets(selection=True), [])

    def test_textures_and_references(self):
        self.c.set_texture(MAT, TEXTURE_INPUTS["albedo"], "textures/a.dds")
        self.assertEqual(self.mock.requests[-1][3],
                         {"asset_file_paths": [[MAT + "/Shader.inputs:diffuse_texture", "textures/a.dds"]]})
        self.assertEqual(self.c.asset_textures(MAT), {"inputs:diffuse_texture": "textures/a.dds"})
        ref = self.c.set_mesh_reference(MESH, "meshes/m.usda")
        self.assertTrue(ref.startswith(MESH + "/ref_"))
        with self.assertRaisesRegex(RemixToolkitError, "HTTP 422"):
            self.c.set_texture(MAT, "inputs:diffuse_texture", "textures/a.png")  # not DDS
        with self.assertRaisesRegex(RemixToolkitError, "HTTP 400"):
            self.c.set_mesh_reference(MAT, "meshes/m.usda")  # not a mesh prim
        with self.assertRaisesRegex(RemixToolkitError, "HTTP 404: unknown asset"):
            self.c.asset_textures("/RootNode/Looks/mat_0000000000000001")

    def test_error_mapping(self):
        self.mock.fail["get_project"] = (500, {"detail": "boom"})
        with self.assertRaisesRegex(RemixToolkitError, "HTTP 500: boom"):
            self.c.get_project()
        from fuse_ai.remix_toolkit import _NotJson
        self.mock.fail["get_project"] = (200, _NotJson(b"<html>"))
        with self.assertRaisesRegex(RemixToolkitError, "non-JSON"):
            self.c.get_project()
        self.mock.fail["list_layers"] = (200, {"layer_ids": "nope"})
        with self.assertRaisesRegex(RemixToolkitError, "malformed layers"):
            self.c.list_layers()
        self.mock.fail["list_layers"] = (200, {"layer_ids": [1, 2]})
        with self.assertRaisesRegex(RemixToolkitError, "strings"):
            self.c.list_layers()
        self.mock.fail["list_assets"] = (200, [])
        with self.assertRaisesRegex(RemixToolkitError, "malformed assets"):
            self.c.list_assets()
        self.mock.fail["asset_textures"] = (200, {"textures": [["only-one"]]})
        with self.assertRaisesRegex(RemixToolkitError, r"\[input, path\]"):
            self.c.asset_textures(MAT)

    def test_unknown_route_is_404(self):
        self.assertEqual(self.mock("GET", "/stagecraft/nope/", {}, None)[0], 404)
        self.assertEqual(self.mock("POST", "/ingestcraft/mass-validator/queue/material", {}, {})[0], 400)


class PushModTest(unittest.TestCase):
    def run_push(self):
        mock = MockRemixToolkit(assets=[MESH, MAT, MAT2])
        client = RemixToolkitClient(transport=mock)
        return mock, client, client.push_mod(manifest())

    def test_end_to_end(self):
        mock, client, summary = self.run_push()
        self.assertIn("mod.usda", mock.layers)
        self.assertEqual(mock.edit_target, "mod.usda")
        self.assertEqual(mock.saved, ["mod.usda"])
        self.assertEqual(mock.references["mod.usda"], {MESH: "meshes/mesh_0123456789ABCDEF.usda"})
        self.assertEqual(mock.textures["mod.usda"], {
            MAT + "/Shader.inputs:diffuse_texture": "textures/T_FEDCBA9876543210_a.dds",
            MAT + "/Shader.inputs:normalmap_texture": "textures/T_FEDCBA9876543210_n.dds",
        })
        self.assertEqual([a["prim"] for a in summary["applied"]], [MAT, MESH])  # sorted by prim
        self.assertEqual(summary["applied"][0]["textures"], ["albedo", "normal"])
        self.assertEqual(summary["missing"], [])
        self.assertEqual(summary["layer_id"], "mod.usda")
        self.assertEqual([m for m, _ in client.calls],
                         ["GET", "GET", "POST", "PUT", "GET", "PUT", "PUT", "PUT", "POST"])
        self.assertEqual(NETWORK_ATTEMPTS, [])

    def test_deterministic(self):
        _, c1, s1 = self.run_push()
        m2 = MockRemixToolkit(assets=[MESH, MAT, MAT2])
        man = manifest()
        man["replacements"].reverse()
        c2 = RemixToolkitClient(transport=m2)
        s2 = c2.push_mod(man)
        self.assertEqual(s1, s2)
        self.assertEqual(c1.calls, c2.calls)

    def test_existing_layer_reused_and_missing_reported(self):
        mock = MockRemixToolkit(assets=[MAT])
        mock.layers.append("mod.usda")
        s = RemixToolkitClient(transport=mock).push_mod(manifest())
        self.assertEqual(s["missing"], [MESH])
        self.assertEqual([a["prim"] for a in s["applied"]], [MAT])
        self.assertFalse(any(r[0] == "POST" and r[1] == "/stagecraft/layers/" for r in mock.requests))

    def test_bad_manifests(self):
        c = RemixToolkitClient(transport=MockRemixToolkit(assets=[MESH, MAT]))
        with self.assertRaisesRegex(RemixToolkitError, "schema"):
            c.push_mod({"schema": "x", "replacements": []})
        with self.assertRaisesRegex(RemixToolkitError, "list"):
            c.push_mod({"schema": MANIFEST_SCHEMA, "replacements": {}})
        bad = manifest()
        bad["replacements"][1].pop("mesh_layer")
        with self.assertRaisesRegex(RemixToolkitError, "mesh_layer"):
            c.push_mod(bad)
        bad = manifest()
        bad["replacements"][0]["textures"] = {"sparkle": "x.dds"}
        with self.assertRaisesRegex(RemixToolkitError, "unknown texture slot"):
            RemixToolkitClient(transport=MockRemixToolkit(assets=[MESH, MAT])).push_mod(bad)


class ProviderTest(unittest.TestCase):
    def test_registered_link_provider(self):
        p = providers.get("remix-toolkit")
        self.assertIsInstance(p, RemixToolkitProvider)
        self.assertIsInstance(p, providers.Provider)
        self.assertEqual(p.capabilities(), set())
        self.assertFalse(providers.info("remix-toolkit").cloud)
        req = Request(task=Task.UPSCALE, provider_id="remix-toolkit")
        self.assertEqual(p.vram_mb(req), 0)
        self.assertEqual(p.estimate(req).usd, 0.0)
        with self.assertRaisesRegex(RemixToolkitError, "live link"):
            p.run(req)
        c = p.client(transport=MockRemixToolkit())
        self.assertEqual(c.get_project()["layer_id"], "project.usda")
        with self.assertRaises(RemixToolkitError):
            p.client("http://example.com:8011")


if __name__ == "__main__":
    unittest.main()
