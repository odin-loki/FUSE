# Heritage — upstream Torque3D reference tree

This directory holds the **Torque3D engine source** that predates FUSE. It is **not** built by the FUSE product CMake graph (`CMakeLists.txt` at the repo root).

| Path | Contents |
|------|----------|
| `torque3d/source/` | Full Torque3D `Engine/source` modules (T3D, gfx, gui, console, …) |
| `torque3d/modules/` | Torque modules (e.g. Verve) |
| `torque3d/bin/` | Legacy Windows binaries used only by optional CI deny-lists |
| `torque3d/CMakeLists.txt` | Standalone Torque3D build entry (addon workflows) |

FUSE product code lives under `Source/FUSE/`. Vendored third-party libraries live under `third_party/vendor/`.
