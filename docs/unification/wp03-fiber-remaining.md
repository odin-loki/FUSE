# WP-03 — Cooperative fiber wait (incremental)

**Status:** Landed (POSIX ucontext on desktop Linux/macOS; Win32 fibers on Windows)  
**Deferred:** Android/iOS dedicated backends, Emscripten coarse pool

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

## Remaining (honest limit)

| Item | Notes |
|------|-------|
| Per-job fiber pools | Today one job fiber per worker; nested waits OK, not arbitrary coroutine depth |
| Android / iOS native backend | Today CV fallback only; future: asm/stackful coroutine or platform fiber API |
| Apple desktop ucontext | Deprecated but used on macOS desktop; migrate when glibc removes ucontext |
| Emscripten | `FUSE_JOBS_SINGLE_THREAD` / coarse pool per architecture-parallel §3.5 |
| Hot-path mutex removal | `JobCounter` still uses mutex for CV fallback and waiter lists |
| I/O + render dependency chains | Needs WP-04 handle commit + WP-06 frame barrier |
| Win32 ASan fiber annotations | POSIX has `__sanitizer_*_switch_fiber`; Windows backend not yet instrumented |

See [architecture-parallel.md](./architecture-parallel.md) §3.2–3.3 and [work-plan.md](./work-plan.md) WP-03.
