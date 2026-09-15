# fuse_scene — B3.5–B3.9 scene module

Phase 3 scene APIs from [FUSE Master Plan](../../docs/plans/FUSE_MASTER_PLAN.md). This module hosts **two complementary scene surfaces** that coexist in one library.

## APIs (both kept)

| API | Milestone | Purpose |
|-----|-----------|---------|
| `fuse::scene::Scene` | B3.7–B3.9 | Named scene + `fuse::Camera` + object table; serialised by `SceneSerialiser` / `project_io` |
| `fuse::scene::SceneManager` | B3.6 | ECS runtime container — `Registry` + BVH stub + optional `SVO` |
| `fuse::scene::SVO` | B3.5 | Sparse Voxel Octree — `set`/`get`/`fill`/`carve`/`sdfQuery`/`rayCast` |

### B3.5 — Sparse Voxel Octree (`svo.hpp`)

- Hash-backed leaf storage (scaffold) with octree node pool allocation
- SDF values stored at leaves when `SVODesc::storeSdf` is enabled
- GPU upload deferred — no renderer dependency

### B3.6 — Scene Manager (`scene_manager.hpp`)

- `SceneManager::init` / `update` / `createCamera` / `rayCast` / `querySphere`
- Uses `fuse_ecs` registry and components (B3.1–B3.2)
- **No** `build_render_data()` — renderer track owns draw-list conversion

### B3.7–B3.9 — Scene + Camera + Serialisation (from main)

- `fuse::Camera` — perspective matrices + frustum (B3.8 stub)
- `SceneSerialiser` — binary `.fuselevel` round-trip
- `saveForProject` / `loadForProject` — project-relative world paths

## Upstream dependencies

| Dependency | Owner | Path |
|------------|-------|------|
| `Registry`, ECS components | B3.1–B3.2 | `Source/FUSE/ECS/` |
| SAH `BVH` | B3.4 | `include/fuse/scene/bvh_stub.hpp` |
| Math helpers | B1.4 / local | `include/fuse/scene/math.hpp` |
| Project manifest | U7 | `fuse_project` |

## Tests (`ctest`)

- `fuse_scene_b37_b39` — camera, serialiser, project I/O
- `fuse_scene_svo` — SVO set/get, fill, carve, SDF, ray cast
- `fuse_scene_manager` — SceneManager init, ECS camera, update, queries

## Build

`fuse_scene` is built when `FUSE_BUILD_PROJECT=ON` (depends on `fuse_project` for serialisation I/O).
