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
| `SpatialHash` / `Gjk` | `broadphase/`, `narrowphase/` | B4.2–B4.3 CPU reference paths; broadphase jobifies shape→cell + per-cell candidate generation via `fuse::jobs::parallel_for`; narrowphase writes one contact slot per candidate pair then compacts |
| `ContactManifold` / `ContactBufferSoA` | `narrowphase/contact_manifold.hpp`, `contact_buffer.hpp` | Multi-point slots (up to 4), normal/penetration per point, warm-start impulse stubs |
| `buildTangentBasis` / `clampFrictionImpulse` | `narrowphase/friction.hpp` | Coulomb friction cone clamp + tangent basis helper (CPU stub) |
| `collideBoxBox` | `narrowphase/box_box.cpp` | Axis-aligned box-box stub emitting four face contact points |
| `PhysicsWorld2D` / `PhysicsWorld3D` | `physics_world_*.hpp` | World composition hooks |
| `DestructionSystem` / `DestructionEvent` | `destruction/` | B4.7 SVO carve → debris spawn scaffold |
| `Svo` | `Source/FUSE/Physics/include/fuse/physics/spatial/` | Minimal carve/query until Track A B3.5 |
| `ParticleSoA` / `ClothSimulator` | `Source/FUSE/Physics/include/fuse/physics/softbody/` | Host XPBD stub; CUDA kernels deferred |
| `PhysicsManager` | `Source/FUSE/Physics/include/fuse/physics/physics_manager.hpp` | ECS ↔ SoA bridge scaffold |
| `CollisionEventSystem` | `Source/FUSE/Physics/include/fuse/physics/events/` | Enter/Stay/Exit/Trigger callback bus |
| `Phase4TestRegistry` | `Source/FUSE/Physics/include/fuse/physics/phase4_test_registry.hpp` | B4.11 checklist + automated smoke |
| `PBDSolver` | `solver/pbd_solver.hpp` | B4.4 CPU XPBD contacts + distance constraints |
| `SolverWorkBuffers` | `solver/solver_work_buffers.hpp` | Job-safe per-body delta scratch + reusable contact buffer |
| `ContactIslandGraph` | `solver/contact_island_graph.hpp` | Connected-component partition for parallel island iteration |
| `ToiBufferSoA` / `runCcdIntoBuffer` | `ccd/toi_buffer.hpp`, `ccd/ccd.hpp` | Job-safe per-pair TOI slots + sphere/plane/slab sweep helpers |

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
- [x] `fuse_physics_broadphase_tests` — parallel vs single-thread scheduler parity + 1k-scene smoke
- [x] `fuse_physics_broadphase_tests` — 256-sphere brute-force reference match
- [ ] 10k random spheres vs brute force O(n²) — catalog `broadphase.spatial_hash_10k` (1k parity smoke landed; full 10k deferred)
- [x] Bodies straddling multiple cells — edge case
- [ ] GPU radix sort — deferred to CUDA B4.1

#### Narrow phase

- [x] `fuse_physics_narrowphase_tests` — analytic sphere/plane + GJK stub paths
- [x] `fuse_physics_narrowphase_tests` — axis-aligned box-sphere + Y-axis capsule-sphere stubs
- [x] `ContactBufferSoA` — per-pair slot clear/reuse + compact; job-safe `runNarrowphaseIntoBuffer`
- [x] `ContactBufferSoA` — multi-point slots, warm-start impulse stubs, box-box four-point manifolds
- [x] `fuse_physics_narrowphase_tests` — friction clamp + tangent basis, box-box contact count
- [ ] Sphere-sphere vs Bullet within 0.001f — catalog `narrowphase.sphere_sphere`
- [ ] Capsule-capsule degeneracies
- [ ] EPA / SDF smooth normals — deferred

#### Solver (PBD)

- [x] `fuse_physics_pbd_tests` — gravity fall, separation, distance constraint, sleep
- [x] `fuse_physics_pbd_tests` — compliant spring stretch + rest-length recovery after release
- [x] `fuse_physics_pbd_tests` — contact island graph partitions disconnected groups; iteration count surfaced
- [x] `fuse_physics_pbd_tests` — job-safe distance/spring accumulation, warm-start lambda, residual early-exit stub
- [x] `fuse_physics_pbd_tests` — rest-length spring converges under N iterations; multi-island independence; residual decreases
- [ ] Stack of 10 spheres stable 5s — catalog `solver.stack_stability`
- [ ] Restitution / friction analytical match — deferred

#### CCD

