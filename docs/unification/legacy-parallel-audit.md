# WP-11 — Legacy Parallel Audit

**Phase:** U0 → ongoing (continuous)  
**Date:** 2026-09-15  
**Owner:** WP-11  
**Deps:** WP-03 (`fuse::jobs::JobScheduler`, `parallel_for`)  
**Evidence:** [concurrency-inventory.md](./concurrency-inventory.md), [architecture-parallel.md](./architecture-parallel.md) §4.2, [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) B1.5

---

## 1. Goal

Identify **safe** Torque3D (T3D) and Torque2D (T2D) loops that can be routed onto `fuse::jobs::JobScheduler::parallel_for` without touching `SimObject` mutation, console state, or GFX device APIs. Shrink reliance on `_forceAllMainThread` / `ThreadPool::smForceAllMainThread` by migrating read-mostly work first.

**Adapter (landed):** `Source/FUSE/Legacy/include/fuse/legacy/parallel_for.hpp` — thin wrapper over `fuse::jobs::parallel_for` for future call-site swaps.

---

## 2. Safety rubric

| Tier | Criteria | Route to JobScheduler? |
|------|----------|------------------------|
| **A — Safe now** | Read-only over immutable snapshot or disjoint write buffers; no `SimObject*`, no `Con::`, no `GFX*` | ✅ Yes |
| **B — Safe after snapshot** | Per-element writes to pre-allocated SOA/chunk buffers built on game thread; publish via `JobCounter` + frame barrier | 🚧 Next |
| **C — I/O lane** | Blocking or async file/DNS work; belongs on I/O job lane (WP-04), not tick `parallel_for` | ✅ Via `submit()` |
| **D — Main-thread only** | Sim mutation, script VM, physics step, render record, `ThreadPool` main-thread queue | ❌ No |

**Invariant (locked):** Workers never see raw `fuse::legacy::t3d::SceneObject*` / T2D component pointers — only FUSE snapshots or scratch buffers ([architecture-parallel.md](./architecture-parallel.md) §5.2).

---

## 3. T3D (`Engine/source`) — candidate loops

### 3.1 Tier A — safe to route (first wave)

| Loop / site | Path | Work pattern | Notes |
|-------------|------|--------------|-------|
| **DDS mip compress** | `gfx/bitmap/imageUtils.cpp` L138–196 | Per-mip `CompressJob` : `ThreadPool::WorkItem`; disjoint `dstBits` per job | Already parallel via `ThreadPool`; **replace** with `fuse::legacy::parallel_for_indices` over mip indices |
| **Raw block compress** | `gfx/bitmap/imageUtils.cpp` L89–101 | `squish::CompressImage` on worker | Same as above — pure CPU, no GFX |
| **VHACD convex hull** | `ts/vhacd/VHACD.h` L7208+ | Internal `std::thread` pool (8 workers) | Mesh cook only; route to job lane when FUSE asset pipeline owns cook |
| **stb parallel decode** | `gfx/bitmap/loaders/stb/stb_image.h` L293+ | Optional parallel decode hook | Read-only input buffer → write pixels to staging |

### 3.2 Tier B — safe after snapshot / SOA publish

| Loop / site | Path | Work pattern | Blocker |
|-------------|------|--------------|---------|
| **Terrain cell VB/PB update** | `terrain/terrCell.cpp` L456–805 | Nested `x/y` over heightfield → vertex/index buffers | Needs per-cell scratch + game-thread publish |
| **Terrain bounds / zoning** | `terrain/terrCell.cpp` L874–957 | Read height samples → write `Box3F` | Same |
| **Zone/frustum cull gather** | `T3D/tsStatic.cpp` L779+ | Read transform + mesh bounds → visibility bitset | Needs `ObjectSnapshot` SOA (U4/U5) |
| **Forest cell iteration** | `forest/` | Cell visibility / LOD | No `ThreadPool` today; snapshot first |

### 3.3 Tier C — I/O lane (not tick `parallel_for`)

| Site | Path | Today | FUSE target |
|------|------|-------|-------------|
| Async file read | `core/stream/fileStream.cpp` L31–61 | `ThreadPool::queueWorkItem` | `VirtualFileSystem::submitLoadAsync` |
| DNS lookup | `platform/platformNetAsync.cpp` L68–136 | `NameLookupWorkItem` | I/O lane `submit()` |
| Async packet stream | `platform/async/asyncBufferedStream.h` L414 | `queueWorkItem` | I/O lane |
| SFX stream decode | `sfx/sfxSystem.cpp` L558 | `SFXThreadPool` | Dedicated audio lane (unchanged v1) |

### 3.4 Tier D — do not route

