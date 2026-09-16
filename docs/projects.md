# Projects

A FUSE project is a directory with a versioned `project.json`. The loader is `fuse_project` (`Source/FUSE/Project/`).

## `project.json` (schema 1)

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
| `schemaVersion` | number | Must be `1` |
| `name` | string | Project identifier |
| `dimensions.*` | bool | Maps to `fuse::hybrid::DimensionFlags` |
| `modules.*` | bool | Enables L3 feature modules |
| `defaultWorld3D` | string | Relative 3D world path (stub OK) |
| `defaultWorld2D` | string | Relative 2D world path (stub OK) |

Examples: `Samples/unification/*/project.json`.

## Loader API

```cpp
#include <fuse/project/loader.hpp>

fuse::project::LoadResult result =
    fuse::project::loadFromDirectory("Samples/unification/demo_3d_empty");

if (result.status == fuse::project::LoadStatus::Ok) {
    auto flags = fuse::project::toDimensionFlags(result.manifest.dimensions);
    (void)flags;
}
```

`LoadStatus`: `Ok`, `FileNotFound`, `ParseError`, `UnsupportedSchema`.

Also: `loadFromFile`, `parseManifest`.

## Importers

Importers register **placeholder worlds**. They parse enough metadata to assign `dimension::WorldHandle` values. They do not merge foreign engine trees.

| Source | Result |
|--------|--------|
| Mission / scene files | World3D placeholder |
| 2D module / sprite scripts | World2D placeholder |

CLI dry-run:

```bash
cmake --build build --target fuse_import
./build/Tools/FUSE/fuse_import --help
```

World files (`*.fuselevel`) are the native target format. Conversion is incremental; stubs are valid while authoring tools catch up.
