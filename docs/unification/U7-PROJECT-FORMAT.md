# FUSE U7 — Project Format + Converters (WP-09)

**Phase:** U7 content / converters  
**Date:** 2026-09-19  
**Status:** `project.json` v1 + `.fuselevel` v2 hierarchy + T2D runtime bridge + BC7 mode-6 cook encoder + encoder hooks (honest stubs)

---

## 1. Overview

FUSE projects are directories containing a versioned `project.json` manifest. The loader lives in `Source/FUSE/Project/` (`fuse_project` CMake target). Importers register **placeholder worlds** — they parse enough legacy metadata to assign `dimension::WorldHandle` values without marrying addon `Engine/` trees.

| Component | Path | Role |
|-----------|------|------|
| Manifest loader | `fuse/project/loader.hpp` | Parse `project.json` from directory or file |
| T3D mission importer | `fuse/project/importer.hpp` | `.mis` → World3D placeholder + `T3DMissionExtract` (SimObjects, materials, datablocks) |
| T2D module importer | `fuse/project/importer.hpp` | `main.cs` / `.cs` → World2D placeholder + `T2DModuleExtract` scene-node stubs |
| T2D runtime bridge | `fuse/project/t2d_module_bridge.hpp` | `bridgeT2DModuleToRuntime` → `World2D` sprites from toybox extract |
| Field extractors | `fuse/project/importer_extract.hpp` | Shared parsers for mission/module text |
| World converter | `fuse/project/world_converter.hpp` | `.mis` / `.cs` → `.fuselevel` (hierarchy-aware) |
| Asset cook stub writers | `Tools/FUSE/Cook/` (`fuse_cook_stubs`) | `FUSEMESH_STUB` / `FUSETEX_STUB` / `FUSEAUDIO_STUB` placeholder outputs |
| CLI dry-run | `Tools/FUSE/fuse_import` | Headless import validation |
| CLI convert | `Tools/FUSE/fuse_convert` | Legacy source → `.fuselevel` |
| Cook stub | `Tools/FUSE/Cook/fuselevel_cook_stub.*` | `fuse_cook --fuselevel` wrapper |

---

## 2. `project.json` schema (version 1)

```json
{
  "schemaVersion": 1,
  "name": "my_game",
  "dimensions": {
    "enable3D": true,
    "enable2D": true,
    "enableUI": true
  },
  "modules": {
    "ai": false,
    "cinematics": false,
    "fx": false,
    "mechanics": false,
    "adventure": false
  },
  "defaultWorld3D": "worlds/main.fuselevel",
  "defaultWorld2D": "worlds/ui.fuselevel"
}
```

| Field | Type | Notes |
|-------|------|-------|
| `schemaVersion` | `u32` | Must be `1` |
| `name` | string | Project identifier |
| `dimensions.*` | bool | Maps to `fuse::hybrid::DimensionFlags` |
| `modules.*` | bool | Enables U5 feature modules for this project |
| `defaultWorld3D` | string | Relative path to converted 3D world |
| `defaultWorld2D` | string | Relative path to converted 2D world |

Samples: `Samples/unification/*/project.json` — `demo_3d_empty` ships `worlds/example.mis` + cooked `worlds/example.fuselevel`.

---

## 3. `.fuselevel` schema (version 2 — hierarchy)

Binary format in `fuse::scene::SceneSerialiser`:

| Version | Layout |
|---------|--------|
| **v1** | Header + camera + name table + transform table (flat entities) |
| **v2** | v1 + **parent index table** (`s32` per entity, `-1` = root) written when any entity has a parent |

Loader accepts v1 and v2. Converter writes v2 when `.mis` nesting produces parent links (e.g. `SimGroup` → `SpawnSphere`).

---

## 4. Loader API

```cpp
#include <fuse/project/loader.hpp>

fuse::project::LoadResult result = fuse::project::loadFromDirectory("Samples/unification/demo_3d_empty");
if (result.status == fuse::project::LoadStatus::Ok) {
    fuse::hybrid::DimensionFlags flags =
        fuse::project::toDimensionFlags(result.manifest.dimensions);
}
```

`LoadStatus` values: `Ok`, `FileNotFound`, `ParseError`, `UnsupportedSchema`.

---

## 5. Importer + converter stubs

### T3D mission (`.mis`)

Recursively extracts nested `new Type(Name) { ... }` blocks with transforms. Emits `__fuse.wire|datablock|…` and `__fuse.wire|material|…` child entities for honest scene wiring stubs (parent-linked to the owning SimObject).

```cpp
fuse::project::ConvertResult result =
    fuse::project::convertT3DMissionToFuselevel("levels/ExampleLevel.mis", "worlds/main.fuselevel");
// result.wiringStubCount — datablock/material wiring entities written
```

Golden source: `Templates/BaseGame/game/data/ExampleModule/levels/ExampleLevel.mis`

### T2D module (`main.cs`)

Scans `new Type(Name)` toybox nodes with brace-depth hierarchy and optional `position = "x y"` into `.fuselevel` v2 parent indices.

