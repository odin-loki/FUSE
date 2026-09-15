# Track B — Physics (B4.1–B4.11 scaffolds)

**Status:** B4.7–B4.11 scaffolds on main (B4.1–B4.6 foundation merged via #27, #29)  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B4.1–B4.11  
**Architecture:** [architecture-parallel.md](./architecture-parallel.md) §3.2 (CUDA physics stream)  
**ECS integration:** `fuse::ecs::EntityID` from B3.1; `RigidBodySoA` vector SoA from B4.1

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `PhysicsPipeline` / `RigidBodySoA` | `physics_pipeline.hpp`, `physics_data.hpp` | B4.1–B4.3 broad/narrow phase stubs |
| `SpatialHash` / `Gjk` | `broadphase/`, `narrowphase/` | B4.2–B4.3 CPU reference paths |
| `PhysicsWorld2D` / `PhysicsWorld3D` | `physics_world_*.hpp` | World composition hooks |
| `DestructionSystem` / `DestructionEvent` | `destruction/` | B4.7 SVO carve → debris spawn scaffold |
| `Svo` | `Source/FUSE/Physics/include/fuse/physics/spatial/` | Minimal carve/query until Track A B3.5 |
| `ParticleSoA` / `ClothSimulator` | `Source/FUSE/Physics/include/fuse/physics/softbody/` | Host XPBD stub; CUDA kernels deferred |
| `PhysicsManager` | `Source/FUSE/Physics/include/fuse/physics/physics_manager.hpp` | ECS ↔ SoA bridge scaffold |
| `CollisionEventSystem` | `Source/FUSE/Physics/include/fuse/physics/events/` | Enter/Stay/Exit/Trigger callback bus |
| `Phase4TestRegistry` | `Source/FUSE/Physics/include/fuse/physics/phase4_test_registry.hpp` | B4.11 checklist + automated smoke |
| `PbdSolver` | `solver/pbd_solver.hpp` | B4.4 stub (B4.4–B4.6 owned by sibling PR #29) |

**Composed APIs:** B4.7–B4.11 use `fuse::ecs::EntityID`, `fuse::physics::vec3`/`quat` from `math.hpp`, and B4.1 `RigidBodySoA` vectors — no duplicate entity/math types.

**Not in scope (other workstreams):** B4.4–B4.6 CUDA PBD/CCD kernels, dual contouring GPU mesh extraction, full ECS `Registry` writeback, Bullet reference tests.

---

## Build flag — `FUSE_BUILD_PHYSICS`

```bash
cmake -B build -DFUSE_UMBRELLA=ON -DFUSE_BUILD_PHYSICS=ON -DFUSE_BUILD_CORE_TESTS=ON
```

| Condition | Behaviour |
|-----------|-----------|
| `FUSE_BUILD_PHYSICS=OFF` | No `fuse_physics` target |
| `FUSE_BUILD_PHYSICS=ON` | `fuse_physics` static lib + narrative tests |
| CI Linux umbrella | `FUSE_BUILD_PHYSICS=ON` — no CUDA toolkit required |

CMake discovers physics in `Source/FUSE/CMakeLists.txt`; tests register when `FUSE_BUILD_CORE_TESTS=ON`.

---

## B4.7 — Voxel Destruction (scaffold)

Destruction pipeline sketch:

1. `DestructionEvent` queued (impulse, impact point/normal, carve radius)
2. `Svo::carve()` removes voxels in sphere (stub bookkeeping)
3. `Svo::extractMeshRegion()` returns placeholder quad mesh
4. `DestructionSystem::spawnDebris()` creates debris entity + mesh allocation counters

```cpp
f32 radius = DestructionSystem::deriveCarveRadius(impulse, material);
if (DestructionSystem::shouldDestroy(impulse, material)) {
  DestructionSystem::processEvents(events, registry, resources);
}
```

Full dual contouring, fragment flood-fill clustering, and GPU debris rigid-body creation land with B3.5 SVO + B4.4 solver integration.

---

## B4.8 — Soft Body & Cloth (scaffold)

| Piece | Path | Behaviour |
|-------|------|-----------|
| `ParticleSoA` | `softbody/particle_system.hpp` | Host allocate/free; GPU pointers reserved |
| `ClothSimulator` | `softbody/cloth_simulator.hpp` | Grid init, pinned corners, CPU XPBD predict + distance constraints |
| CUDA kernels | deferred | `cloth_kernels.cuh` from master plan — not vendored in this PR |

Cloth exposes `vertexBuffer` / `indexBuffer` handle placeholders for renderer consumption (B2.6 interop follow-up).

---

## B4.9 — Physics Manager & ECS Integration (scaffold)

`PhysicsManager::step()` lifecycle (CPU stub):

1. `syncEcsToSoa_()` — map `PhysicsRegistry` entity count into SoA slots
2. `PbdSolver::step()` — gravity integration stub
3. `syncSoaToEcs_()` — no-op until managed-memory writeback
4. `processDestructionEvents_()` — delegates to `DestructionSystem`

Query/force APIs (`rayCast`, `applyImpulse`, `setKinematicTarget`) return plausible stub results for narrative tests.

`PhysicsStreamManager` holds opaque `physicsStream` pointer — wires to `fuse::renderer::cuda::StreamManager` when B2.6 + B4.1 land.

---

## B4.10 — Collision Callbacks & Event System (scaffold)

```cpp
enum class CollisionEventType : u8 { Enter, Stay, Exit, Trigger };

class CollisionEventSystem {
  void registerCallback(EntityId entity, CollisionCallback cb);
  void dispatch(const std::vector<CollisionEvent>& events);
};
```

Callbacks keyed by `EntityId::index()`. Both `entityA` and `entityB` receive dispatch when registered. Solver contact manifold → event translation deferred to B4.9+B4.3 integration.

---

## B4.11 — Phase 4 Deliverables & Test Suite

`Phase4TestRegistry::catalog()` enumerates the master-plan checklist (broad phase through performance baselines). Automated smoke (`runAutomatedSmoke()`) exercises destruction radius derivation + cloth step. End-to-end CPU integration is covered by `fuse_physics_phase4_integration` (broadphase → narrowphase → PBD/CCD stubs → `PhysicsManager`).

### B4.11 checklist

#### Broad phase

- [x] `fuse_physics_broadphase_tests` — spatial hash overlapping pairs (CPU stub)
- [ ] 10k random spheres vs brute force O(n²) — catalog `broadphase.spatial_hash_10k`
- [ ] Bodies straddling multiple cells — edge case
- [ ] GPU radix sort — deferred to CUDA B4.1

#### Narrow phase

- [x] `fuse_physics_narrowphase_tests` — analytic sphere/plane + GJK stub paths
- [ ] Sphere-sphere vs Bullet within 0.001f — catalog `narrowphase.sphere_sphere`
- [ ] Capsule-capsule degeneracies
- [ ] EPA / SDF smooth normals — deferred

#### Solver (PBD)

- [x] `fuse_physics_pbd_tests` — gravity fall, separation, distance constraint, sleep
- [ ] Stack of 10 spheres stable 5s — catalog `solver.stack_stability`
- [ ] Restitution / friction analytical match — deferred

#### CCD

- [x] `fuse_physics_ccd_tests` — swept sphere-sphere + `CcdPipeline` RB_CCD filter
- [x] `PhysicsManager` runs `CcdPipeline` sweep when `enableCcd=true`
- [ ] High-velocity tunneling through thin wall — catalog `ccd.tunneling`

#### Destruction

- [x] `fuse_voxel_destruction` — carve radius, SVO carve/query, debris counters
- [x] Automated smoke — `destruction.carve_radius`, `destruction.debris_mass`
- [ ] Dual contouring watertight mesh — B3.5 follow-up

#### Soft body

- [x] `fuse_softbody` — `ParticleSoA`, cloth init, pinned corners, wind
- [x] Automated smoke — `softbody.cloth_gravity`, `softbody.pinned_corners`
- [ ] Cloth-sphere collision — deferred

#### Integration

- [x] `fuse_physics_manager` — init/step lifecycle, queries, destruction queue
- [x] `fuse_collision_events` — Enter/Trigger dispatch
- [x] `fuse_physics_phase4_integration` — broadphase → narrowphase → PBD/CCD → `PhysicsManager`
- [x] Catalog entries — `integration.manager_step`, `integration.collision_callbacks`, `integration.broad_to_manager`
- [ ] ECS Transform writeback every frame — `syncSoaToEcs_` stub

#### Performance baselines (RTX 3090 / CUDA)

- [ ] 1000-body `PhysicsManager::step` < 8ms — catalog `perf.manager_1000_bodies`
- [ ] Full pipeline 1000 dynamics < 4ms — deferred to CUDA B4.1–B4.6
- [ ] 10k sleeping bodies < 0.5ms — deferred

---

## Thread ownership (locked)

- Physics simulation data lives on the CUDA **Physics** stream (`CUDAStreamKind::Physics` from B2.6 `StreamManager`)
- CPU gameplay code queues destruction events and reads query results after `step()` — no GPU record from job workers
- Collision callbacks dispatch on the game thread after solver step (same frame as ECS sync)

---

## Tests

| Target | Validates |
|--------|-----------|
| `fuse_physics_data_tests` | B4.1 SoA allocate/addBody |
| `fuse_physics_broadphase_tests` | B4.2 spatial hash pairs |
| `fuse_physics_narrowphase_tests` | B4.3 analytic + GJK stubs |
| `fuse_physics_pipeline_tests` | B4.1 frame pipeline step |
| `fuse_physics_world_composition_tests` | World3D + physics composition |
| `fuse_voxel_destruction` | Carve radius derivation, SVO carve/query, debris spawn counters |
| `fuse_softbody` | `ParticleSoA` lifecycle, cloth init, pinned corners, wind |
| `fuse_physics_manager` | Init/step lifecycle, queries, destruction event queue |
| `fuse_collision_events` | Register/unregister, Enter/Trigger dispatch |
| `fuse_phase4_deliverables` | Catalog non-empty, category counts, automated smoke |
| `fuse_physics_phase4_integration` | B4.11 end-to-end: broadphase → narrowphase → PBD/CCD → `PhysicsManager` |

Run:

```bash
ctest --test-dir build --output-on-failure -R 'fuse_physics|fuse_voxel|fuse_softbody|fuse_collision|fuse_phase4'
```

---

## CI story (honest)

1. **Linux umbrella** — `FUSE_BUILD_PHYSICS=ON`; all physics tests are CPU-only stubs (no `nvcc`, no Bullet).
2. **ASan smoke** — physics lib not linked into `fuse_runtime_smoke` yet; subsystem tests run in Release umbrella job.
3. **CUDA physics** — deferred to B4.1–B4.6 + `FUSE_BUILD_CUDA` integration.

---

## Next

- [x] B4.7 destruction scaffold — `DestructionSystem`, `Svo` stub, debris counters
- [x] B4.8 soft body scaffold — `ParticleSoA`, `ClothSimulator` CPU XPBD
- [x] B4.9 physics manager scaffold — ECS ↔ SoA bridge, destruction queue
- [x] B4.10 collision events scaffold — callback registration + dispatch
- [x] B4.11 deliverable registry — checklist catalog + automated smoke + phase-4 integration test
- [x] B4.1–B4.3 composed: `RigidBodySoA`, spatial hash, narrow phase, `PhysicsPipeline`
- [ ] B4.4–B4.6 upstream: CUDA PBD, Barnes-Hut, CCD kernels (sibling PR #29)
- [ ] B3.5 SVO integration — real carve + dual contouring mesh extraction
- [ ] Wire `PhysicsManager` to `fuse::renderer::cuda::StreamManager`
- [ ] Bullet/reference acceptance tests from B4.11 catalog

---

## Related docs

- [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) — CUDA stream manager (B2.6)
- [work-plan.md](./work-plan.md) — Track B entries
- [BUILD.md](./BUILD.md) — umbrella CMake options
