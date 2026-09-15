# Track B — ECS Core (B3.1–B3.9)

**Status:** B3.1 archetype registry + B3.2 core components + B3.3–B3.8 systems/spatial stubs + **B3.9 Phase 3 integration test suite**  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B3.1–B3.9  
**Threading:** [architecture-parallel.md](./architecture-parallel.md) §4

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `EntityID` | `include/fuse/ecs/entity.hpp` | Index + generation handle; stale detection O(1) |
| `Registry` | `include/fuse/ecs/registry.hpp` | Create/destroy, add/remove/get/has, `each` + `each_parallel` bulk iteration |
| `QueryFilter` / `With` / `Without` | `include/fuse/ecs/query_filter.hpp` | Archetype-scoped query filters; `each_query` / `each_query_parallel` (B3 deepen) |
| `Archetype` / `ComponentColumn` | `include/fuse/ecs/archetype.hpp` | SoA columns keyed by `std::type_index`; archetype migration on add/remove |
| `IsComponentV` trait | `include/fuse/ecs/component.hpp` | Plain data + `component_name` string (C++17) |
| Math types (`vec3`, `quat`, `mat4`) | `include/fuse/ecs/math/vec.hpp` | Minimal POD until shared `fuse/math` lands in Core |
| Core components | `include/fuse/ecs/components/` | Transform, Mesh, SDFObject, RigidBody, Camera, lights, tags |
| `TransformSystem` | `include/fuse/ecs/systems/transform_system.hpp` | Hierarchy + optional `each_parallel` dirty-root pass (B3.3) |
| `CullingSystem` | `include/fuse/ecs/systems/culling_system.hpp` | BVH + frustum cull for meshes/SDF/lights (B3.3) |
| `SceneBuildSystem` | `include/fuse/ecs/systems/scene_build_system.hpp` | `CullResult` → `SceneData` draw/SDF/light payloads (B3.3) |
| `CameraSystem` | `include/fuse/ecs/systems/camera_system.hpp` | Active camera view/proj/frustum (B3.8) |
| `SystemScheduler` | `include/fuse/ecs/system_scheduler.hpp` | Named system DAG + dependency order (B3.3) |
| `fuse::spatial::BVH` | `Source/FUSE/Spatial/include/fuse/spatial/bvh.hpp` | SAH build, ray/AABB/sphere/frustum queries (B3.4) |
| `fuse::scene::SceneManager` | `Source/FUSE/Scene/include/fuse/scene/scene_manager.hpp` | ECS runtime container + BVH/SVO stubs (B3.6) |
| `fuse::scene::SVO` | `Source/FUSE/Scene/include/fuse/scene/svo.hpp` | Voxel octree scaffold (B3.5) |

**Not in scope (follow-up):** CUDA-managed ECS columns, `each_parallel` parity harness at 100k scale (smaller parity test ships in `fuse_ecs_each_parallel`), GPU SDF buffer upload, renderer `SceneData` consumption, full `SceneManager::update` system wiring.

---

## Design (B3.1 sketch)

### Handle-based identity

```cpp
struct EntityID {
  u32 index;
  u32 generation;
  bool valid() const { return generation != 0; }
};
```

`Registry::alive()` compares generation against the slot record. Destroyed indices are recycled; generation increments on reuse.

### Archetype storage

Entities with the **same component signature** share one `Archetype`:

- Each component type is a `ComponentColumn` (byte vector + `element_size`).
- Adding/removing a component **migrates** the entity to a new archetype (swap-remove in source).
- `Registry::each<Transform, RigidBody>(fn)` scans archetypes whose signature is a superset of the requested types.

Empty archetype (index 0, hash 0) holds newly created entities before their first component is added.

### Phase 3 frame pipeline (B3.3 + B3.9)

```
SceneManager::init → Registry
       ↓
TransformSystem::update (hierarchy, parallel dirty roots)
       ↓
CameraSystem::update (active camera view/proj/frustum)
       ↓
spatial::BVH::build / refit (mesh AABB leaves)
       ↓
CullingSystem::cull (BVH frustum query + per-entity fallback)
       ↓
SceneBuildSystem::build → SceneData (draw items, SDF, lights)
       ↓
SceneManager::update (stub tick — systems wired in integration test)
```

