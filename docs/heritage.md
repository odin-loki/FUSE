# Heritage

FUSE is an independent engine. Product APIs, docs, and new code use the `fuse` namespace and live under `Source/FUSE/`.

The repository is AGPL-3.0-licensed (same terms as [Cypha](https://github.com/odin-loki/Cypha)). Portions of the tree originate from earlier MIT-licensed simulation code; vendored third-party libraries keep their upstream licences under `vendor/`. See [`LICENSE.md`](../LICENSE.md).

That is the whole provenance story for users of FUSE.

## GitHub fork network

FUSE is a **standalone AGPL-3.0 product** (`Source/FUSE/`, [`LICENSE.md`](../LICENSE.md)). Git history may show ancestry from [TorqueGameEngines/Torque3D](https://github.com/TorqueGameEngines/Torque3D); that does not make FUSE a Torque3D distribution or submodule. The GitHub repo is **not** in the Torque3D fork network (`fork: false` on the REST API).

If you clone an old fork that still shows upstream linkage, a repo admin can detach via **Settings → General → Danger Zone → Leave fork network** (type the repository name to confirm). There is no public API for that operation.
