# Track B — Core B1.7 Platform Window & Event Pump (deepen follow-up)

**Status:** B1.7 deepen follow-up — resize/focus/close stubs, poll-queue drain/coalesce, FIFO/overflow tests on `fuse_core`  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B1.7  
**Vulkan wiring:** [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §Surface abstraction (External kind)  
**Related:** B7.8 surface-loss hooks (`surface_loss.hpp`) fire when mobile WSI recreates drawables

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `window.hpp` | `Source/FUSE/Core/include/fuse/platform/` | `Window`, `WindowDesc`, `VulkanSurfaceWire` |
| `event_pump.hpp` | same | `EventPump`, `PlatformEvent` queue |
| `platform_window.cpp` | `Source/FUSE/Core/src/platform/` | Desktop + mobile no-op stub backend |
| `test_platform_window.cpp` | `Source/FUSE/Core/tests/` | Window metadata, resize/focus/close notify stubs, poll-queue FIFO/overflow/coalesce, drain helper, Vulkan wire metadata |

**Not in scope (follow-up PRs):** Win32 / X11 / Wayland window creation, Android `ANativeWindow` / iOS `UIView` wiring, raw input (`input.hpp`), DPI awareness, real OS event translation, GLFW.

---

## Build

Window stubs ship inside `fuse_core`:

```bash
cmake -B build -DFUSE_UMBRELLA=ON -DFUSE_BUILD_CORE=ON -DFUSE_BUILD_CORE_TESTS=ON
cmake --build build --target fuse_core_platform_window_tests
ctest --test-dir build -R fuse_core_platform_window
```

| Target | CI |
|--------|-----|
| Linux umbrella | `fuse_core_platform_window` via CTest |
| Android NDK | `fuse_core` compile (same stub TU, mobile profile) |

---

## Window API (stub)

```cpp
#include <fuse/platform/window.hpp>

fuse::platform::WindowDesc desc;
desc.title = "FUSE";
desc.width = 1280;
desc.height = 720;

fuse::platform::Window window(desc);
window.resize(1920, 1080);
window.requestClose();
```

Optional `EventPump*` on `resize`, `setFocused`, and `requestClose` enqueues synthetic lifecycle events when geometry or focus changes (headless tests and future OS backends share the same queue):

```cpp
fuse::platform::EventPump pump;
window.resize(1920, 1080, &pump);   // WindowResized when dimensions change
window.setFocused(false, &pump);      // WindowFocusLost on transition
window.requestClose(&pump);           // WindowCloseRequested (once per close cycle)
```

The B1.7 stub:

- Stores title and geometry in host memory
- Returns `nativeHandle().value == nullptr`
- Returns `nativeVulkanSurface() == nullptr`
- Exposes `vulkanSurfaceWire()` with `presentable == false`

Editor game viewport windowing stays **Qt-owned** (Track A / B6). This API is for standalone game executables.

---

## Event pump API (stub)

```cpp
#include <fuse/platform/event_pump.hpp>

fuse::platform::EventPump pump;
fuse::platform::Window window;

while (pump.pumpOnce()) {
  fuse::platform::PlatformEvent event;
  while (pump.pollEvent(event)) {
    if (event.type == fuse::platform::PlatformEventType::WindowCloseRequested &&
        event.window == &window) {
      pump.requestQuit();
    }
    if (event.type == fuse::platform::PlatformEventType::WindowResized &&
        event.window == &window) {
      // swapchain recreate stub …
    }
    if (event.type == fuse::platform::PlatformEventType::WindowFocusLost &&
        event.window == &window) {
      // pause input / duck audio …
    }
  }
  // game tick …
}
```

`processOsEvents()` is a **no-op** on desktop and mobile until platform backends land. Tests and headless runners may call `pushSyntheticEvent()` or the `pushWindow*` helpers to simulate resize / focus / quit without a display server.

Pending `WindowResized` events for the same window pointer are **coalesced in-place** (latest width/height wins) so rapid resize bursts collapse before the game loop drains the queue.

Poll-queue introspection and drain helpers for tests:

```cpp
if (pump.hasPendingEvents()) {
  const fuse::u32 queued = pump.pendingEventCount();
  // drain or assert FIFO order …
}

std::vector<fuse::platform::PlatformEvent> drained;
const fuse::u32 moved = pump.drainEvents(drained); // FIFO into `drained` (0 when empty)
```

---

## Vulkan External surface wiring (deferred)

When a platform backend creates WSI resources, map the window into the renderer like this:

```cpp
#include <fuse/platform/window.hpp>
#include <fuse/renderer/vk/surface.hpp>

const fuse::platform::VulkanSurfaceWire wire = window.vulkanSurfaceWire();

fuse::renderer::SurfaceDesc surfaceDesc;
surfaceDesc.kind = wire.presentable
    ? fuse::renderer::SurfaceKind::External
    : fuse::renderer::SurfaceKind::Headless;
surfaceDesc.nativeSurface = wire.nativeSurface; // opaque VkSurfaceKHR
```

| Stage | Owner | Responsibility |
|-------|-------|----------------|
| **B1.7 (this PR)** | `fuse::platform::Window` | Stub metadata + null surface |
| **B2.2 (landed)** | `fuse::renderer::VulkanSurface` | Headless vs External abstraction |
| **Follow-up** | Platform Win32 / X11 / Android modules | Create `VkSurfaceKHR`, enable WSI extensions, call `notifySurfaceLost()` / `notifySurfaceRestored()` on mobile context loss |

CI continues to run Lavapipe headless (`SurfaceKind::Headless`). Presentable swapchains activate only when a non-null External surface is supplied.

---

## Tests

| Test binary | CTest name | Coverage |
|-------------|------------|----------|
| `fuse_core_platform_window_tests` | `fuse_core_platform_window` | Window desc storage, resize/focus/close notify stubs, poll-queue FIFO/overflow/coalesce (per-window), empty-queue + ordered `drainEvents`, Vulkan wire metadata, synthetic events, quit flow, mobile profile no-op |

---

## Gates (B1.7 deepen follow-up)

- [x] `fuse::platform::Window` stub on FUSE APIs (not ungated Torque guts)
- [x] `fuse::platform::EventPump` desktop + mobile no-op
- [x] Resize / focus / close-requested synthetic event stubs (`pushWindow*`, optional pump on `Window` mutators)
- [x] Poll-queue introspection (`hasPendingEvents`, `pendingEventCount`) + FIFO/overflow/coalesce tests
- [x] `drainEvents(std::vector<PlatformEvent>&)` helper for headless runners and tests
- [x] Vulkan External surface wire notes (`VulkanSurfaceWire` + this doc)
- [x] Unit tests + this doc
- [ ] Win32 / Linux WSI window backends — deferred
- [ ] Raw input (`Key` / `InputState`) — deferred
- [ ] Real OS event translation — deferred

---

## Related documents

| Doc | Link |
|-----|------|
| Platform hardening (B7.8) | [TRACK-B-PLATFORM.md](./TRACK-B-PLATFORM.md) |
| Vulkan bootstrap | [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) |
| Build matrix | [BUILD.md](./BUILD.md) |
