# U6 — Qt 6 Editor Shell (WP-08)

**Phase:** U6 / WP-08 vertical slice  
**Date:** 2026-09-19  
**Status:** Inspector + coalesced UI property undo + runtime embed with `PresentPath`; **live Qt GPU viewport present** on X11 (`RuntimeViewportHook::tickWindowPresent_`, editor-scoped `TrackBHostFeature::EditorViewportPresent`, CI gate `fuse_editor_qt_live_present`)

---

## 1. Scope

| Target | Role |
|--------|------|
| `fuse_editor_api` | Qt-free boundary: `CommandQueue`, `EditorHost`, `FeaturePaneBridge` |
| `fuse_editor` | Qt 6 desktop executable (optional; gated on `find_package(Qt6)`) |

**Architecture rule (locked):** UI thread **posts** `EditorCommand` envelopes; game thread **drains** via `EditorHost::gameTick()`. No raw `SceneObject*` (or other scene pointers) cross the UI boundary — only `fuse::Handle<T>` in command payloads.

See [architecture-parallel.md](./architecture-parallel.md) §2.1–§3.1 for process/thread model.

---

## 2. Desktop vs mobile policy

| Platform | Ships |
|----------|-------|
| **Desktop** (Windows, Linux, macOS) | `fuse_runtime` + optional `fuse_editor` |
| **Mobile** (iOS, Android) | **`fuse_runtime` only** — no editor binary in product builds |

`FUSE_BUILD_EDITOR` is ignored (with a CMake warning) when `FUSE_PLATFORM_MOBILE` is set. Mobile CI and store builds should leave `FUSE_BUILD_EDITOR=OFF`.

---

## 3. CMake options

