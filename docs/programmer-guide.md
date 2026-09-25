# Programmer guide

Unified FUSE API surface for contributors and game authors. Points at real headers, tests, and parity demos — not aspirational stubs.

## Repository map

| Path | Library / target | Role |
|------|------------------|------|
| `Source/FUSE/Core` | `fuse_core` | Types, handles, jobs, log, platform |
| `Source/FUSE/World2D`, `World3D` | `fuse_world2d`, `fuse_world3d` | Dimension facades |
| `Source/FUSE/Hybrid` | `fuse_hybrid` | `HybridComposer`, placeholder renderer, cooked-asset bindings |
| `Source/FUSE/Modules/*` | `fuse_ai`, `fuse_fx`, … | L3 feature modules |
| `Source/FUSE/Project` | `fuse_project` | `project.json`, converters, VFS mounts |
| `Source/FUSE/Apps` | `demo_*`, `fuse_runtime_smoke` | Headless acceptance binaries |
| `Tools/FUSE` | `fuse_import`, `fuse_cook` | CLI importers and cookers |
| `Samples/unification` | — | U8 parity demo projects |

Namespace: `fuse::` for all new code. Legacy quarantine: `fuse::legacy::t3d` / `t2d` static libs only — do not link raw upstream trees into product targets.

Threading rules: [architecture.md](architecture.md). Coding rules: [coding-standards.md](coding-standards.md).

## Configure for development

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_PROJECT=ON \
  -DFUSE_BUILD_PARITY_DEMOS=ON \
  -DFUSE_BUILD_MODULES=ON

cmake --build build
ctest --test-dir build --output-on-failure
```

Sanitizers: `-DFUSE_SMOKE_ENABLE_ASAN=ON` links ASan on parity demos and smoke. Editor API + PIE embed: `-DFUSE_BUILD_EDITOR_API=ON` (default ON).

## Project loading

```cpp
#include <fuse/project/loader.hpp>

fuse::project::LoadResult project =
    fuse::project::loadFromDirectory("Samples/unification/demo_3d_empty");
```

`ProjectManifest` fields map to `fuse::hybrid::DimensionFlags` via `fuse::project::toDimensionFlags`.

Mount asset roots and apply worker cap (parity demos use this via `fuse::demo::wiring::prepareProjectRuntime` in `Source/FUSE/Apps/Common/demo_project_wiring.cpp`).

## World conversion (U7)

Converters live in `Source/FUSE/Project/src/world_converter.cpp`.

| API | Input | Output |
|-----|-------|--------|
| `convertT3DMissionToFuselevel` | `.mis` | `.fuselevel` scene + wiring stubs |
| `convertT2DModuleToFuselevel` | `.cs` module | `.fuselevel` + `animated_sprite` wire stubs |
| `ensureDefault3DWorldReady` | loaded project | converts golden/bundled `.mis` when `.fuselevel` missing |
| `ensureDefault2DWorldReady` | loaded project | converts golden/bundled `.cs` when `.fuselevel` missing |

Golden vs bundled resolution: `fuse::project::resolveParityLegacySource` in `parity_legacy_sources.cpp`. Submodule directories are classified locally (`classifySubmoduleDirectory`) — no network fetch in CI.

Tests: `ctest -R fuse_world_converter`, `ctest -R fuse_parity_legacy_sources`.

## Runtime wiring pattern (demos)

Parity apps share `fuse_demo_wiring`:

```cpp
#include "demo_project_wiring.hpp"

