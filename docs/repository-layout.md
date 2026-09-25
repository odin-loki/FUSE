# Repository layout

FUSE is one product graph under `Source/FUSE/`. Everything else is content, tools, vendored deps, or reference samples.

## Product (built by default)

| Path | Role |
|------|------|
| `Source/FUSE/Core/` | L0 — types, handles, jobs, platform, log, B7 integration (`fuse_b7`) |
| `Source/FUSE/ECS/` | Entity-component-system + spatial (`ECS/Spatial/`) |
| `Source/FUSE/World2D/`, `World3D/`, `Hybrid/` | L2 dimension facades and compositor |
| `Source/FUSE/Renderer/`, `Compute/`, `ScreenSpace/` | Vulkan RHI, compute kernels, screen-space references |
| `Source/FUSE/Modules/` | L3 feature modules (AI, FX, cinematics, mechanics, adventure) |
| `Source/FUSE/Project/`, `Scene/` | `project.json`, converters, scene graph |
| `Source/FUSE/Editor/`, `Apps/` | Qt editor shell and headless demos |
| `Source/FUSE/Relight/` | Optional DXVK remaster stack (MinGW/Wine CI; default off on MSVC) |
| `Tools/FUSE/` | CLI — `fuse_import`, `fuse_cook`, `fuse_convert` |
| `cmake/` | Platform helpers, sanitizers, `FuseVendor.cmake`, presets |

**Language:** ISO C++23 for all targets under `Source/FUSE/` (`CMAKE_CXX_STANDARD 23` in `Source/FUSE/CMakeLists.txt`).

## Vendored libraries

| Path | Role |
|------|------|
| `vendor/` | Pinned third-party libraries — enet, lua, bullet, vma, dxvk, assimp, … |
| `vendor/stb/` | STB image headers |
| `vendor/ispc_texcomp/` | BC texture compression headers for the cook pipeline |

## Content and tests

| Path | Role |
|------|------|
| `Samples/` | U8 parity demo `project.json` projects |
| `Tests/` | Golden renders, Relight integration fixtures |
| `Content/` | Runtime profiles (Relight) |
| `Templates/` | Torque-era templates (reference) |

## CMake entry

```text
CMakeLists.txt (root)
  ├── cmake/FuseVendor.cmake
  ├── Source/FUSE/          ← product graph (C++23)
  └── Tools/FUSE/           ← CLI (when FUSE_BUILD_TOOLS)
```

See also [architecture.md](architecture.md) and [building.md](building.md).
