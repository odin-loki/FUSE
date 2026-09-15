# WP-03 — Cooperative fiber wait (incremental)

**Status:** Landed (POSIX ucontext on desktop Linux/macOS; Win32 fibers on Windows)  
**Deferred:** Android/iOS dedicated backends, Emscripten pthread coarse pool

---

## Shipped

- `fuse/platform/fiber.hpp` + `fiber_posix.cpp` — stack switching via `ucontext` on **desktop** Linux/macOS
- `fuse/platform/fiber.hpp` + `win/fiber_win32.cpp` — cooperative fibers via `ConvertThreadToFiber` / `CreateFiber` / `SwitchToFiber` on **Windows desktop**
- **Android / iOS / mobile:** `cooperativeFibersAvailable()` is false — same fiber API, but `JobCounter::wait()` blocks on condition variables (no ucontext; NDK omits `getcontext`/`makecontext`/`swapcontext`)
- Worker threads run jobs on a dedicated job fiber; `JobCounter::wait()` on workers yields to the scheduler fiber instead of blocking the OS thread on a condition variable
- ASan fiber annotations when built with `-fsanitize=address` (POSIX backend)
- Scheduler refuses new jobs on a worker whose job fiber is suspended in a cooperative wait (prevents fiber stack corruption)
- Game/submit thread `wait()` still uses condition variables (expected for fork-join root)
- `fiberBackendName()` diagnostic (`posix-ucontext`, `win32`, or `stub`) + `fuse_core_fiber` unit tests (swap round-trip on desktop; compile-gated backend name checks elsewhere)
- **`FUSE_JOBS_SINGLE_THREAD` compile-time profile** — `computeWorkerCount()` → `0`, scheduler `submit()` inline, serial `parallel_for`; Linux CI job + `fuse_core_jobs_single_thread` tests
- **Work-steal policy stubs** — `fuse/jobs/work_steal.hpp`: `pickStealVictim`, `stealHalfQueueBatchSize`, `canStealFromVictim` (empty-victim fallback); `JobScheduler::trySteal` uses rotating victim pick; half-queue batch is stubbed (one job per steal today)
- **Emscripten stub profile documented** — `FUSE_PLATFORM_EMSCRIPTEN` + default `FUSE_JOBS_SINGLE_THREAD=ON`; fiber `stub` backend; does **not** gate desktop/mobile WP-03 exit (see [BUILD.md](./BUILD.md#emscripten-stub-profile-deferred-non-blocking))

## Remaining (honest limit)

| Item | Notes |
|------|-------|
| Per-job fiber pools | Today one job fiber per worker; nested waits OK, not arbitrary coroutine depth |
| Android / iOS native backend | Today CV fallback only; future: asm/stackful coroutine or platform fiber API |
| Apple desktop ucontext | Deprecated but used on macOS desktop; migrate when glibc removes ucontext |
| Emscripten pthread coarse pool | Stub profile shipped (`FUSE_JOBS_SINGLE_THREAD` + fiber `stub`); `FUSE_JOBS_COARSE_POOL` deferred per architecture-parallel §3.5 |
| Hot-path mutex removal | `JobCounter` still uses mutex for CV fallback and waiter lists |
| I/O + render dependency chains | Needs WP-04 handle commit + WP-06 frame barrier |
| Win32 ASan fiber annotations | POSIX has `__sanitizer_*_switch_fiber`; Windows backend not yet instrumented |

See [architecture-parallel.md](./architecture-parallel.md) §3.2–3.3 and [work-plan.md](./work-plan.md) WP-03.

---

## TSan nightly — known race surfaces

Nightly job: [`.github/workflows/fuse-tsan-nightly.yml`](../../.github/workflows/fuse-tsan-nightly.yml) (`FUSE_CORE_ENABLE_TSAN=ON`, `fuse_core_*` CTest only).

### Fixed (regression-covered)

| Surface | Symptom | Mitigation |
|---------|---------|------------|
| Cross-thread `WorkerState` mutation on `JobCounter::signal()` | TSan data race on `waitingOn` when one worker signaled a counter while another worker's scheduler fiber cleared `waitingOn` after cooperative `wait()` | Cooperative wakeups poll `waitingOn->isComplete()` on the owning worker only; no cross-thread `WorkerState` writes |
| `JobCounter` teardown vs in-flight `signal()` | TSan race on `m_fiberWaiters` / mutex when `wait()` returned before `signal()` released `m_waitMutex` | Removed unused fiber-waiter list; `wait()` calls `synchronizeCompletion()` to serialize with the final `signal()` |
| Shared `Impl::useFibers` bool | Potential torn read when fiber allocation fails on one worker while others observe the flag | `std::atomic<bool>` with acquire/release loads |

Regression tests: `fuse_core_jobs` — `testParallelSerialFallbackParity`, `testNestedParallelForParity`, `testNestedParallelForSerialFallbackParity`, `testNestedParallelForWithCooperativeWait`, work-steal helper/integration tests (`testWorkSteal*`), `testNestedParallelForVisitCountSingleThread`.

### Remaining (documented, not yet eliminated)

| Surface | Risk | Notes |
|---------|------|-------|
| Eager fiber wake queue | Perf only — workers poll `waitingOn->isComplete()` today | Explicit wake list deferred; avoids cross-thread `WorkerState` mutation |
| `JobCounter::wait()` CV path (game/submit thread) | Expected — mutex + `condition_variable` for non-worker waiters | Not a fiber hot path; documented in Shipped § above |
| Work-stealing deque locks | Contention only — each queue has its own mutex | TSan-clean; perf tuning deferred |
| `parallel_for` body captures `&body` | User responsibility — lambdas with stale references across nested `parallel_for` are UB | Tests use stack-local functors; callers must not capture temporaries |
| Fiber stack depth | Logic bug, not a data race — one job fiber per worker; nested `parallel_for` + `wait()` is OK, arbitrary coroutine depth is not | See per-job fiber pools row above |
| Concurrent nested `parallel_for` on all workers | **Deadlock** when every worker is cooperatively waiting on an inner counter and none steal (by design) | Safe pattern: single outer chunk (`grain >= outer`) or serial outer driver; see `fuse_core_jobs` nested parity tests |
