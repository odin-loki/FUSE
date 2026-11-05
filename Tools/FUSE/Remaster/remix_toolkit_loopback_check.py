"""W2.5 loopback check: the remix-toolkit client over real HTTP against the mock Toolkit on 127.0.0.1.

Runs outside the network-blocked test package (Tools/FUSE/Remaster/tests blocks sockets). Binds 127.0.0.1 port 0
only; nothing leaves the machine. Exit 0 on success, 1 on a failed check.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from fuse_ai.remix_toolkit import (MANIFEST_SCHEMA, MockRemixToolkit, RemixToolkitClient,  # noqa: E402
                                   RemixToolkitError, serve_mock)

MESH = "/RootNode/meshes/mesh_0123456789ABCDEF"
MAT = "/RootNode/Looks/mat_FEDCBA9876543210"


def main() -> int:
    failures = []

    def check(cond, what):
        print(("ok   " if cond else "FAIL ") + what)
        if not cond:
            failures.append(what)

    mock = MockRemixToolkit(assets=[MESH, MAT])
    server, thread, base = serve_mock(mock)
    try:
        client = RemixToolkitClient(base)
        summary = client.push_mod({
            "schema": MANIFEST_SCHEMA, "mod": "mod.usda",
            "replacements": [
                {"oaid": "a", "kind": "mesh", "prim": MESH, "remix_hash": "0123456789ABCDEF",
                 "mesh_layer": "meshes/mesh_0123456789ABCDEF.usda"},
                {"oaid": "b", "kind": "material", "prim": MAT, "remix_hash": "FEDCBA9876543210",
                 "textures": {"albedo": "textures/a.dds"}},
            ]})
        check(base.startswith("http://127.0.0.1:"), "mock bound to loopback (" + base + ")")
        check([a["prim"] for a in summary["applied"]] == [MAT, MESH], "both replacements applied over HTTP")
        check(mock.saved == ["mod.usda"] and mock.edit_target == "mod.usda", "layer created, targeted and saved")
        check(mock.textures["mod.usda"] == {MAT + "/Shader.inputs:diffuse_texture": "textures/a.dds"},
              "texture input set through the HTTP body")
        check(client.asset_textures(MAT) == {"inputs:diffuse_texture": "textures/a.dds"}, "textures read back")
        try:
            client.set_edit_target("nope.usda")
            check(False, "HTTP 404 maps to RemixToolkitError")
        except RemixToolkitError as e:
            check("HTTP 404" in str(e), "HTTP 404 maps to RemixToolkitError")
        try:
            serve_mock(host="0.0.0.0")
            check(False, "serve_mock refuses a non-loopback bind")
        except RemixToolkitError:
            check(True, "serve_mock refuses a non-loopback bind")
    finally:
        server.shutdown()
        server.server_close()
    print("PASSED" if not failures else f"FAILED ({len(failures)})")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