| Option | Default | Description |
|--------|---------|-------------|
| `FUSE_BUILD_EDITOR_API` | `ON` | Build `fuse_editor_api` + headless tests |
| `FUSE_BUILD_EDITOR` | `OFF` | Build `fuse_editor` Qt shell when Qt 6 Widgets is found |
| `FUSE_BUILD_CORE_TESTS` | `ON` | Registers `fuse_editor_command_queue` and `fuse_editor_host` CTest entries |
| `FUSE_ENABLE_QT_PRESENT` | `OFF` | Compile-time Qt WSI present gate on `fuse_rhi` (`desktopQtPresentEnabled()`). **Not required** for `fuse_editor` live viewport — the editor opts in at runtime via `EditorViewportPresent` once the Qt Vulkan surface is wired (see [editor.md](../editor.md#embedded-vulkan-viewport)). |
| `FUSE_TRACK_B_UNLOCK` | `OFF` | Global Track B production unlock (`VulkanProduction`). The editor does **not** need this for viewport present; it uses the host-scoped flag above instead. |

`fuse_editor` uses `find_package(Qt6 COMPONENTS Widgets QUIET)`. When Qt is absent, **sources are still shipped** and CI runs the headless host tests only.

---

## 4. Build locally (with Qt 6)

### Linux (Ubuntu/Debian)

```bash
sudo apt-get install -y qt6-base-dev

cmake -B build-editor -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_EDITOR_API=ON \
  -DFUSE_BUILD_EDITOR=ON

cmake --build build-editor --target fuse_editor
./build-editor/Source/FUSE/Editor/fuse_editor --samples Samples/unification
```

### macOS / Windows

Install Qt 6.5+ (Widgets module). Point CMake at your Qt installation (`CMAKE_PREFIX_PATH` or `Qt6_DIR`) and use the same flags as above.

---

## 5. Headless CI path (no Qt)

Linux umbrella CI configures with `FUSE_BUILD_EDITOR=OFF` (default). Tests:

| CTest name | Binary | Purpose |
|------------|--------|---------|
| `fuse_editor_command_queue` | `fuse_editor_api_tests` | Mutex-backed queue post/drain + SetProperty coalescing |
| `fuse_editor_host` | `fuse_editor_host_tests` | PIE, inspector props, UI coalesced property undo/redo, `RuntimeViewportHook` |
| `fuse_editor_runtime_embed` | `fuse_editor_runtime_embed_tests` | WSI backend label, headless GPU path counters, swapchain wiring attempts, project world load |

```bash
cmake -B build-fuse -G Ninja \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_EDITOR_API=ON \
  -DFUSE_BUILD_EDITOR=OFF

cmake --build build-fuse
ctest --test-dir build-fuse -R fuse_editor --output-on-failure
```

These targets exercise `RuntimeViewportHook` and embed counters on the **headless** GPU path (`FUSE_BUILD_EDITOR=OFF`). They do **not** call `vkQueuePresentKHR` on a Qt window.

### Qt live GPU present (CI + local)

The real editor viewport presents through an embedded native Vulkan child window (`QWidget::createWindowContainer`). The game thread runs `RuntimeViewportHook::tickWindowPresent_()`: publish a WSI-capable `VkInstance` → UI adopts it and posts `viewport.vk_surface_adopted` → game thread wires an external swapchain → acquire / render / **`vkQueuePresentKHR`** each tick. Details: [editor.md](../editor.md#embedded-vulkan-viewport).

| Prerequisite | Notes |
|--------------|-------|
| **Display** | Real desktop or **Xvfb** (`1920×1080×24`, `QT_QPA_PLATFORM=xcb`; CI sets this on Qt gate tests) |
| **Vulkan ICD** | Linux CI uses Mesa Lavapipe: `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json` |
| **Qt 6 Widgets + xcb** | `qt6-base-dev`, xcb libs (see `FUSE_APT_LINUX_QT_EDITOR` in [fuse-umbrella-linux.yml](../../.github/workflows/fuse-umbrella-linux.yml)) |
| **Build** | `FUSE_BUILD_EDITOR=ON`, `FUSE_BUILD_VULKAN=ON`, Release recommended for timing gates |

**Opt-out / fallback:** no display, unsupported WSI, no device, `FUSE_EDITOR_HEADLESS_VIEWPORT=1`, or failed surface wiring → software placeholder + headless embed counters (umbrella **core** job path).

**Local reproduce (matches CI `fuse-editor-qt` job):** see [building.md](../building.md#qt-editor-gates-local-playbook) — separate `build/editor-qt` tree, `cmake --build … fuse_editor fuse_editor_qt_gates_all`, then `ctest -L qt` under `xvfb-run`. For a single slow gate: `ctest -R fuse_editor_qt_live_present -j1`.

| CTest / job | When it runs | What it proves |
|-------------|--------------|----------------|
| **`fuse-editor-qt`** (umbrella workflow) | Every push/PR to `main` / `development` | Full Qt gate suite under `xvfb-run`, including live present |
| **`fuse_editor_qt_live_present`** (`gate;qt;xvfb`) | Inside `fuse-editor-qt` | Real `MainWindow`, game thread running, `SurfaceWired`, acquire/present counts, on-screen pixels ≠ placeholder, resize recreate, input forward, validation clean teardown |
| **`fuse_editor_qt_present_adopt`** | Same job | Instance adopt path before full present stress |
| **`fuse_editor_host`**, **`fuse_editor_runtime_embed`** | Umbrella **core** job (`FUSE_BUILD_EDITOR=OFF`) | Headless embed + WSI wiring counters only |

Optional CMake preset: **`fuse-editor-qt`** (Release editor + Vulkan, mirrors CI configure).

**U8 / runtime present (related, not the editor shell):** game-window present uses the same host unlock pattern (`fuse::demo::present::HostPresentUnlock` → `EditorViewportPresent`) for parity demos with `--present` / `FUSE_DEMO_PRESENT=1`, and hybrid regression gate **`fuse_hybrid_frame_vk_swapchain`** (X11 `GameWindow` under the display wrapper). See [demo-corpus-parity-targets.md](./demo-corpus-parity-targets.md) §5 and [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) (WP-06m editor present checklist).

---

## 6. Runtime layout (this slice)

```
┌─────────────────────────────────────────────────────────────┐
│ fuse_editor (single process)                                 │
│  ┌──────────────────┐   ┌───────────────────────────────┐ │
│  │ Qt UI thread     │   │ Game thread (QThread + QTimer) │ │
│  │  ProjectHub      │   │  EditorHost::gameTick()        │ │
│  │  PropertyPane    │──▶│  CommandQueue::drain()         │ │
│  │  Viewport embed  │   │  PlaySession tick while PIE    │ │
│  │  postFromUi()    │   │  RuntimeViewportHook embed     │ │
│  └──────────────────┘   └───────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

- **Project hub** — lists `Samples/unification/*/project.json` directories.
- **Property pane (WP-08 hook)** — `PropertyPaneWidget` + `FeaturePaneBridge`; Play/Stop posts `StartPlay`/`StopPlay` commands (no direct scene mutation from UI thread). Position spinboxes post `SetProperty` envelopes that coalesce on the UI queue and record undo via the game-thread `CommandStack`.
- **Runtime viewport hook** — `RuntimeViewportHook` + `RuntimeEmbedSession`: loads project default `.fuselevel` when `project.root` is posted, mirrors editor ECS entities into `runtimeScene`, runs hybrid embed ticks; with a wired Qt Vulkan surface, `tickWindowPresent_()` acquires/presents on the real swapchain (otherwise headless `PresentPath` / software placeholder).
- **Open project** — posts `SetProperty { project = <name> }` and optional `project.root = <dir>` to the queue (handle-only command envelope).

---

## 7. Public API headers

| Header | Notes |
|--------|-------|
| `fuse/editor/command_queue.hpp` | `EditorCommand`, `CommandQueue` (mutex + deque); kinds include `Undo`/`Redo` |
| `fuse/editor/editor_host.hpp` | `EditorHost` — `postFromUi()` / `gameTick()` + in-process PIE |
| `fuse/editor/feature_pane_bridge.hpp` | Feature-pane hook — posts commands for play/stop/selection/property edits; game-thread `CommandStack` undo |
| `fuse/editor/command_stack.hpp` | Property-edit undo/redo — owned by `EditorHost`, drained on `gameTick()` |
| `fuse/editor/runtime_viewport.hpp` | Viewport ↔ runtime scene embed hook (headless-safe) |
| `fuse/editor/runtime_embed_session.hpp` | Embed session counters (world load, WSI backend, GPU submit, mirror) |

---

## 8. Done vs remaining (WP-08)

| Done (this slice) | Remaining |
|-------------------|-----------|
| Mutex-backed command queue with SetProperty coalescing on UI thread | Timelines, addon feature panes |
| `EditorHost` routes UI property edits through `CommandStack` undo on game thread | Measured renderer latency of a slider drag on real GPU / display (not asserted in CI) |
| Headless cross-thread queue proof (32 UI posts → game drain) | Live mesh/material preview from cooked assets in the wired viewport (counters exist; visual parity not gated) |
| `FeaturePaneBridge` + property edits (`transform`, `mesh.material_id`, `sdf.blend_alpha`) through queue; E20 (UNI-U6-INSP-1): typed Qt editors for every schema component field (`component_schema.hpp`: float / vec3 / Euler / colour / enum / bool / integer / asset path) posting SetProperty through the queue, AddComponent / RemoveComponent command kinds + inspector menu, hierarchy drag-and-drop reparent keeping the world pose — `fuse_editor_e20_inspector_commands`, `fuse_editor_qt_e20_inspector` / `_reparent` | Runtime glslang source compile behind `FUSE_SHADER_GLSLANG` |
| `PropertyInspector` sections: every registered ECS type (typed: Transform, Mesh, SDF, RigidBody, Collider, Camera, Point/Directional/Spot lights, AudioSource, Script, SpawnMarker, tags; module types read-only); inspector edits mark the Transform dirty and repaint the viewport on the applying tick | — |
| `CommandStack` property-edit undo/redo + coalesced Qt spinbox drags | — |
| ECS `Registry::has_all<Ts...>()` + lazy init (Linux segfault fix); cull light path requires Transform + light | — |
| **Live Qt GPU viewport present** — embedded `ViewportVulkanWindow`, external swapchain wiring, `RuntimeViewportHook::tickWindowPresent_()`, `TrackBHostFeature::EditorViewportPresent`; CI `fuse_editor_qt_live_present` (Xvfb + Lavapipe); headless umbrella core job still uses embed-only tests | — |
| `ViewportSwapchainWiring` + optional `QVulkanInstance` adopt path; `probeQVulkanWindowWsi` probes (headless-safe); `qtLivePresentAttempts` / `qtLivePresentTicks` when `qtRealSurface` wired | — |
| `RuntimeViewportHook` loads manifest world, applies wire bindings + `workerCap`, mirrors editor entities, `PresentPath` / hybrid embed tick counters | — |
| `syncEcsToEmbedWorld3D_` (+ quaternion→yaw/pitch/roll deepen) mirrors editor ECS into embed `World3D`; `HybridRendererBootstrap::runFrame` — `hybridComposerFrames` | — |
| `RuntimeEmbedSession` VFS mounts, async material/shader loads, cook-cache drains, cooked asset bind counters (waves 15–18) | — |
| `MeshSdfPreviewCatalog` mesh/SDF preview stubs in embed tick | — |
| `loadCookedFuseshaderSpirv` offline path in `fuse_rhi` shader compiler (headless-safe) | — |

### GUI-1 — editor GUI pass (2026-10-29)

| Landed | Gate |
|---|---|
| Main toolbar (New / Open / Save / Undo / Redo) ahead of the transport bar (Play / Pause / Stop); File > Open Project (Ctrl+Shift+O); Undo / Redo enabled state follows the unified history | `fuse_editor_qt_shell_toolbar`, `_undo`, `_project` |
| Entity menu: Create Empty (Ctrl+Shift+N) / Cube / SDF Sphere / SDF Box / Point Light / Directional Light / Camera, Duplicate (Ctrl+D), Delete (Del), all undoable | `fuse_editor_qt_shell_entity` |
| Asset browser: Up / Refresh, path label, filter, type icons, double-click enters folders and opens `.fuselevel` scenes (save prompt first) | `fuse_editor_gui_models`, `fuse_editor_qt_shell_assets` |
| Profiler: frame-time graph with a budget line, min / avg / max / fps / over-budget summary, Pause and Clear, fed by the measured game-tick time | `fuse_editor_gui_models`, `fuse_editor_qt_shell_profiler` |
| Window geometry and dock layout saved to QSettings on close and restored on start (`fuse_editor` only; tests keep the default layout) | `fuse_editor_qt_shell_layout` |
| `fuse_editor --project <dir>`, `--software-viewport`, `--screenshot <png> [--screenshot-delay ms]` | `fuse_editor_qt_shell_docks`, `_play` |

All Qt shell gates run offscreen (`QT_QPA_PLATFORM=offscreen`) in `fuse_editor_qt_shell_gate` (part of `fuse_editor_qt_gates_all`).

Still open:

- A gizmo toolbar and in-viewport transform handles. The headless `GizmoSystem` exists, but neither `EditorHost` nor the Qt shell owns one yet.
- A real material dock. `material.*` commands are not applied by `EditorHost`, and the material catalog is never populated.
- Timelines.

The engine-native GUI framework that will eventually replace Qt (UI9) is tracked in [`../plans/FUSE_UI_PLAN.md`](../plans/FUSE_UI_PLAN.md). What is in tree so far is described in [`../ui/README.md`](../ui/README.md).

---

## 9. Related docs

- [editor.md](../editor.md) — embedded Vulkan viewport + `fuse_editor_qt_live_present`
- [building.md](../building.md#qt-editor-gates-local-playbook) — reproduce `fuse-editor-qt` CI locally
- [demo-corpus-parity-targets.md](./demo-corpus-parity-targets.md) — U8 demos; optional `--present` game-window path
- [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) — `FUSE_ENABLE_QT_PRESENT`, Track B present gates
- [work-plan.md](./work-plan.md) — WP-08 status
- [architecture-parallel.md](./architecture-parallel.md) — editor threading
- [U7-PROJECT-FORMAT.md](./U7-PROJECT-FORMAT.md) — `project.json` consumed by hub
- [unified-layout.md](./unified-layout.md) — `Source/FUSE/Editor/` layout