`Registry::each_parallel` parallelizes row iteration per matching archetype via `JobScheduler::parallel_for` (default batch size 256). `TransformSystemOptions::parallelDirtyRoots` selects the parallel dirty-root pass (default on). `Registry::each_query` / `each_query_parallel` accept compile-time `With<...>` required types and an optional trailing `Without<...>` exclusion tag; `archetype_matches` evaluates filters before row iteration. Managed-memory columns at production scale remain **deferred** to B4+ physics/GPU paths. Current columns use `std::vector<std::byte>` on the CPU.

---

## B3.2 — Core component types

All components are plain data (`IsComponentV` + trivially destructible). No virtual methods.

| Component | Header | Notes |
|-----------|--------|-------|
| `Transform` | `components/transform.hpp` | Position/rotation/scale/parent + derived matrices (filled by `TransformSystem`) |
| `Mesh` | `components/mesh.hpp` | ECS-local `fuse::Handle` aliases — no `fuse_rhi` dependency |
| `SDFObject` | `components/sdf_object.hpp` | Primitive enum + params; GRIA α placeholder constant |
| `RigidBody` | `components/rigidbody.hpp` | SoA-friendly dynamics fields (solver in B4) |
| `Camera` | `components/camera.hpp` | Projection params + derived matrices/frustum |
| `DirectionalLight` / `PointLight` / `SpotLight` | `components/light.hpp` | Light payloads for future culling |
| `TagStatic` / `TagPlayer` / `TagDestroy` | `components/tags.hpp` | Zero-size marker components |

---

## Build

`fuse_ecs` builds with the umbrella by default (no extra CMake flag). Scene integration tests require `FUSE_BUILD_PROJECT=ON`.

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_PROJECT=ON \
  -DFUSE_BUILD_VULKAN=OFF

