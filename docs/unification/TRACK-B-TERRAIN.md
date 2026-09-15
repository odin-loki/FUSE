# Track B — Terrain System (B7.5 deepen)

**Status:** B7.5 deepen — LOD residency set, skirt/morph stubs, budget clamp helpers, residency queue priority/budget  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.5  
**Source narrative:** [P7.md](../sources/P7.md) §7.5

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `TerrainDesc` / `TerrainChunk` | `Source/FUSE/Terrain/include/fuse/terrain/terrain_desc.hpp` | Resolution, world size, LOD count; `morph_factor` + `ChunkResidencyState` per chunk |
| `Heightfield` | `Source/FUSE/Terrain/include/fuse/terrain/heightfield.hpp` | CPU heightmap, bilinear `sample_height` / `sample_normal` |
| `LodLevel` / `LodTransition` / `AdjacentLodPair` | `Source/FUSE/Terrain/include/fuse/terrain/lod.hpp` | Distance-based LOD rings + morph band + adjacent LOD pair |
| `clamp_morph_factor` / `blend_morph_between_lods` | `Source/FUSE/Terrain/src/lod.cpp` | Morph clamp + blend between fine/coarse rings |
| `clamp_adjacent_lod_pair` / `blend_adjacent_lod_morph` | `Source/FUSE/Terrain/src/lod.cpp` | Adjacent LOD pair morph clamp + factor blend |
| `compute_lod_mesh_vertex_counts` | `Source/FUSE/Terrain/src/lod.cpp` | CPU stub — grid + skirt + seam vertex budgets per LOD |
| `LodSkirtParams` / `clamp_skirt_params` | `Source/FUSE/Terrain/src/lod.cpp` | Skirt depth/segment clamp + strip vertex count stub |
| `morph_vertex_position` | `Source/FUSE/Terrain/src/lod.cpp` | CPU stub — snap XZ toward coarser grid |
| `LodResidencySet` | `lod_residency_set.hpp` | Focus-distance resident chunk set with eviction ordering |
| `LodResidencyBudget` / clamp helpers | `lod_residency_budget.hpp` | `clamp_lod_level`, resident headroom, tick/pending budget clamps |
| `LodResidencyQueue` | `lod_residency_queue.hpp/.cpp` | Mutex-backed completion buffer; enqueue promote/demote, budget clamp, priority drain |
| `promote_residency_priority` / `demote_residency_priority` | `lod_residency_queue.cpp` | Pending load priority raise/lower stubs |
| `capture_morph_snapshot` / `sync_morph_after_residency` | `lod_residency_queue.cpp` | Keep morph in sync across async promotion/demotion |
| `ChunkGrid` | `Source/FUSE/Terrain/include/fuse/terrain/chunk_grid.hpp` | Clipmap ring LOD update, morph-factor tracking, async residency queue |
| `sample_height` / `raycast_heightfield` | `Source/FUSE/Terrain/include/fuse/terrain/queries.hpp` | Query APIs for gameplay and physics |
| `Terrain` | `Source/FUSE/Terrain/include/fuse/terrain/terrain.hpp` | Facade: generate, deform, LOD, visible chunks |

**Not in scope:** GPU heightmap textures, vertex-shader displacement, `fuse_scene::SVO` cave integration, async chunk asset I/O, runtime mesh rebuild, legacy Torque terrain cook.

---

## Design

### Heightfield surface

The surface is a regular grid of `f32` heights in metres. `Heightfield::sample_height` bilinearly interpolates world XZ coordinates. `Terrain::generate` fills the grid with deterministic hash noise for smoke tests.

### Clipmap LOD ring transitions

`compute_lod_transition` maps camera-to-chunk distance to a discrete LOD ring and a morph factor:

| Field | Meaning |
|-------|---------|
| `lod` | Discrete ring index (0 = finest) |
| `morph_factor` | [0, 1] blend toward coarser vertex grid in the outer 25% of each ring |

Ring width is 8 m (stub constant). `select_lod_level` delegates to `compute_lod_transition` for backward compatibility.

### Vertex morph stub

`morph_vertex_position` snaps vertex XZ toward the coarser LOD grid:

```
grid = base_stride * 2^lod
morphed = lerp(position, round(position / grid) * grid, morph_factor)
```

Y (height) is preserved — GPU heightmap displacement handles vertical detail later. `ChunkGrid::update_lod` writes `morph_factor` onto each `TerrainChunk` and marks chunks `dirty` when LOD changes or morph is active.

