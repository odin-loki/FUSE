# Heritage

FUSE is an independent engine. Product APIs, docs, and new code use the `fuse` namespace and live under `Source/FUSE/`.

The repository is AGPL-3.0-licensed (same terms as [Cypha](https://github.com/odin-loki/Cypha)). Portions of the tree originate from earlier MIT-licensed simulation code; vendored third-party libraries keep their upstream licences under `vendor/`. See [`LICENSE.md`](../LICENSE.md).

That is the whole provenance story for users of FUSE.

## GitHub fork network

FUSE is a **standalone AGPL-3.0 product** (`Source/FUSE/`, [`LICENSE.md`](../LICENSE.md)). Git history may show ancestry from TorqueGameEngines/Torque3D; that does not make FUSE a Torque3D distribution or submodule.

GitHub may still list this repository as a **fork** of [TorqueGameEngines/Torque3D](https://github.com/TorqueGameEngines/Torque3D). There is no public API to leave the fork network — only a repository admin can detach it.

**To detach (repo admin):** GitHub **Settings → General → Danger Zone → Leave fork network**, confirm by typing `FUSE`.
