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
| `cmake/` | Platform helpers, sanitizers, `FuseVendor.cmake` — preset overview in [`cmake/README.md`](../cmake/README.md) |

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
| `Samples/unification/` | U8 parity demo `project.json` projects (see `Samples/Modules/` for script-only templates) |
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

## Vendor / reorg / licence migration (completion checklist)

Structural migration off Torque submodules, `third_party/`, and `Engine/` is **complete** for the FUSE product graph. Use this table when onboarding or reviewing PRs that touch paths or vendoring.

| Criterion | Expected state | Verified |
|-----------|----------------|----------|
| Product licence | AGPL-3.0 at repo root ([`LICENSE.md`](../LICENSE.md)); provenance in [`heritage.md`](heritage.md) | ✅ |
| Product source | All shipped C++ under `Source/FUSE/` (C++23) | ✅ |
| Vendored deps | Pinned trees under `vendor/` only; clone needs no extra submodule step | ✅ |
| Vendor CMake | Root includes [`cmake/FuseVendor.cmake`](../cmake/FuseVendor.cmake); targets use `FUSE_VENDOR_DIR` / `fuse_vendor_path` / `fuse_stb_include` | ✅ |
| No legacy product paths | No `third_party/` or `Engine/` directory; no `third_party` paths in product `CMakeLists.txt` / `cmake/*.cmake` (Relight package checks may mention `THIRD_PARTY.md` keys) | ✅ |
| Torque submodules | No [`.gitmodules`](../.gitmodules) — upstream ore is out-of-band | ✅ |
| User-facing docs | [`README.md`](../README.md), [`building.md`](building.md), this file describe `vendor/` + `Source/FUSE/` | ✅ |
| U8 parity samples | Seven `project.json` projects under [`Samples/unification/`](../Samples/unification/) (see [`Samples/unification/README.md`](../Samples/unification/README.md)) | ✅ |

**Optional hygiene (not blocking configure/build):**

- Root [`vcpkg.json`](../vcpkg.json) still names `torque3d` and is **not** consumed by the FUSE umbrella CMake graph (audio codecs are vendored under `vendor/xiph/`). Safe to delete or archive with unused `Tools/CMake/` Torque vcpkg ports when cleaning legacy Torque build assets.
- Consolidated repo-root `THIRD_PARTY.md` remains a future packaging doc; Relight ships [`Source/FUSE/Relight/THIRD_PARTY.md`](../Source/FUSE/Relight/THIRD_PARTY.md).

Working U0/U8 notes under [`docs/unification/`](unification/) and [`docs/plans/`](plans/) may still cite historical `third_party/Torque2D` paths as **external** reference — that is evidence, not an in-tree layout requirement.
