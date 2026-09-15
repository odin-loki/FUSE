# Track B — Platform Hardening (B7.8 scaffolds)

**Status:** B7.8 lifecycle, surface-loss, crash-report, and profile stubs on `fuse_core`  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.8  
**Architecture:** [architecture-parallel.md](./architecture-parallel.md) §3.1.1, §3.6, §4.4  
**Jobs integration:** `computeWorkerCountForCurrentPlatform()` reads `platform::isMobileProfile()` and `getPowerState()`

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `lifecycle.hpp` | `Source/FUSE/Core/include/fuse/platform/` | Foreground/background hooks; drives `PowerState::Background` |
| `power.hpp` | same | `getPowerState()`, thermal/low-power callbacks (stub state machine) |
| `surface_loss.hpp` | same | GPU drawable loss/restore callbacks (§4.4 mobile context loss) |
| `crash_report.hpp` | same | Install/submit crash report path — no OS signal wiring yet |
| `profile.hpp` | same | Desktop vs mobile job profile limits (§3.1.1 table) |
| `platform_hardening.cpp` | `Source/FUSE/Core/src/platform/` | Stub implementations + callback dispatch |
| `platform_stub.cpp` | same | Thread/core queries; fiber stack from `currentJobProfileLimits()` |

**Not in scope (follow-up PRs):** Win minidump / Linux `sigaction` handlers, DPI awareness, shipping strip macros, leak detector, real iOS/Android lifecycle wiring.

---

## Build flag — `FUSE_BUILD_CORE`

Platform hardening ships inside `fuse_core` (always on when core builds):

```bash
cmake -B build -DFUSE_UMBRELLA=ON -DFUSE_BUILD_CORE=ON -DFUSE_BUILD_CORE_TESTS=ON
cmake --build build --target fuse_core_platform_hardening_tests
ctest --test-dir build -R fuse_core_platform_hardening
```

| Target | CI |
|--------|-----|
| Linux umbrella | `fuse_core_platform_hardening` via CTest |
| Android NDK | `fuse_core` compile (mobile profile compile defs) |

---

## B7.8 — Lifecycle hooks (scaffold)

When the OS backgrounds the app (mirrors T2D `backgrounded`):

1. Platform backend calls `notifyAppVisibility(AppVisibility::Background)`
2. `getPowerState()` becomes `Background` → job scheduler uses background profile (max 1 worker)
3. Registered lifecycle callbacks fire on the game thread

Foreground resume restores `PowerState::Normal` unless the platform sets thermal/low-power explicitly via `setPowerState()`.

```cpp
fuse::platform::registerLifecycleCallback([](fuse::platform::AppVisibility v) {
  if (v == fuse::platform::AppVisibility::Background) {
    // pause speculative I/O, duck audio, etc.
  }
});
fuse::platform::notifyAppVisibility(fuse::platform::AppVisibility::Background);
```

See architecture-parallel §3.6 for worker pool, I/O, render, audio, and net behaviour tables.

---

## B7.8 — Surface loss (scaffold)

Portable GPU surface lifecycle for GLES/Metal/Vulkan mobile backends:

```cpp
fuse::platform::registerSurfaceLossHandlers(
  [] { /* invalidate swapchain / idle workers */ },
  [] { /* recreate surface on render thread */ });
fuse::platform::notifySurfaceLost();
// ... recreate drawable ...
fuse::platform::notifySurfaceRestored();
```

`isSurfaceValid()` is false between loss and restore. Only `renderThread()` may recreate GPU context (§4.4).

---

## B7.8 — Crash report (scaffold)

FUSE-facing crash path without linking Torque platform guts:

```cpp
fuse::core::initialize(); // calls installCrashHandlers()
fuse::platform::setCrashReportCallback([](const fuse::platform::CrashReportContext& ctx) {
  fuse::log::error("crash: %s (%s:%u)", ctx.message, ctx.file, ctx.line);
});
fuse::platform::submitCrashReport({"manual report", __FILE__, __LINE__});
```

Future: Win32 minidump + Linux signal handlers behind `installCrashHandlers()` (master plan §B7.8).

---

## B7.8 — Desktop + mobile profile hooks

Locked limits from architecture-parallel §3.1.1:

| Profile | `reserve` | `min` | `max` | Fiber stack | I/O budget / frame |
|---------|-----------|-------|-------|-------------|-------------------|
| **Desktop** | 2 | 1 | 16 | 64 KiB | 8000 µs (8 ms) |
| **Mobile** | 1 | 1 | 4 | 32 KiB | 2000 µs (2 ms) |

```cpp
const auto limits = fuse::platform::currentJobProfileLimits();
// limits.maxWorkers, limits.fiberStackBytes, limits.ioBudgetMicrosPerFrame
```

Compile-time target sets default profile (`FUSE_PLATFORM_MOBILE`). Tests may override via `setActiveProfileOverride()`.

`recommendedFiberStackBytes()` and `computeWorkerCountForCurrentPlatform()` consume these hooks.

---

## Init / shutdown order

1. `fuse::core::initialize()` — job scheduler, `registerRenderThread()`, `installCrashHandlers()`
2. Platform backends register lifecycle / surface-loss handlers before main loop
3. `fuse::core::shutdown()` — `shutdownCrashHandlers()`, job scheduler shutdown

All steps are idempotent.

---

## Tests

| Test | Coverage |
|------|----------|
| `fuse_core_platform_hardening` | Lifecycle → power state, surface loss/restore, crash callback, desktop/mobile profile limits, core init installs crash handlers |

---

## Checklist

- [x] B7.8 stubs on FUSE `fuse::platform` APIs (not ungated Torque guts)
- [x] Lifecycle foreground/background hooks
- [x] Surface-loss stub
- [x] Crash report stub
- [x] Desktop + mobile profile hooks aligned with architecture-parallel §3.1.1
- [x] Narrative tests green on Linux CI
- [ ] OS crash handlers (Win minidump / Linux signals) — deferred
- [ ] Real mobile lifecycle wiring (iOS/Android backends) — deferred

---

## Related documents

| Doc | Link |
|-----|------|
| Parallel architecture | [architecture-parallel.md](./architecture-parallel.md) |
| Build matrix | [BUILD.md](./BUILD.md) |
| Job worker formula | WP-03 / `worker_count.hpp` |
| GFX threading | [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §Thread ownership |
