# FUSE U7 — Project Format + Converters (WP-09)

**Phase:** U7 content / converters  
**Date:** 2026-09-19  
**Status:** `project.json` v1 + `.fuselevel` v2 hierarchy + converter/cooker stubs

---

## 1. Overview

FUSE projects are directories containing a versioned `project.json` manifest. The loader lives in `Source/FUSE/Project/` (`fuse_project` CMake target). Importers register **placeholder worlds** — they parse enough legacy metadata to assign `dimension::WorldHandle` values without marrying addon `Engine/` trees.

| Component | Path | Role |
|-----------|------|------|
| Manifest loader | `fuse/project/loader.hpp` | Parse `project.json` from directory or file |
| T3D mission importer | `fuse/project/importer.hpp` | `.mis` → World3D placeholder |
| T2D module importer | `fuse/project/importer.hpp` | `main.cs` / `.cs` → World2D placeholder |
| World converter | `fuse/project/world_converter.hpp` | `.mis` / `.cs` → `.fuselevel` (hierarchy-aware) |
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

Recursively extracts nested `new Type(Name) { ... }` blocks with transforms. Skips root `Scene` in output entities but preserves parent indices for `SimGroup` children.

```cpp
fuse::project::ConvertResult result =
    fuse::project::convertT3DMissionToFuselevel("levels/ExampleLevel.mis", "worlds/main.fuselevel");
```

Golden source: `Templates/BaseGame/game/data/ExampleModule/levels/ExampleLevel.mis`

### T2D module (`main.cs`)

Parses `module "Name"` or `module @Name` declarations. Falls back to filename stem.

```cpp
fuse::project::ConvertResult result =
    fuse::project::convertT2DModuleToFuselevel("main.cs", "worlds/ui.fuselevel");
```

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
| `fuse_world_converter_tests` | `.mis` hierarchy → `.fuselevel` v2 round-trip |
| `fuse_scene_b37_b39` | Serialiser v1/v2 + hierarchy round-trip |

---

## 8. U8 parity demos

Each demo under `Samples/unification/<demo_id>/` ships a `project.json` consumed by its matching binary in `Source/FUSE/Apps/`. See [demo-corpus-parity-targets.md](./demo-corpus-parity-targets.md).

---

## 9. Deferred (honest backlog)

- Full T3D SimObject field extraction (materials, datablocks, spawn classes)
- T2D scene graph import from toybox modules
- Asset path remapping via VFS mounts ([vfs-mount-plan.md](./vfs-mount-plan.md))
- `project.json` `workerCap` override for `computeWorkerCount()`
- Real mesh/texture/audio cooks (stubs exist under `fuse_cook`; not production pipelines)