`make_adjacent_lod_pair` exposes the fine/coarse ring pair for a transition; `blend_morph_between_lods` lerps XZ toward the coarser grid using `clamp_adjacent_lod_pair`. `blend_adjacent_lod_morph` blends morph factors with `[0, 1]` clamp on both endpoints and blend weight.

### LOD residency set

`LodResidencySet` tracks resident chunk indices with planar focus distance (mirrors B7.6 `ResidencySet`). `ChunkGrid` adds/removes entries on load/unload and refreshes focus distance each `update_lod`. `pick_eviction_candidate` / `collect_eviction_candidates` return farthest-first eviction order for future byte/cell cap pressure.

### Budget clamp helpers

`lod_residency_budget.hpp` exposes `clamp_lod_level`, `resident_chunk_headroom`, `can_accept_resident_chunk`, `effective_tick_budget`, and `clamp_pending_submits`. `ChunkGrid::process_queues_` uses these when enforcing `TerrainDesc::max_resident_chunks` and per-tick submission caps.

### Skirt / seam vertex budget (CPU stub)

`compute_lod_mesh_vertex_counts` estimates displaced-grid vertices, four-edge skirts (with optional `LodSkirtParams` segment multiplier), and optional T-junction seam verts when a neighbor differs by `seam_neighbor_lod_delta`:

| Bucket | Formula (stub) |
|--------|----------------|
| `grid_vertices` | `(cells + 1)²` where `cells = chunk_resolution / 2^lod` |
| `skirt_vertices` | `4 × (cells + 1)` |
| `seam_vertices` | `4 × seam_neighbor_lod_delta × (cells + 1)` when delta > 0 |

### LOD residency lifecycle

```
Unloaded ──queue──▶ QueuedLoad ──submit──▶ Loading ──drain──▶ Resident
   ▲                                              │
   │                                              │
   └── drain ◀── Unloading ◀── submit ◀── QueuedUnload
```

Game thread owns `TerrainChunk` state. Worker threads only run the `LodResidencyWorkFn` stub (disk read simulation in production). Camera distance drives load/unload with hysteresis (`load_radius` vs `load_radius * 1.25`). Each async request carries a `LodResidencyMorphSnapshot`; `sync_morph_after_residency` reapplies morph when the chunk LOD still matches the snapshot at completion.

### Async residency queue

Mirrors B7.6 `StreamingRequestQueue`:

```cpp
#include <fuse/terrain/chunk_grid.hpp>

fuse::terrain::ChunkGrid grid;
fuse::terrain::TerrainDesc desc{};
desc.async_loading = true;
desc.max_async_in_flight = 4;
grid.init(desc);

grid.update_lod(camera_pos, dt);       // drains completions, queues new work
grid.drain_completed_requests();       // explicit drain (also called from update_lod)
const fuse::u32 inFlight = grid.in_flight_request_count();
```

`LodResidencyQueue` can also be exercised directly in tests or tooling:

```cpp
fuse::terrain::LodResidencyQueue queue;
queue.set_max_pending_submits(4);
queue.enqueue({chunk_index, fuse::terrain::LodResidencyRequestKind::Load, priority});
queue.demote(chunk_index, fuse::terrain::LodResidencyRequestKind::Load, 0.5f);
queue.flush(budget, [](fuse::u32, fuse::terrain::LodResidencyRequestKind) {
    return true; // worker I/O stub
});
queue.submit({chunk_index, fuse::terrain::LodResidencyRequestKind::Load, priority},
             [](fuse::u32, fuse::terrain::LodResidencyRequestKind) {
                 return true; // worker I/O stub
             });
```

Pending requests dedupe by `(chunk_index, kind)` and promote priority on re-enqueue. `set_max_pending_submits` rejects direct `submit` when in-flight + completed buffer reaches the cap (mirrors B7.6 `StreamingRequestQueue`). `drain_completed` returns highest-priority completions first.

### Queries

`raycast_heightfield` marches along a ray in world-space steps derived from texel size and reports the first crossing below the heightfield surface.

### Future unification

| Legacy | FUSE target |
|--------|-------------|
| `Engine/source/terrain/*` heightfield blocks | `fuse::terrain::Heightfield` + GPU displacement via `fuse_rhi` |
| Underground / cave volumes | `fuse::scene::SVO` carved below the heightfield (B3.5) |
| Editor terrain tools | U6 editor commands over `Terrain::deform` / layer paint |