const auto runtime = fuse::demo::wiring::prepareProjectRuntime(project);
fuse::scene::Scene scene;
const auto world3D = fuse::demo::wiring::ensure3DWorldFromProject(project, scene);
fuse::world2d::World2D world2D;
const auto world2D = fuse::demo::wiring::bridge2DWorldFromProject(project, world2D);
```

`ensure3DWorldFromProject` chains `ensureDefault3DWorldReady`, scene load, and T3D datablock/material VFS resolve. `bridge2DWorldFromProject` chains `ensureDefault2DWorldReady`, optional runtime bridge (`bridgeT2DModuleToRuntime`), and `World2D::loadWorld`.

Reference implementations: `Source/FUSE/Apps/DemoFx/main.cpp` (hybrid 3D mission + 2D sockets), `Source/FUSE/Apps/DemoAiBt/main.cpp` (BT on both dims).

## Feature modules

Enable in `project.json` `modules.*`, then link the CMake target.

| Module | Headers | Demo | Key test prefix |
|--------|---------|------|-----------------|
| `fuse_ai` | `fuse/ai/` | `demo_ai_bt` | `fuse_ai_` |
| `fuse_cinematics` | `fuse/cinematics/` | `demo_timeline` | `fuse_cinematics` |
| `fuse_fx` | `fuse/fx/` | `demo_fx` | `fuse_fx_` |
| `fuse_mechanics` | `fuse/mechanics/` | `demo_adventure_stub` | `fuse_mechanics` |
| `fuse_adventure` | `fuse/adventure/` | `demo_adventure_stub` | `fuse_adventure` |

Module overview: [modules.md](modules.md). Hybrid integration gate: `hybrid_module_gates` in `Source/FUSE/Apps/HybridHud/hybrid_module_gates.cpp` — called from `demo_hybrid_hud`.

## Hybrid frame

```cpp
#include <fuse/hybrid/hybrid_composer.hpp>

fuse::hybrid::HybridComposer composer;
composer.setProjectFlags(flags);
composer.attachWorld3D(&world3D);
composer.attachWorld2D(&world2D);
composer.tick(ctx);
composer.render(ctx);  // software placeholder unless Track B present path wired
```

`HybridComposer::setSoftwarePlaceholderEnabled` — editor embed toggles when external swapchain is ready. Cooked mesh/SDF bindings: `fuse/hybrid/cooked_asset_bindings.hpp`.

## Editor embed (U6)

Qt-free API: `fuse_editor_api`. PIE smoke across all seven parity demos: `ctest -R fuse_u8_parity_embed_pie_smoke`.

`RuntimeEmbedSession` converts worlds before load, drains async material VFS, and mirrors ECS into `World3D` for viewport compositing. Details: [U6-EDITOR.md](unification/U6-EDITOR.md).

## Tools

```bash
cmake --build build --target fuse_import fuse_cook
./build/Tools/FUSE/fuse_import --help
./build/Tools/FUSE/fuse_cook --help
```

Cook pipeline stubs (mesh/texture/audio/shader) with cache skip: `Source/FUSE/Project/src/asset_cooker.cpp`. BC7 / OGG hooks are honest stubs until CI images ship real encoders.

## Adding a parity demo

1. Create `Samples/unification/demo_<name>/project.json` + `worlds/` bundled sources.
2. Add `Source/FUSE/Apps/Demo<Name>/` binary using `fuse_demo_wiring`.
3. Register golden paths in `parity_legacy_sources.cpp` (`goldenMissionPathForDemo` / `goldenModulePathForDemo`).
4. Add `fuse_u8_demo_<name>` test in `Source/FUSE/Apps/CMakeLists.txt`.
5. Extend `test_u8_parity_embed_pie.cpp` if editor PIE should cover it.
6. Document in [demo-corpus-parity-targets.md](unification/demo-corpus-parity-targets.md).

Minimum demo set must not shrink (prestarter §13.1).

## Exit criteria tracking

U8 unification exit gate: [FUSE_UNIFIED_PRESTARTER.md](plans/FUSE_UNIFIED_PRESTARTER.md) §13.2. Status snapshot: [demo-corpus-parity-targets.md](unification/demo-corpus-parity-targets.md).

## Internal engineering docs

Working packages and Track B deepen notes: [unification/](unification/). These are contributor references, not the product story in [README.md](README.md).