cmake --build build
ctest --test-dir build --output-on-failure -R 'fuse_ecs|fuse_scene'
```

| Condition | Behaviour |
|-----------|-----------|
| `FUSE_BUILD_VULKAN=OFF` | ECS unaffected — no renderer dependency |
| `FUSE_BUILD_CORE_TESTS=ON` | All ECS CTest targets registered |
| `FUSE_BUILD_PROJECT=ON` | `fuse_scene` + B3.9 `fuse_ecs_phase3_integration` target |

---

## Tests

| Target | Validates |
|--------|-----------|
| `fuse_ecs_registry` | Create/destroy, stale handles, add/get/remove, archetype migration, `each` |
| `fuse_ecs_query_filter` | `archetype_matches` With/Without filters; `each_query` serial/parallel coverage |
| `fuse_ecs_each_parallel` | `each_parallel` visit/mutation parity vs `each`; `TransformSystem` serial/parallel dirty-root paths |
| `fuse_ecs_components` | Component names, defaults, registry storage for lights/tags |
| `fuse_ecs_system_scheduler` | System dependency DAG execution order |
| `fuse_ecs_bvh` | SAH BVH ray cast, frustum query vs brute force, refit |
| `fuse_ecs_systems` | Transform hierarchy, camera + cull + scene-build pipeline |
| `fuse_ecs_phase3_integration` | **B3.9** end-to-end: `SceneManager` + `Registry` + all Phase 3 systems + `spatial::BVH` |
| `fuse_scene_manager` | SceneManager init, ECS camera, stub update, ray/sphere queries |
| `fuse_scene_svo` | SVO set/get, fill, carve, SDF query, ray cast |
| `fuse_scene_b37_b39` | Camera matrices/projection helpers, entity+transform serialiser round-trip, `SceneSnapshot` capture/restore, project I/O |

Run Phase 3 suite:

```bash
ctest --test-dir build --output-on-failure -R 'fuse_ecs|fuse_scene'
```

---

## Gates

### B3.1–B3.2 (registry + components)

- [x] **B3.1** `EntityID`, `Registry`, archetype SoA columns on FUSE APIs
- [x] **B3.2** Core component POD types (Transform, Mesh, SDF, RigidBody, Camera, lights, tags)
- [x] CTest targets green with `FUSE_BUILD_VULKAN=OFF`
- [ ] ASan/UBSan smoke on ECS tests (umbrella ASan job covers runtime smoke; ECS-specific ASan optional follow-up)
- [x] No owning raw pointers in public ECS APIs

### B3.3 — Core systems

- [x] **B3.3** `TransformSystem`, `CullingSystem`, `SceneBuildSystem`, `SystemScheduler` on FUSE APIs
- [x] Transform hierarchy propagates parent translation to children (`fuse_ecs_systems`)
- [x] System scheduler honours declared dependencies (`fuse_ecs_system_scheduler`)
- [x] `each<T>` iterates exactly the correct entities — no missed entities, no spurious iterations (`fuse_ecs_registry`)
- [x] `each_parallel<T>` produces identical results to `each<T>` across randomised cases (`fuse_ecs_each_parallel`; 100k scale deferred)
- [x] `QueryFilter` With/Without archetype matching + `each_query` / `each_query_parallel` coverage (`fuse_ecs_query_filter`)

### B3.4 — Bounding Volume Hierarchy

- [x] **B3.4** SAH `fuse::spatial::BVH` build, ray cast, frustum/sphere/AABB queries (`fuse_ecs_bvh`)
- [x] Frustum query matches brute-force on randomised leaf sets
- [x] Refit preserves node count after bounds update
- [ ] 100k AABB build < 500 ms perf gate (deferred — perf baseline harness)
- [ ] 10k-object frustum cull < 0.1 ms perf gate (deferred)

### B3.5 — Sparse Voxel Octree

- [x] **B3.5** `fuse::scene::SVO` scaffold — set/get, fill, carve, `sdfQuery`, `rayCast` (`fuse_scene_svo`)
- [ ] 1M voxel insert/get at depth 10 (deferred — scale test)
- [ ] SVO ray cast vs brute-force on 10k rays (deferred)

### B3.6 — Scene Manager

- [x] **B3.6** `SceneManager` owns `Registry` + BVH stub + optional `SVO` (`fuse_scene_manager`)
- [x] `createCamera` / `activeCamera` / `update` stub tick
- [x] `rayCast` / `querySphere` delegate to BVH/SVO stubs
- [ ] `SceneManager::update` wires real `TransformSystem` + `CameraSystem` (deferred — integration test wires explicitly today)

### B3.7 — Scene serialisation

- [x] **B3.7** `SceneSerialiser` + `project_io` round-trip (`fuse_scene_b37_b39`)
- [x] Entity + `SceneEntityTransform` table round-trip (header `reserved[1]='T'`; legacy name-only files load with identity transforms)
- [x] In-memory `SceneSnapshot::capture` / `apply` for play-mode restore stubs
- [ ] 10k-entity byte-identical save/load (deferred — scale test)

### B3.8 — Camera system

- [x] **B3.8** `CameraSystem::update` fills view, projection, view_projection, frustum for active camera
- [x] Camera frustum drives `CullingSystem` (`fuse_ecs_systems`, `fuse_ecs_phase3_integration`)
- [x] `fuse::Camera::projectWorldPoint` / `worldToNdc` projection helpers (`fuse_scene_b37_b39`)
- [ ] `update_free_camera` input controller (deferred — editor/debug follow-up)

### B3.9 — Phase 3 deliverables & integration

- [x] **B3.9** End-to-end test: `SceneManager` + `Registry` + `TransformSystem` + `CameraSystem` + `CullingSystem` + `spatial::BVH` + `SceneBuildSystem` (`fuse_ecs_phase3_integration`)
- [x] In-frustum mesh visible; off-frustum mesh culled; `SceneData` draw list matches visible set
- [x] Phase 3 CTest targets registered and green under umbrella CI
- [ ] Master-plan perf baselines (10k transform update, full scene build < 2 ms, renderer handoff) — deferred to B4/B5 integration

---

## Next

- [ ] Runtime query builder / cached query descriptors (deferred — compile-time With/Without stubs ship first)
- [ ] Wire `TransformSystem` + `CameraSystem` inside `SceneManager::update` (replace scaffold comments)
- [ ] Bridge `Mesh` handles → `renderer::BufferHandle` when scene submit lands
- [ ] Managed/pinned column allocators for CUDA physics path (B4)
- [ ] SimObject ↔ `EntityID` compat shim (dual-run during P4)
- [ ] Phase 3 perf baseline harness (100k entities, BVH build timing)

---

## Related docs

- [work-plan.md](./work-plan.md) — Track B ECS entry
- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B3
- [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) — parallel Track B renderer lane
- [Source/FUSE/Scene/README.md](../../Source/FUSE/Scene/README.md) — `SceneManager` + SVO module notes