---

## Build

`fuse_terrain` builds with the FUSE umbrella (always on). Tests register as `fuse_terrain_b75`.

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON

cmake --build build
ctest --test-dir build --output-on-failure -R fuse_terrain
```

| Option | Effect |
|--------|--------|
| `async_loading = false` | Synchronous queue drain on update |
| `max_async_in_flight` | Cap concurrent JobScheduler submissions per grid |
| `load_radius = 0` | Default `world_size * 0.75` |
| Single-threaded scheduler | Falls back to synchronous execute path |

---

## Tests

`Source/FUSE/Terrain/tests/test_terrain.cpp` (`fuse_terrain_b75`):

| Test | Gate |
|------|------|
| `testHeightfieldSampling` | Bilinear height and flat normal |
| `testLodSelection` | Distance-based LOD and stride growth |
| `testLodTransitionMorphBand` | Morph ramp, ring edge, and boundary reset |
| `testMorphFactorClamp` | `clamp_morph_factor` and snapshot clamp |
| `testAdjacentLodPair` | Fine/coarse pair + `blend_morph_between_lods` |
| `testAdjacentLodMorphBlend` | `clamp_adjacent_lod_pair` + `blend_adjacent_lod_morph` |
| `testLodResidencySetAddRemove` | Residency set add/remove/update/clear/eviction order |
| `testLodClampHelpers` | `clamp_lod_level` + resident/tick budget clamps |
| `testLodSkirtStubs` | Skirt param clamp + segment vertex multiplier |
| `testEmptyTerrainResidency` | Empty residency set + far-camera zero residents |
| `testLodMeshVertexCounts` | Grid/skirt/seam vertex budget sanity |
| `testResidencyMorphSync` | `sync_morph_after_residency` on LOD match/mismatch |
| `testResidencyPriorityPromoteDemote` | Priority raise/lower helper stubs |
| `testVertexMorphSnapsToGrid` | Full/half morph, LOD 0 skip, zero morph |
| `testChunkResidencyStateHelpers` | `is_*_state` predicates |
| `testLodResidencyQueueEnqueuePromoteDemote` | Pending enqueue dedupe + demote |
| `testLodResidencyQueueStub` | Submit, worker execution, drain |
| `testLodResidencyQueueDrainOrdering` | Highest-priority completion first |
| `testLodResidencyQueueBudgetReject` | `max_pending_submits` rejects overflow submit |
| `testLodResidencyQueueFlushBudget` | `flush` respects budget and priority order |
| `testChunkGridLodTransitions` | Sync residency + per-chunk morph factors |
| `testChunkGridAsyncResidency` | `Loading` → `Resident` → `Unloading` → `Unloaded` with 1 worker |
| `testHeightfieldRaycast` | Ray hits flat heightfield |
| `testTerrainFacade` | Generate, deform, raycast, visible chunks with morph |

---

## Gates (B7.5 deepen)

- [x] `LodTransition` struct with LOD ring + morph factor
- [x] `compute_lod_transition` clipmap ring heuristic
- [x] `morph_vertex_position` CPU stub (XZ snap toward coarser grid)
- [x] `ChunkGrid` tracks per-chunk `morph_factor`
- [x] `ChunkResidencyState` lifecycle helpers
- [x] `LodResidencyQueue` JobScheduler async stub
- [x] `ChunkGrid` game-thread drain applies residency transitions
- [x] Expanded morph factor tests (ramp, interpolation, boundary)
- [x] `AdjacentLodPair` + `blend_morph_between_lods` helpers
- [x] `compute_lod_mesh_vertex_counts` skirt/seam vertex stub
- [x] Residency queue morph snapshot + `sync_morph_after_residency`
- [x] Residency queue promote/demote priority + budget clamp + priority drain
- [x] `LodResidencySet` focus-distance tracking + eviction candidate ordering
- [x] `LodResidencyBudget` clamp helpers (`clamp_lod_level`, resident headroom)
- [x] Skirt param clamp + segment vertex multiplier stub
- [x] Adjacent LOD morph clamp + factor blend helpers
- [x] `fuse_terrain_b75` CTest target green
- [x] No owning raw pointers in public FUSE APIs

---

## References

- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.5
- [P7.md](../sources/P7.md) §7.5
- [TRACK-B-WORLD-PARTITION.md](./TRACK-B-WORLD-PARTITION.md) — B7.6 residency queue pattern
- Torque clipmap terrain in `Engine/source/terrain/`