| Site | Path | Reason |
|------|------|--------|
| Sim event queue | `console/simManager.cpp` L59–99 | `gEventQueueMutex`; `Sim::postEvent` |
| Script compile | `console/torquescript/codeBlock.cpp` L464 | `AssertFatal(Con::isMainThread())` |
| Theora decode → upload | `gfx/video/theoraTexture.cpp` L339 | Decode on workers OK; **GFX upload main-thread only** (comment L151–152) |
| Physics / collision | `T3D/physics/`, Bullet | Sequential per world (v1) |
| Render / GFX submit | `app/mainLoop.cpp` process chain | Game thread owns context |
| Main-thread work queue | `threadPool.cpp` `processMainThreadWorkItems` | Compat shim — shrink, not parallelize |

### 3.5 `_forceAllMainThread` shrink plan

| Step | Action |
|------|--------|
| 1 | Route Tier A compress loops through `fuse::legacy::parallel_for` (this WP) |
| 2 | Add TSan nightly coverage for migrated sites |
| 3 | Default `_forceAllMainThread=0` in CI smoke; compare checksums |
| 4 | Remove `ThreadPool::GLOBAL()` for Tier A once parity proven |

---

## 4. T2D (`third_party/Torque2D/engine/source`) — candidate loops

T2D has **no** `ThreadPool` ([concurrency-inventory.md](./concurrency-inventory.md) §3). Parallelism is net-new via FUSE job spine.

### 4.1 Tier A — safe to route (first wave)

| Loop / site | Path | Work pattern | Notes |
|-------------|------|--------------|-------|
| **Sprite bounds scan** | Future `World2D` cull | Read `SceneSnapshot2D` AABBs → visibility flags | U4 `parallel_for` cull stub exists in FUSE |
| **Layer list gather** | Future `World2D` | Read-only sort/gather per layer chunk | Same snapshot contract |

### 4.2 Tier B — safe after snapshot

| Loop / site | Path | Work pattern | Blocker |
|-------------|------|--------------|---------|
| **Component tick read** | `component/simComponent.cpp` | Per-component update | `mMutex` on lists — snapshot required |
| **Scene object iteration** | `scene/` | Transform/bounds read for cull | `SimSet` mutex on iteration |
| **Particle / FX sim buffers** | `fx/` (T2D) | SOA particle update | Needs double-buffered sim state |

### 4.3 Tier C — I/O lane

| Site | Path | Today | FUSE target |
|------|------|-------|-------------|
| DNS lookup | `platform/platformNetAsync.cpp` L108–163 | Dedicated `Thread` + `gNetAsyncMutex` | I/O lane `submit()` |

### 4.4 Tier D — do not route

| Site | Path | Reason |
|------|------|--------|
| Box2D step | `physics/` | Sequential world mutation |
| `Sim::postEvent` | 20 refs / 10 files | Main-thread event queue |
| `SimComponent` list lock | `simComponent.cpp` L35 | Mutex on component graph |
| GLES draw | `platformiOS/iOSGL2ES.mm`, `platformAndroid/AndroidGL2ES.cpp` | Game thread owns context |
| `Game->mainLoop()` | `game/defaultGame.h` | Single-threaded frame |

---

## 5. Migration order (sprint backlog)

| Priority | Dimension | Site | Effort | WP |
|----------|-----------|------|--------|-----|
| P0 | FUSE | `fuse::legacy::parallel_for` adapter + smoke sum | S | WP-11 ✅ |
| P1 | T3D | `imageUtils.cpp` mip compress → `parallel_for_indices` | S | WP-11 |
| P2 | T3D | Terrain cell VB update (per-cell jobs) | M | WP-11 + U5 |
| P3 | Both | World2D/3D cull over snapshots | M | WP-06 |
| P4 | T3D | Retire `ThreadPool` for Tier A | M | WP-11 |
| P5 | T2D | Sprite cull via `World2D` snapshot | M | WP-06 |

---

## 6. Verification

| Check | Command / gate |
|-------|----------------|
| Adapter compiles | `fuse_legacy_common` linked by `fuse_t3d_legacy` / `fuse_t2d_legacy` |
| Smoke sum | `fuse_runtime_smoke` — `parallel_for_smoke_sum(0, 100, 10) == 4950` |
| Serial parity | `FUSE_JOBS_SINGLE_THREAD=ON` — same checksum as parallel |
| TSan | `.github/workflows/fuse-tsan-nightly.yml` (post-migration) |
| Force-main-thread | `_forceAllMainThread=1` — legacy ThreadPool path still works until P4 |

---

## 7. References

- [work-plan.md](./work-plan.md) WP-11
- [wp03-fiber-remaining.md](./wp03-fiber-remaining.md) — scheduler backends
- [U4-HYBRID-FRAME.md](./U4-HYBRID-FRAME.md) — `World2D`/`World3D` cull stubs
- `Source/FUSE/Legacy/include/fuse/legacy/parallel_for.hpp` — adapter header