```cpp
fuse::project::ConvertResult result =
    fuse::project::convertT2DModuleToFuselevel("main.cs", "worlds/ui.fuselevel");
```

Golden source: `third_party/Torque2D/toybox/SpriteToy/1/main.cs`

### Dry-run

```cpp
fuse::project::ImportDryRunResult result =
    fuse::project::importDryRun(manifest, sourcePath);
```

When `sourcePath` is empty, importers run against `defaultWorld3D` / `defaultWorld2D` from the manifest.

---

## 6. CLI

```bash
cmake --build build-fuse --target fuse_import fuse_convert fuse_cook

# Load project + dry-run default worlds
./build-fuse/Tools/FUSE/fuse_import --project Samples/unification/demo_3d_empty

# Convert legacy mission → .fuselevel
./build-fuse/Tools/FUSE/fuse_convert \
  --mis Samples/unification/demo_3d_empty/worlds/example.mis \
  --output Samples/unification/demo_3d_empty/worlds/example.fuselevel

# Cook stub (delegates to world converter)
./build-fuse/Tools/FUSE/fuse_cook \
  --fuselevel --mis path/to/level.mis --output worlds/out.fuselevel
```

Exit `0` on success; prints one line per registered world / convert note.

---

## 7. Tests

| Test | Target |
|------|--------|
| `fuse_project_tests` | Manifest parse, schema rejection, T3D/T2D importer stubs |
| `fuse_world_converter_tests` | `.mis` hierarchy + datablock wiring stubs; T2D toybox hierarchy + `bridgeT2DModuleToRuntime` |
| `fuse_bc7_encoder` | BC7 mode-6 block encode/decode + `FUSETEX_BC7` cook writer |
| `fuse_world2d_fuselevel_bridge` | T2D `.fuselevel` → `World2D::loadWorld` bridge |
| `fuse_scene_wire_stub` | `__fuse.wire|*` entity name parser |
| `fuse_scene_b37_b39` | Serialiser v1/v2 + hierarchy round-trip |

---

## 8. U8 parity demos

Each demo under `Samples/unification/<demo_id>/` ships a `project.json` consumed by its matching binary in `Source/FUSE/Apps/`. See [demo-corpus-parity-targets.md](./demo-corpus-parity-targets.md).

---

## 9. Cook stubs (`Tools/FUSE/Cook`)

`fuse_cook_stubs` (linked by `fuse_project`) writes placeholder binaries on `AssetCooker` cache miss. Each writer tries an optional real encoder hook first (`tryCookMeshAssimp`, `tryCookTextureBc7`, `tryCookAudioOgg`) and falls back to stub headers when libs are absent:

| Writer | Hook | Output marker |
|--------|------|---------------|
| `write_mesh_stub` | Assimp (`FUSE_HAS_ASSIMP`) — passes `input_path` | `FUSEMESH_STUB` |
| `write_texture_stub` | STB decode (`FUSE_HAS_STB_IMAGE`) + in-house BC7 mode-6 (`FUSE_HAS_INHOUSE_BC7_ENCODER`) | `FUSETEX_STUB` / `FUSETEX_BC7` |
| `write_audio_stub` | OGG Vorbis (`FUSE_HAS_OGG_VORBIS`) — WAV sniff stub when linked | `FUSEAUDIO_STUB` |

`fuselevel_cook_stub.*` populates `hierarchyLinks` from `ConvertResult::wiringStubCount` on `--fuselevel` cooks.

## 10. Wire runtime binding (wave 7 progress)

| API | Role |
|-----|------|
| `fuse::scene::populateLegacyTableFromScene` | Distill `__fuse.wire|datablock|*` / `material|*` into `LegacyDatablockTable` |
| `fuse::scene::applyWireBindingsToEcs` | Apply material wires to `ecs::Mesh::material_id` and datablock wires to `ecs::SpawnMarker::datablock_id` |
| `fuse::scene::applyWireBindingsFromScene` | One-shot: populate table from scene, create ECS entities for non-wire objects, apply bindings |
| `World2D::setProjectWorldSource` | `loadWorld()` resolves `projectRoot + defaultWorld2D` when no explicit fuselevel path is set |

Cook encoder hooks (`fuse_cook_stubs`): vendored Assimp fallback when system `libassimp-dev` is absent; STB RGBA decode + in-house BC7 mode-6 block payload (`fuse/cook/bc7_encoder.hpp`); optional ispc_texcomp when `third_party/ispc_texcomp` is present; OGG when `libvorbisenc` is linked.

CTest: `fuse_scene_wire_runtime_bind`, `fuse_world2d_fuselevel_bridge`, `fuse_assets_b79`.

## 11. Deferred (honest backlog)

- ispc_texcomp-quality BC7/BC5 compression replacing mode-6 solid-block stub
- libvorbisenc system package on CI images (runtime libs present; dev headers optional today)
- T2D toybox runtime parity (physics, layers, composite sprites beyond sprite bridge)
- Asset path remapping via VFS mounts ([vfs-mount-plan.md](./vfs-mount-plan.md))
- `project.json` `workerCap` override for `computeWorkerCount()`
