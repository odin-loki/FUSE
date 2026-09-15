# Track B — Phase 1 Core Deliverables (B1.1–B1.8)

**Status:** B1.8 Phase 1 gate — checklist registry + cross-subsystem integration smoke on `fuse_core`  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B1.1–B1.8  
**Related:** [TRACK-B-MATH.md](./TRACK-B-MATH.md), [TRACK-B-CORE-B16.md](./TRACK-B-CORE-B16.md), [BUILD.md](./BUILD.md)

---

## B1.1–B1.7 status summary

| ID | System | Library / location | Stub landed | Automated tests | Notes |
|----|--------|-------------------|-------------|-----------------|-------|
| **B1.1** | Project structure & build | Umbrella CMake + `fuse_core` | ✅ | CI umbrella workflow | U1 landed — [BUILD.md](./BUILD.md); C++17 host today (C++23 + shipping preset deferred) |
| **B1.2** | Type system & handles | `fuse::types`, `Handle<T>`, `Object` | ✅ | `fuse_core_services` | Fixed-width aliases, generation handles, object hierarchy |
| **B1.3** | Memory & allocators | `fuse::alloc` (frame/pool/stack) | ✅ | `fuse_core_allocator`, `fuse_core_services` | Frame bump + pool/stack allocators with stats hooks (#70); GPU allocators deferred |
| **B1.4** | Math library | `fuse::math` | ✅ | `fuse_core_math` | Vec/Mat/Quat/AABB/Frustum/SDF — see [TRACK-B-MATH.md](./TRACK-B-MATH.md) |
| **B1.5** | Job system & fibers | `fuse::jobs` | ✅ | `fuse_core_jobs`, `fuse_core_fiber`, `fuse_cuda_jobs` | Scheduler, `JobCounter`, `parallel_for`, CUDA job lane stub |
| **B1.6** | Logging, assert & profiler | `fuse::log`, `fuse::assertion`, `fuse::profiler` | ✅ | `fuse_core_profiler_assert` | See [TRACK-B-CORE-B16.md](./TRACK-B-CORE-B16.md) |
| **B1.7** | Platform & window | `fuse::platform::Window`, `EventPump` | ✅ | `fuse_core_platform_window`, `fuse_core_platform_hardening` | Window + event-pump stubs (#69); see [TRACK-B-CORE-B17.md](./TRACK-B-CORE-B17.md) |

Submodule detail:

| Topic | Doc / README |
|-------|----------------|
| B1.1 Build | [BUILD.md](./BUILD.md), [unified-layout.md](./unified-layout.md) |
| B1.4 Math | [TRACK-B-MATH.md](./TRACK-B-MATH.md) |
| B1.5 Jobs | [architecture-parallel.md](./architecture-parallel.md) §3, [wp03-fiber-remaining.md](./wp03-fiber-remaining.md) |
| B1.6 Profiler | [TRACK-B-CORE-B16.md](./TRACK-B-CORE-B16.md) |
| B1.7 Window | [TRACK-B-CORE-B17.md](./TRACK-B-CORE-B17.md) |
| B1.7 Platform hardening | [TRACK-B-PLATFORM.md](./TRACK-B-PLATFORM.md) (B7.8 lifecycle hooks) |

---

## B1.8 — Deliverable checklist registry

`Phase1TestRegistry` (`Source/FUSE/Core/`) catalogs master-plan acceptance items per B1 subsystem. Entries record whether the stub scaffold landed and whether an automated probe exists today. Full Phase 1 gates from §B1.8 (1M-job stress, GPU allocators, shipping preset, etc.) remain `automated=false` until upstream implementations land.

```cpp
const auto& checklist = fuse::core::Phase1TestRegistry::checklist();
fuse::core::Phase1TestRegistry::runIntegrationSmoke(); // jobs + math + handles + VFS + profiler
```

---

## B1.8 — Cross-subsystem integration smoke

`fuse_core_phase1_integration` exercises the Phase 1 spine in one process:

| Subsystem | API exercised | CMake gate |
|-----------|---------------|------------|
| B1.5 Jobs | `core::initialize`, `parallel_for` over math transforms | always |
| B1.4 Math | `Mat4::fromTRS`, `transformPoint`, `AABB::contains` | always |
| B1.2/B1.3 Handles | `HandleTable` worker publish + game-thread `commit` | always |
| B1.2 I/O + VFS | `VirtualFileSystem::submitLoadAsync` → `drainCompletedLoads` → handle resolve | always |
| B1.6 Profiler | `FUSE_PROFILE_SCOPE`, `beginFrame`/`endFrame`, chrome JSON export | always |
| B1.7 Platform | `Window` stub metadata + `VulkanSurfaceWire` null surface | always |

No OS WSI surface, CUDA device, or Torque legacy is required — the smoke validates stub data paths and cross-lane wiring only.

---

## Build

Phase 1 registry ships inside `fuse_core`. Tests register when `FUSE_BUILD_CORE_TESTS=ON`.

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF

cmake --build build
ctest --test-dir build --output-on-failure -R fuse_core_phase1
```

| Condition | Behaviour |
|-----------|-----------|
| `FUSE_JOBS_SINGLE_THREAD=ON` | Smoke still passes — scheduler runs on caller thread |
| `FUSE_BUILD_CORE_TESTS=OFF` | No `fuse_core_phase1_integration` CTest target |
| Mobile profile (`FUSE_PLATFORM_MOBILE`) | Window/EventPump no-op stubs compile |

---

## Tests

| Target | Validates |
|--------|-----------|
| `fuse_core_phase1_integration` | Checklist non-empty, per-module counts, `runIntegrationSmoke()` ties jobs + math + handles + VFS async + profiler |

Run:

```bash
ctest --test-dir build --output-on-failure -R fuse_core_phase1
```

Existing per-subsystem tests remain the authoritative unit coverage:

| CTest name | B1 area |
|------------|---------|
| `fuse_core_worker_count` | B1.5 |
| `fuse_core_jobs` | B1.5 |
| `fuse_core_fiber` | B1.5 |
| `fuse_cuda_jobs` | B1.5 |
| `fuse_core_services` | B1.2 |
| `fuse_core_allocator` | B1.3 |
| `fuse_core_io_handle` | B1.2 handles + VFS |
| `fuse_core_math` | B1.4 |
| `fuse_core_profiler_assert` | B1.6 |
| `fuse_core_platform_window` | B1.7 |
| `fuse_core_platform_hardening` | B1.7 / B7.8 |

---

## Gates (B1.8 scaffold)

- [x] B1.1–B1.7 status documented with stub/test pointers
- [x] `Phase1TestRegistry` checklist on FUSE APIs
- [x] Cross-subsystem integration smoke ties jobs + math + handles + VFS async + profiler
- [x] CTest target green in Linux umbrella CI
- [ ] Master-plan §B1.8 full acceptance matrix (1M alloc stress, GPU allocators, shipping preset, valgrind gate, etc.) — deferred per-subsystem

---

## CI story (honest)

1. **Linux umbrella** — all `fuse_core_*` tests plus `fuse_core_phase1_integration` run in the Release CTest job.
2. **TSan nightly** — `fuse_core_jobs` / `fuse_core_io_handle` run separately; phase-1 integration runs in the umbrella job.
3. **ASan smoke** — `fuse_runtime_smoke` remains separate one-process gate (U2).

---

## Next

- [ ] Wire remaining §B1.8 acceptance items to real backends as B1.1–B1.7 implementations mature
- [ ] C++23 host + shipping preset when umbrella CMake upgrades
- [ ] Pool/linear/GPU allocator suite (B1.3 deepen)
- [ ] Lock-free async logger ring (B1.6 logging follow-up)

---

## Related docs

- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B1.1–B1.8
- [TRACK-B-PHASE7.md](./TRACK-B-PHASE7.md) — B7.10 deliverable pattern
- [TRACK-B-PHYSICS.md](./TRACK-B-PHYSICS.md) — B4.11 phase-4 integration pattern
