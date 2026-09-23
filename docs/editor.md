# Editor

The editor is a **desktop** Qt 6 application. Mobile builds ship `fuse_runtime` only.

## Pieces

| Target | Role |
|--------|------|
| `fuse_editor_api` | Qt-free boundary: `CommandQueue`, `EditorHost` |
| `fuse_editor` | Qt 6 Widgets shell (optional binary) |

**Locked rule:** the UI thread **posts** `EditorCommand` envelopes; the game thread **drains** them in `EditorHost::gameTick()`. No raw scene pointers cross that boundary — only `fuse::Handle<T>` in payloads.

Engine libraries must not `#include` Qt headers.

## Platform policy

| Platform | Ships |
|----------|-------|
| Windows, Linux, macOS | Runtime + optional editor |
| iOS, Android | Runtime only |

`FUSE_BUILD_EDITOR` is ignored (CMake warning) when `FUSE_PLATFORM_MOBILE` is set.

## Build with Qt 6

Requires Qt 6.5+ Widgets.

### Linux

```bash
sudo apt-get install -y qt6-base-dev

cmake -B build-editor -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_EDITOR_API=ON \
  -DFUSE_BUILD_EDITOR=ON \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF

cmake --build build-editor --target fuse_editor
./build-editor/Source/FUSE/Editor/fuse_editor --samples Samples/unification
```

### Windows / macOS

Install Qt 6 Widgets. Pass `CMAKE_PREFIX_PATH` or `Qt6_DIR` if CMake cannot find it, then use the same flags.

When Qt is absent, sources still ship. CI runs the headless host tests only (`FUSE_BUILD_EDITOR=OFF`).

## Headless tests (no Qt)

| CTest | Binary | Purpose |
|-------|--------|---------|
| `fuse_editor_command_queue` | `fuse_editor_api_tests` | Post / drain |
| `fuse_editor_host` | `fuse_editor_host_tests` | `EditorHost` game-tick without Qt |

## Intended chrome

Track B panels (in progress): project hub, viewport, gizmos, hierarchy, inspector, material editor, SDF sculpt, asset browser, profiler, console, play mode. Viewport embeds the renderer via a native window container; gizmos draw into the viewport, not a third-party immediate-mode UI.

Current tree: a project hub, the embedded Vulkan viewport (software placeholder as headless fallback), and command drain.

### Embedded Vulkan viewport

The live editor presents its viewport through a native Vulkan child window (`ViewportVulkanWindow`, `surfaceType() == VulkanSurface`) embedded with `QWidget::createWindowContainer`:

1. Before the game thread starts, `ViewportPlaceholderWidget::enableEmbeddedVulkanViewport` asks `RuntimeViewportHook::requestWindowSystemPresent` for an instance with the Qt platform's WSI extensions (`VK_KHR_surface` + `VK_KHR_xcb_surface` / `_wayland_surface` / `_win32_surface`).
2. The game thread's first tick creates the viewport renderer (`HybridRendererBootstrap` → `RhiContext`) on that instance and publishes the `VkInstance` (`windowPresentInstance()`, state `InstanceReady`).
3. The UI thread adopts it (`QVulkanInstance::setVkInstance`), embeds the child window, creates the surface (`QVulkanInstance::surfaceForWindow`) and posts `viewport.vk_surface_adopted`.
4. The game thread builds a real `VkSwapchainKHR` on that surface (`SurfaceWired`) and every tick runs fence wait → acquire → render/submit → `vkQueuePresentKHR`. Viewport resizes rebuild the swapchain (GPU drained first).
5. Key / mouse / wheel / focus events that land on the child window are forwarded to the viewport widget, so fly camera (RMB + WASDQE, wheel = dolly / fly speed), picking and the entity context menu behave as before.

Teardown (`MainWindow::~MainWindow`): stop the game thread → `releaseWindowSurface()` (drain, swapchain replaced by a headless one) → `releaseVulkanViewport()` (child window + Qt surface, then the adopted `QVulkanInstance`) → the FUSE `VkInstance` goes with `EditorHost`.

Fallback: without a display, an xcb/wayland/windows platform, a Vulkan device with WSI, or with `FUSE_EDITOR_HEADLESS_VIEWPORT=1`, the viewport stays on the headless render path and paints the software placeholder. `FUSE_EDITOR_VK_VALIDATION=1` enables `VK_LAYER_KHRONOS_validation` on the viewport instance.

**Track B gate.** `vkQueuePresentKHR` is Track B production (`productionPresentAllowed()`, compile-time `FUSE_TRACK_B_UNLOCK`). The editor does not flip that global unlock: once its surface is wired it sets the host-scoped runtime flag `fuse::core::TrackBHostFeature::EditorViewportPresent` for its own process, which also satisfies the Qt present gate (`desktopQtPresentRuntimeReady`) without `FUSE_ENABLE_QT_PRESENT`. The game runtime never sets it; shipping builds ignore it.

Gate: `fuse_editor_qt_live_present` (labels `gate;qt;xvfb`) drives the real `MainWindow` with the game thread running and checks acquire / present counts, screen pixels (rendered frame, not the placeholder), resize → swapchain recreate, forwarded input, 0 validation messages and a clean teardown.
