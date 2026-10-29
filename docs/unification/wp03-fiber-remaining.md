# WP-03 — Cooperative fiber wait (incremental)

**Status:** Landed (POSIX ucontext on desktop Linux/macOS; Win32 fibers on Windows)  
**Deferred:** Android/iOS dedicated backends, Emscripten pthread coarse pool

---

## Shipped

- `fuse/platform/fiber.hpp` + `fiber_posix.cpp` — stack switching via `ucontext` on **desktop** Linux/macOS
- `fuse/platform/fiber.hpp` + `win/fiber_win32.cpp` — cooperative fibers via `ConvertThreadToFiber` / `CreateFiber` / `SwitchToFiber` on **Windows desktop**
- **Android / iOS / mobile:** `cooperativeFibersAvailable()` is false — same fiber API, but `JobCounter::wait()` blocks on condition variables (no ucontext; NDK omits `getcontext`/`makecontext`/`swapcontext`)
- Worker threads run jobs on pooled job fibers (16 prewarmed per worker, 64 KiB stacks on desktop, grown on demand up to `kMaxFibersPerWorker` = 128). `JobCounter::wait()` on a worker parks the current job fiber and yields to the scheduler fiber instead of blocking the OS thread on a condition variable; the worker keeps running other jobs on fresh fibers and resumes the parked one once its counter reaches zero
- ASan fiber annotations when built with `-fsanitize=address` (POSIX backend)
- A worker whose fiber pool is at the cap stops taking new jobs and only resumes parked fibers (a running job never shares a stack with a parked one)
- Game/submit thread `wait()` still uses condition variables (expected for fork-join root)
- `fiberBackendName()` diagnostic (`posix-ucontext`, `win32`, or `stub`) + `fuse_core_fiber` unit tests (swap round-trip on desktop; compile-gated backend name checks elsewhere)
- **`FUSE_JOBS_SINGLE_THREAD` compile-time profile** — `computeWorkerCount()` → `0`, scheduler `submit()` inline, serial `parallel_for`; Linux CI job + `fuse_core_jobs_single_thread` tests
- **Work stealing** — `fuse/jobs/work_steal.hpp`: `pickStealVictim`, `stealHalfQueueBatchSize`, `canStealFromVictim` (empty-victim fallback). `JobScheduler::trySteal` rotates victims, high band first, and moves the victim's newest half (ceil(n/2)) into the thief's queue under both queue locks; the thief's ring is filled to at most half its capacity, so stealing never grows a ring
- **Emscripten stub profile documented** — `FUSE_PLATFORM_EMSCRIPTEN` + default `FUSE_JOBS_SINGLE_THREAD=ON`; fiber `stub` backend; does **not** gate desktop/mobile WP-03 exit (see [BUILD.md](./BUILD.md#emscripten-stub-profile-deferred-non-blocking))

## Remaining (honest limit)

| Item | Notes |
|------|-------|
| Fiber pool cap | Pooled job fibers per worker (16 prewarmed, cap 128). Parking deeper than the cap stops the worker taking new jobs until a parked fiber resumes; not arbitrary coroutine depth |
| Android / iOS native backend | Today CV fallback only; future: asm/stackful coroutine or platform fiber API |
| Apple desktop ucontext | Deprecated but used on macOS desktop; migrate when glibc removes ucontext |
| Emscripten pthread coarse pool | Stub profile shipped (`FUSE_JOBS_SINGLE_THREAD` + fiber `stub`); `FUSE_JOBS_COARSE_POOL` deferred per architecture-parallel §3.5 |
| Hot-path mutex removal | `JobCounter` still uses mutex for CV fallback and waiter lists |
| I/O + render dependency chains | Needs WP-04 handle commit + WP-06 frame barrier |
| Win32 ASan fiber annotations | `__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber` wrap `SwitchToFiber` when the ASan runtime exports them (Clang/compiler-rt, and MSVC ASan 14.44 / 14.51). Bounds are the fiber reserve (`DeallocationStack` through `StackBase`), checked with `VirtualQuery` and the TEB; a mismatch aborts instead of passing false bounds. `fiberASanAnnotationsActive()` is true only when those hooks are linked. Runtimes that do not export them leave the diagnostic false. `__asan_handle_no_return` only clears the current thread stack shadow, so it is not used. |

**MSVC ASan spike (Option B):** Hidden preset `fuse-msvc-asan-debug` inherits `fuse-msvc-debug` with `FUSE_CORE_ENABLE_ASAN=ON` (instruments `fuse_core_*` test executables via `/fsanitize=address`; debug flags drop `/RTC1`, which MSVC ASan rejects). Manual CI only: [`.github/workflows/fuse-msvc-asan-spike.yml`](../../.github/workflows/fuse-msvc-asan-spike.yml) (`workflow_dispatch` on `windows-2022`, `ctest -R 'fuse_core_fiber|fuse_core_jobs'`). Does not gate PRs. On a runtime that exports the switch hooks, `fuse_core_fiber` runs with `fiberASanAnnotationsActive()` true; an older MSVC ASan runtime without those exports keeps the diagnostic false and does not annotate.

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

Regression tests: `fuse_core_jobs` — `testParallelSerialFallbackParity`, `testNestedParallelForParity`, `testNestedParallelForSerialFallbackParity`, `testNestedParallelForWithCooperativeWait`, `testNestedParallelForMultiWorkerVisitCount`, `testNestedParallelForStressVisitCoverage` (per-index `atomic<bool>` visit matrix on zero-worker path; avoids aggregate-count flakes), work-steal helper/integration tests (`testWorkSteal*`), `testNestedParallelForVisitCountSingleThread`; `fuse_core_jobs_single_thread` — nested parity + full-grain visit stress under `FUSE_JOBS_SINGLE_THREAD`.

### Remaining (documented, not yet eliminated)

| Surface | Risk | Notes |
|---------|------|-------|
| Eager fiber wake queue | Perf only — workers poll `waitingOn->isComplete()` today | Explicit wake list deferred; avoids cross-thread `WorkerState` mutation |
| `JobCounter::wait()` CV path (game/submit thread) | Expected — mutex + `condition_variable` for non-worker waiters | Not a fiber hot path; documented in Shipped § above |
| Work-stealing deque locks | Contention only — each queue has its own mutex | TSan-clean; perf tuning deferred |
| `parallel_for` body captures `&body` | User responsibility — lambdas with stale references across nested `parallel_for` are UB | Tests use stack-local functors; callers must not capture temporaries |
| Fiber stack depth | Logic bug, not a data race — up to 128 parked job fibers per worker; nested `parallel_for` + `wait()` is OK, arbitrary coroutine depth is not | See fiber pool cap row above |
| Concurrent nested `parallel_for` on all workers | **Deadlock** when every worker is cooperatively waiting on an inner counter and none steal (by design) | Safe pattern: single outer chunk (`grain >= outer`) or serial outer driver; see `fuse_core_jobs` nested parity tests |
