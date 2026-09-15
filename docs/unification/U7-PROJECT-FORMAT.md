# FUSE U7 — Project Format + Converters (WP-09)

**Phase:** U7 content / converters  
**Date:** 2026-09-15  
**Status:** Minimal `project.json` loader + honest importer stubs landed

---

## 1. Overview

FUSE projects are directories containing a versioned `project.json` manifest. The loader lives in `Source/FUSE/Project/` (`fuse_project` CMake target). Importers register **placeholder worlds** — they parse enough legacy metadata to assign `dimension::WorldHandle` values without marrying addon `Engine/` trees.

| Component | Path | Role |
|-----------|------|------|
| Manifest loader | `fuse/project/loader.hpp` | Parse `project.json` from directory or file |
| T3D mission importer | `fuse/project/importer.hpp` | `.mis` → World3D placeholder |
| T2D module importer | `fuse/project/importer.hpp` | `main.cs` / `.cs` → World2D placeholder |
| CLI dry-run | `Tools/FUSE/fuse_import` | Headless import validation |

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
| `defaultWorld3D` | string | Relative path to converted 3D world (stub ok) |
| `defaultWorld2D` | string | Relative path to converted 2D world (stub ok) |

Samples: `Samples/unification/*/project.json`

---

## 3. Loader API

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

## 4. Importer stubs

### T3D mission (`.mis`)

Parses the first `new Scene(NAME)` or `new SimGroup(NAME)` token. Registers a World3D placeholder with the parsed mission name.

```cpp
fuse::project::ImportRecord record = fuse::project::importT3DMission(path, worldIndex);
```

Golden source: `Templates/BaseGame/game/data/ExampleModule/levels/ExampleLevel.mis`

### T2D module (`main.cs`)

Parses `module "Name"` or `module @Name` declarations. Falls back to filename stem.

```cpp
fuse::project::ImportRecord record = fuse::project::importT2DModule(path, worldIndex);
```

Golden source: `third_party/Torque2D/toybox/SpriteToy/1/main.cs`

### Dry-run

```cpp
fuse::project::ImportDryRunResult result =
    fuse::project::importDryRun(manifest, sourcePath);
```

When `sourcePath` is empty, importers run against `defaultWorld3D` / `defaultWorld2D` from the manifest.

---

## 5. CLI

```bash
cmake --build build-fuse --target fuse_import

# Load project + dry-run default worlds
./build-fuse/Tools/FUSE/fuse_import --project Samples/unification/demo_3d_empty

# Dry-run a single legacy file
./build-fuse/Tools/FUSE/fuse_import --source Templates/BaseGame/game/data/ExampleModule/levels/ExampleLevel.mis
```

Exit `0` on success; prints one line per registered world.

---

## 6. Tests

| Test | Target |
|------|--------|
| `fuse_project_tests` | Manifest parse, schema rejection, T3D/T2D importer stubs |

---

## 7. U8 parity demos

Each demo under `Samples/unification/<demo_id>/` ships a `project.json` consumed by its matching binary in `Source/FUSE/Apps/`. See [demo-corpus-parity-targets.md](./demo-corpus-parity-targets.md).

---

## 8. Deferred (honest backlog)

- Real `.fuselevel` binary format + cookers under `Tools/FUSE/Cook/`
- Full T3D SimObject tree extraction from `.mis`
- T2D scene graph import from toybox modules
- Asset path remapping via VFS mounts ([vfs-mount-plan.md](./vfs-mount-plan.md))
- `project.json` `workerCap` override for `computeWorkerCount()`
