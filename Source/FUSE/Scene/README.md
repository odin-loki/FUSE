# fuse_scene — B3.5–B3.9 scene module

Phase 3 scene APIs from [FUSE Master Plan](../../docs/plans/FUSE_MASTER_PLAN.md). This module hosts **two complementary scene surfaces** that coexist in one library.

## APIs (both kept)

| API | Milestone | Purpose |
|-----|-----------|---------|
| `fuse::scene::Scene` | B3.7–B3.9 | Named scene + `fuse::Camera` + entity table (`SceneEntity` + `SceneEntityTransform`); serialised by `SceneSerialiser` / `project_io` |
| `fuse::scene::SceneSnapshot` | B3.7 deepen | In-memory capture/restore (`captureSnapshot` / `applySnapshot`) for play-mode and round-trip stubs |
| `fuse::scene::SceneManager` | B3.6 | ECS runtime container — `Registry` + `spatial::BVH` of every Mesh/SDF + optional `SVO` |
| `fuse::scene::SVO` | B3.5 | Sparse Voxel Octree — `set`/`get`/`fill`/`carve`/`sdfQuery`/`rayCast` |

### B3.5 — Sparse Voxel Octree (`svo.hpp`)

- Octree of 32-byte interior nodes ending in 8x8x8 brick leaves (O(depth) `get`/`set`); brick
  payloads live in one pooled word arena as sparse (index, material) pairs, a shared-material
  bitmask, a dense material array, or a single uniform material (whole-brick `fill`)
- 1M scattered voxels at depth 10: ~1.09M nodes / 32 MB (per-voxel leaves: ~4.06M nodes / ~195 MB);
  a 256^3 dense fill: ~1.2 MB. `memoryBytes()` reports the pooled capacity
- Per-voxel SDF plane allocated only for bricks a `carve` refined (`SVODesc::storeSdf`)
- `rayCast` is an exact 3D-DDA that jumps over empty octree cells and empty bricks
- GPU upload deferred — no renderer dependency

### B3.6 — Scene Manager (`scene_manager.hpp`)

- `SceneManager::init` / `update` / `createCamera` / `rayCast` / `querySphere` / `buildFrame`
- `update` runs `TransformSystem` and `CameraSystem`, then keeps a `spatial::BVH` of every Mesh/SDF
  entity (refit when the set is unchanged, rebuild on spawn/despawn)
- `buildFrame` culls against the active camera through that BVH and gathers draw items / SDF objects
  into `ecs::SceneData`; draw counts equal a brute-force cull (`fuse_b3_scene_gates`: 1000 mesh+SDF
  first build 0.63 ms, steady 0.15 ms Release)
- `bvh()` still exposes the legacy `bvh_stub.hpp` BVH for older callers
- Uses `fuse_ecs` registry and components (B3.1–B3.2)

### B3.7–B3.9 — Scene + Camera + Serialisation (from main)

- `fuse::Camera` — perspective matrices, frustum, `projectWorldPoint` / `worldToNdc` helpers (B3.8 stub)
- `SceneSerialiser` — binary `.fuselevel` round-trip (entity names + optional transform table)
- `SceneSnapshot` — entity + transform in-memory round-trip stub
- `saveForProject` / `loadForProject` — project-relative world paths

## Upstream dependencies

| Dependency | Owner | Path |
|------------|-------|------|
| `Registry`, ECS components | B3.1–B3.2 | `Source/FUSE/ECS/` |
| SAH `BVH` | B3.4 | `fuse::spatial::BVH` (`fuse/spatial/bvh.hpp`); legacy `include/fuse/scene/bvh_stub.hpp` |
| Math helpers | B1.4 / local | `include/fuse/scene/math.hpp` |
| Project manifest | U7 | `fuse_project` |

## Tests (`ctest`)

- `fuse_scene_b37_b39` — camera projection helpers, entity+transform serialiser, `SceneSnapshot`, project I/O
- `fuse_scene_svo` — SVO set/get, fill, carve, SDF, ray cast
- `fuse_b3_svo_gates`, `fuse_b3_scene_gates` — B3.9 gate rows (SVO round trip / ray cast / carve / SDF; scene build, cull, transform budgets)
- `fuse_scene_manager` — SceneManager init, ECS camera, update, queries

## Build

`fuse_scene` is built when `FUSE_BUILD_PROJECT=ON` (depends on `fuse_project` for serialisation I/O).