- [x] `fuse_physics_ccd_tests` — swept sphere-sphere + `CcdPipeline` RB_CCD filter
- [x] `PhysicsManager` runs `CcdPipeline` sweep when `enableCcd=true`
- [x] `ToiBufferSoA` — per-pair slot clear/reuse + compact; job-safe `runCcdIntoBuffer`
- [x] `fuse_physics_ccd_tests` — sphere-plane + thin-slab TOI helpers, tunneling smoke
- [ ] High-velocity tunneling through thin wall — catalog `ccd.tunneling` (analytic smoke landed; full gameplay acceptance deferred)

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
- **Broadphase jobify (CPU stub):** `runBroadphase` / `runBroadphase2D` read immutable `RigidBodySoA` + `CollisionShapeSoA` snapshots and write disjoint per-cell / per-dynamic pair buffers via `fuse::jobs::parallel_for`; falls back to serial when `JobScheduler` is single-threaded or uninitialized
- **PBD constraint iteration (CPU stub):** each substep builds `ContactIslandGraph` from contacts + distance constraints; `parallel_for` dispatches islands while contacts within an island resolve sequentially (Gauss-Seidel). `SolverWorkBuffers` holds reusable manifolds, per-body `PositionDelta` slots, and per-constraint lambda warm-start buffers. `accumulateDistanceSpringCorrection` / `accumulateContactCorrection` write disjoint body slots (job-safe SoA); `applyPositionDeltas` commits corrections after each iteration. `SolverParams::iterations` configures the max pass count; `residualTolerance` enables early-exit when max constraint violation drops below threshold; `lastConstraintResidual()` surfaces the stub for tests
- **Narrowphase job-safe slots (CPU stub):** `runNarrowphaseIntoBuffer` assigns one output slot per candidate pair index; workers write only their slot, then `ContactBufferSoA::compact()` gathers valid manifolds without shared mutable pair state (serial dispatch on CPU stub; slot layout matches parallel kernel path). Each slot stores up to four contact points plus warm-start normal/tangent impulse stubs for solver reuse.
- **CCD job-safe slots (CPU stub):** `runCcdIntoBuffer` assigns one TOI slot per candidate pair; workers write only their slot, then `ToiBufferSoA::compact()` gathers valid impacts. `sweptSpherePlane` / `sweptSphereSlabZ` cover fast-mover vs wall stubs; `PhysicsManager` reuses `m_toiBuffer_` each step

---

## Tests

| Target | Validates |
|--------|-----------|
| `fuse_physics_data_tests` | B4.1 SoA allocate/addBody |
| `fuse_physics_broadphase_tests` | B4.2 spatial hash pairs, parallel jobify parity, straddling cells, brute-force reference |
| `fuse_physics_narrowphase_tests` | B4.3 analytic + GJK stubs, box/capsule-sphere/box-box, SoA contact buffer reuse, friction clamp |
| `fuse_physics_pipeline_tests` | B4.1 frame pipeline step |
| `fuse_physics_world_composition_tests` | World3D + physics composition |
| `fuse_voxel_destruction` | Carve radius derivation, SVO carve/query, debris spawn counters |
| `fuse_softbody` | `ParticleSoA` lifecycle, cloth init, pinned corners, wind |
| `fuse_physics_manager` | Init/step lifecycle, queries, destruction event queue |
| `fuse_collision_events` | Register/unregister, Enter/Trigger dispatch |
| `fuse_phase4_deliverables` | Catalog non-empty, category counts, automated smoke |
| `fuse_physics_phase4_integration` | B4.11 end-to-end: broadphase → narrowphase → PBD/CCD → `PhysicsManager` |
| `fuse_physics_ccd_tests` | B4.6 CCD sweep helpers, `ToiBufferSoA`, sphere/plane/slab TOI, job-safe pair dispatch |

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
- [x] B4.3 narrowphase deepen — `ContactBufferSoA`, box-sphere / capsule-sphere stubs, job-safe pair dispatch
- [x] B4.3 narrowphase deepen — multi-point manifolds, warm-start impulses, `friction.hpp`, axis-aligned box-box stub
- [x] B4.2 CPU broadphase jobify — `parallel_for` over shape→cell build + per-cell candidate generation stubs
- [x] B4.4 CPU deepen — island-partitioned constraint iterations, job-safe `SolverWorkBuffers`, rest-length spring tests
- [x] B4.4 CPU deepen — configurable iteration count, lambda warm-start, residual early-exit stub, job-safe constraint accumulation helpers
- [x] B4.6 CPU deepen — `ToiBufferSoA`, sphere/plane/slab sweep helpers, job-safe `runCcdIntoBuffer`
- [ ] B4.4–B4.6 CUDA: PBD kernels, Barnes-Hut GPU, CCD sweep GPU (CPU stubs on main via #29)
- [ ] B3.5 SVO integration — real carve + dual contouring mesh extraction
- [ ] Wire `PhysicsManager` to `fuse::renderer::cuda::StreamManager`
- [ ] Bullet/reference acceptance tests from B4.11 catalog

---

## Related docs

- [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) — CUDA stream manager (B2.6)
- [work-plan.md](./work-plan.md) — Track B entries
- [BUILD.md](./BUILD.md) — umbrella CMake options
