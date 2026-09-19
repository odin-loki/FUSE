# U6 — Qt 6 Editor Shell (WP-08)

**Phase:** U6 / WP-08 vertical slice  
**Date:** 2026-09-19  
**Status:** Inspector + coalesced UI property undo + headless runtime embed (null WSI / optional Vulkan submit)

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

`fuse_editor` uses `find_package(Qt6 COMPONENTS Widgets QUIET)`. When Qt is absent, **sources are still shipped** and CI runs the headless host tests only.

---

## 4. Build locally (with Qt 6)

### Linux (Ubuntu/Debian)

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
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_EDITOR_API=ON \
  -DFUSE_BUILD_EDITOR=OFF \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF

cmake --build build-fuse
ctest --test-dir build-fuse -R fuse_editor --output-on-failure
```

---

## 6. Runtime layout (this slice)

```
┌─────────────────────────────────────────────────────────────┐
│ fuse_editor (single process)                                 │
│  ┌──────────────────┐   ┌───────────────────────────────┐ │
│  │ Qt UI thread     │   │ Game thread (QThread + QTimer) │ │
│  │  ProjectHub      │   │  EditorHost::gameTick()        │ │
│  │  PropertyPane    │──▶│  CommandQueue::drain()         │ │
│  │  Viewport stub   │   │  PlaySession tick while PIE    │ │
│  │  postFromUi()    │   │  RuntimeViewportHook embed     │ │
│  └──────────────────┘   └───────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

- **Project hub** — lists `Samples/unification/*/project.json` directories.
- **Property pane (WP-08 hook)** — `PropertyPaneWidget` + `FeaturePaneBridge`; Play/Stop posts `StartPlay`/`StopPlay` commands (no direct scene mutation from UI thread). Position spinboxes post `SetProperty` envelopes that coalesce on the UI queue and record undo via the game-thread `CommandStack`.
- **Runtime viewport hook** — `RuntimeViewportHook` + `RuntimeEmbedSession`: loads project default `.fuselevel` when `project.root` is posted, mirrors editor ECS entities into `runtimeScene`, ticks a headless present stub (`FUSE_VULKAN_BACKEND` submits empty frame when device ready).
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
| Mutex-backed command queue with SetProperty coalescing on UI thread | Full GPU swapchain viewport in Qt widget |
| `EditorHost` routes UI property edits through `CommandStack` undo on game thread | Timelines, addon feature panes |
| Headless cross-thread queue proof (32 UI posts → game drain) | — |
| `FeaturePaneBridge` + property edits (`transform`, `mesh.material_id`, `sdf.blend_alpha`) through queue | Live Qt widgets for mesh/SDF beyond position spinboxes |
| `PropertyInspector` sections: Transform, Mesh, SDF, RigidBody, Camera, Point/Directional/Spot lights | All ECS types + live renderer preview on slider drag |
| `CommandStack` property-edit undo/redo + coalesced Qt spinbox drags | — |
| ECS `Registry::has_all<Ts...>()` + lazy init (Linux segfault fix); cull light path requires Transform + light | — |
| `ViewportSwapchainWiring` consumes External handoff via `VulkanBootstrap::ensureSwapchain` (headless fallback; Qt winId stub short-circuits safely) | Live Qt swapchain present |
| Optional `QVulkanInstance` surface path in viewport widget (winId stub fallback) | — |
| `RuntimeViewportHook` loads manifest world, populates legacy wire table stats, mirrors editor entities (parent indices), null WSI headless GPU stub | Full in-process GPU viewport compositing |

---

## 9. Related docs

- [work-plan.md](./work-plan.md) — WP-08 status
- [architecture-parallel.md](./architecture-parallel.md) — editor threading
- [U7-PROJECT-FORMAT.md](./U7-PROJECT-FORMAT.md) — `project.json` consumed by hub
- [unified-layout.md](./unified-layout.md) — `Source/FUSE/Editor/` layout
