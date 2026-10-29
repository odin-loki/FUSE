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

### GUI-2 … GUI-4 — gizmo, materials, sculpting (2026-10-29)

| Landed | Gate |
|---|---|
| **Transform toolbar** (Select / Move / Rotate / Scale on Ctrl+1..4, Local, Snap). The Qt-free `TransformGizmo` (`transform_gizmo.hpp`) projects X/Y/Z handles at the primary selection, hit-tests them and turns a drag into a `TransformCommand`. Each drag is wrapped in an `UndoStack` macro, so one drag is one undo step. Snapping uses 0.5 units, 15° and 0.1× scale. Plain W/E/R are not bound because they collide with WASD fly. | `fuse_editor_qt_shell_gizmo` |
| **Material Editor dock.** `EditorHost` owns a per-scene `MaterialCatalog` (`material_catalog.hpp`) with a default palette. It is saved and loaded beside the scene as `<scene>.materials` (`FUSEEDMAT 1`). The dock lists the catalog and follows the selected entity's material. It edits base colour (sRGB picker, stored linear), roughness and metallic with a live preview sphere, and supports New and Assign to selection. Every edit is an `UndoStack` command. | `fuse_editor_qt_shell_material` |
| **SDF Sculpt dock** with a *Sculpt in viewport* toggle. A left-drag applies `SdfSculptPanel::applyBrushStroke` at the picked surface, or at the y = 0 ground plane on a miss. One drag is one undo step, and a brush circle follows the cursor. The dock sets brush op, shape, material (from the catalog), radius, strength, spacing, GRIA α and X symmetry. | `fuse_editor_qt_shell_sculpt` |
| `fuse_editor_qt_startup` always runs on a private Xvfb (`FUSE_FORCE_XVFB=1`). On the shared display, startup timing was flaky. | `fuse_editor_qt_startup` |

GUI-4 (Oct 2026):

- **Vulkan overlay.** The gizmo and sculpt-brush circle now draw over the embedded Vulkan viewport through a masked native sibling window (`ViewportOverlayWidget`: XShape on X11, `SetWindowRgn` on Windows), created before the Vulkan container so it never re-creates the native hierarchy under a live swapchain. It repaints only when its change signature (tool, handles, brush hover, camera) changes. `FUSE_EDITOR_NO_VK_OVERLAY=1` turns it off. The software viewport shares the same painter (`paintInteractionOverlay`).
- **Parented gizmo edits.** The gizmo works in the parent frame: a world-space drag is mapped through the parent's inverse linear part, and world-space rotation uses the world axis expressed in the parent frame. (ECS `inverse_affine` handles scale.)
- **Materials reach the renderer.** When the catalog revision changes, the catalog is written into the embedded `World3D` material table (row id == `Mesh::material_id`), and mesh/SDF preview hints carry the catalog colour.
- **Timeline.** `EditorTimeline` keys the 9 Transform channels of any entity on `cinematics::PropertyTrack`s. Every edit is one undo step. Timelines save as a `.timeline` sidecar (`FUSEEDTL 1`) beside the scene. The Timeline dock offers scrubbing, key/move/delete and play.
- **`--select new|<index>`** selects an entity before `--screenshot` (`new` spawns an SDF sphere in front of the camera).
- **Timing gates under load** scale by `FUSE_TIMING_SCALE` or 1.5 x (loadavg / threads)^2 once the box is oversubscribed. The spike gate is skipped under load.

- **Curves and sequencer.** Timeline keys carry tangents (curve editing), and sequencer clips bind to editor timelines.
- **Textured / shader materials.** Catalog materials carry a base-colour texture, a shader (Default / Wood / Metal / Concrete), a UV scale and triplanar mapping (sidecar `FUSEEDMAT 2`). `HybridSceneRenderer` cooks them into layered-material rows; the procedural shaders are evaluated on the GPU in `ml_common.{glsl,slang}` (CPU twin `procedural_materials.cpp`). `fuse_hybrid_frame_vk --mode material` checks on Lavapipe that the three floors differ.
- **No-project viewport** disables the 2D/UI layers, so the composer's placeholder UI quad (the stray white square) is no longer drawn.
- ECS `inverse_affine` now handles scale.

- **Project Settings** (File > Project Settings...) edits `project.json` (name, default 3D/2D worlds, dimensions, modules, worker cap), validates it and reopens the project. **Import Asset** (File > Import Asset..., Ctrl+I) copies files into `assets/<textures|models|audio|scenes|scripts|misc>/` with unique names and rescans the Asset Browser. Gate: `fuse_editor_qt_project_settings`.
- **Prefabs.** Entity > Save Selection as Prefab... writes the selected entity and its Transform descendants to a `.prefab` (a v3 `.fuselevel` image, parents relinked, root unparented). Entity > Instantiate Prefab..., a double-click on a `.prefab` in the Asset Browser, or `prefab.spawn <path>` (`prefab.save <path>` in the console) instantiates it as one undo step and selects the new root. Test: `fuse_editor_e20_inspector_commands`.
- **Play-in-editor polish.** F5 plays (resumes when paused), F6 pauses, Shift+F5 stops, F10 steps. The title shows `[Playing]` / `[Paused]` and the status bar turns green / amber. Save, Save As, prefab actions and the Entity menu are disabled during Play (they act on the edit scene).
- **Validation.** With `VK_LAYER_PATH` pointing at a Khronos validation layer and `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`, the hybrid, cooked-asset and editor present gates (`fuse_hybrid_*`, `fuse_cooked_assets_vk_*`, `fuse_editor_qt_live_present`, `fuse_editor_qt_present_adopt`, `fuse_editor_b6_viewport_resize_vk_gates`) report 0 validation messages on Lavapipe.

- **Linked prefabs.** Instances remember their source `.prefab` and a baseline (sidecar `<scene>.prefablinks`, saved and loaded with the scene). Overrides are found by diffing components against the baseline; the root Transform is the per-instance placement and never counts. The status bar shows `Prefab: <name> (N override(s))`. Entity > Revert Prefab Overrides / Update Prefab Instances / Unlink Prefab Instance (console `prefab.revert` / `prefab.reload` / `prefab.unlink`) are single undo steps. Saving a prefab re-baselines that instance and propagates to the other instances, keeping their overrides. Test: `fuse_editor_e20_inspector_commands` (`testLinkedPrefab`).
- **Cook on import.** Imported models, textures and audio are cooked asynchronously by `fuse_cook` into `assets/cooked/{models,textures,audio}/*.{fusemesh,fusetex,fuseaudio}`. The tool is found via `FUSE_COOK`, next to the editor, the build tree, then `PATH`. Gate: `fuse_editor_qt_project_settings`.
- **GPU bring-up outside the scene lock.** The game loop snapshots window/present state under the lock, builds the Vulkan instance/device (`buildWindowPresentPrewarm`) unlocked, then adopts it under the lock, so UI refreshes are no longer starved during the first Lavapipe tick. Gate: `fuse_editor_qt_live_present`.
- **Edit tools.** Ctrl/Shift-click toggles viewport selection; Edit > Select All (Ctrl+A) / Deselect All (Ctrl+Shift+A); the Move gizmo moves every selected root. Cut / Copy / Paste (`EntityClipboard`, `entity_clipboard.hpp`) copy whole subtrees, each paste is one undo step and pastes are disabled during Play. Edit > Find (Ctrl+F) focuses the hierarchy search; space-separated terms must all match the label, a component name, or `#<id>`. Lights and cameras draw clickable icons in the viewport. Gate: `fuse_editor_qt_edit_tools`; clipboard test in `fuse_editor_e20_inspector_commands`.
- **Build / Package** (File > Build / Package..., Ctrl+Shift+B). `packageProject` copies the project into `<out>/<name>/project/` (skipping dot-dirs, `build/` and old cooked output), cooks every model/texture/audio source, optionally copies a runtime player into `bin/` with `run.sh` / `run.bat` launchers, and writes `package.json` (files, cooked outputs, failures). Gate: `fuse_editor_qt_build_package`.
- **`idle_frames` under load.** The gate was not hung, just slow (~30 ms/frame on an oversubscribed box over 3–6 × 10000 frames). Under load (`timingScale() > 1`) it now runs one 2000-frame pass.
- **Project cook.** `fuse_cook --project <root>` uses `<root>/cook_manifest.json` when present, otherwise scans mesh/texture/audio/`.lua` sources into `assets/cooked/<models|textures|audio|scripts>/<stem>.<fusemesh|fusetex|fuseaudio|fusescript>` (stem collisions get `_N`; dot-dirs, `build/` and cooked output are skipped). Build / Package now cooks with one `fuse_cook --project` call, can bundle non-system shared libraries into `lib/` (Linux, via `ldd`; listed in `package.json`), and `run.sh` sets `LD_LIBRARY_PATH=$PWD/lib`. The `build_package` gate packages `demo_3d_empty` and runs the packaged player with only `lib/` on the library path. Test: `fuse_asset_pipeline` (`testScanProjectCookManifest`).
- **Multi-select gizmos.** Move, Rotate and Scale act on every selected root (`TransformGizmo::applyGroupDelta`): rotate orbits the primary's pivot, scale spreads offsets along the primary's local axes. Roots under a different parent are rotated / scaled in place.
- **Entity names.** Names live in an `EntityNames` sidecar (`<scene>.names`, `fuse-names 1`) since the ECS has no Name component. Rename from the Inspector Name field or the Hierarchy (F2 / context menu); every rename is one undo step, and the hierarchy search matches names. Cut / Copy / Paste keep names.
- **Group Selection** (Entity menu, Ctrl+G; **Ungroup** Ctrl+Shift+G moves children up a level keeping their world pose and deletes the empty group) parents the selected roots, which must share a parent, under a new empty "Group" at their centroid, keeping world positions; one undo step. **Frame Selected** (View menu, F) moves the camera along its view direction so the selection fits. Tests: `fuse_editor_e20_inspector_commands`, gate `fuse_editor_qt_edit_tools`.
- **Preview sun.** Scenes without a `DirectionalLight` (e.g. `demo_3d_empty`, whose mission has only a ground plane) rendered unlit: the dark ground in the screenshots. `SceneFrameDesc::fallbackSun` (via `HybridSceneRenderer::setFallbackSun`) now adds a renderer-owned directional light when the registry has none and removes it once the scene has its own sun. The editor viewport turns it on; `FUSE_EDITOR_NO_PREVIEW_SUN=1` turns it off. Nothing is written into the scene.
- **Duplicate** (Ctrl+D) now copies whole subtrees with their names through `EntityClipboard` (one undo step). **Double-clicking a hierarchy row** selects and frames that entity. The preview sun has a renderer test: `fuse_scene_renderer_vk_fallback_sun_t0`.

Still open:

- `fsr3` / `framegen` reflection-layout gates fail with glslangValidator 16.6 `-Os` (unused cbuffer members are stripped); this is a toolchain issue, not an editor one.
- The MinGW `fuse_ui_*` targets are owned by the FUSE-ui work stream.

The engine-native GUI framework that will eventually replace Qt (UI9) is tracked in [`../plans/FUSE_UI_PLAN.md`](../plans/FUSE_UI_PLAN.md). What is in tree so far is described in [`../ui/README.md`](../ui/README.md).

---

### GUI-5 — editor tools batch (2026-10-30)

| Feature | Where | Notes / gate |
|---|---|---|
| Preferences dialog | Edit > Preferences... (Ctrl+,) — `src/qt/preferences_dialog.*` | General tab: preview sun (live), camera speed, look sensitivity, restore layout. Keyboard Shortcuts tab: every `fuseAction*` action, filter, conflict detection (Apply refused while conflicting), reset. Stored in QSettings `prefs/*` and `shortcuts/<objectName>`, applied at startup. Gate `preferences`. |
| View Mode: Lit / Unlit | View > View Mode (Alt+1 / Alt+2) | `SceneFrameDesc::unlit` -> `GpuScene::setUnlitPreview`: non-layered material rows are uploaded as emissive base colour (albedo zeroed), originals restored when off. Layered materials are unchanged. Renderer test `fuse_scene_renderer_vk_fallback_sun_t0` (unlit view brighter with no lights; rows restored). |
| Show Mesh Bounds | View > Show Mesh Bounds | Mesh local AABBs through the world transform, drawn with the collider overlay. |
| Collider wireframes / editing | View > Show All Colliders; Inspector "Collider" section | Shape, params, friction, layer/mask, trigger, CCD are editable in the Inspector (component schema); the wireframe follows edits. |
| Lua scripts in PIE | Entity > Open Script (Ctrl+E) | Opens the selection's Script `.lua` (relative to the project root) in the system editor. Saving while playing hot-reloads it (`EditorHost::pollHotReload_`, every 15 game ticks; errors keep the old code and print to the Console; covered by `test_e15_console_pie`). |
| Navigation panel | Navigation dock (tabbed with the Inspector) — `src/qt/navmesh_panel.*` | Bakes a `fuse::navigation::GridNavmesh` on XZ from non-trigger colliders above ground, draws blocked cells / grid / test path in cyan, A* "Test Path". |
| Recent projects, Group / Ungroup, Frame Selected, preview sun | File / Entity / View menus | See GUI-4 notes above. |

Gates: `fuse_editor_qt_gates edit_tools` (bounds, view mode, Open Script, navigation) and `preferences`.

Remaining: a true triangle wireframe view (needs a line / polygon-mode raster pipeline in the renderer), collider handles dragged in the viewport, and a terrain sculpt panel (`fuse_terrain` has no ECS component or renderer path yet).

### GUI-6 — wireframe, collider handles, terrain (2026-10-30)

- **Wireframe / Lit Wireframe** view modes (View menu, Alt+3 / Alt+4, `FUSE_EDITOR_VIEW_MODE=2|3`). `MeshRegistry` keeps the decoded unique triangle edges per mesh; the viewport overlay draws Mesh and SDF-proxy edges (selection in orange, capped at 400k segments). This is a CPU overlay, not a GPU line pipeline (that would touch WP-1.4-owned renderer CMake).
- **Collider drag handles**: box (6 faces), sphere (3 radius), capsule (radius X/Z, half-height ±Y). One undo step per drag. Gate: `edit_tools`.
- **Terrain**: `ecs::Terrain` (procedural heightfield + up to 256 Raise/Lower/Flatten stamps, regenerated deterministically), `terrain_builder` (heightfield, chunk mesh, raycast), runtime viewport builds/registers the mesh and assigns `Mesh`, and a **Terrain** dock (create, brush, click or drag-stroke sculpting in the viewport, clear; each click or whole stroke is one undo step). `--spawn terrain` creates a sculpted terrain for screenshots. Limit: 256 stamps per terrain. Gate: `terrain`.

## 9. Related docs

- [editor.md](../editor.md) — embedded Vulkan viewport + `fuse_editor_qt_live_present`
- [building.md](../building.md#qt-editor-gates-local-playbook) — reproduce `fuse-editor-qt` CI locally
- [demo-corpus-parity-targets.md](./demo-corpus-parity-targets.md) — U8 demos; optional `--present` game-window path
- [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) — `FUSE_ENABLE_QT_PRESENT`, Track B present gates
- [work-plan.md](./work-plan.md) — WP-08 status
- [architecture-parallel.md](./architecture-parallel.md) — editor threading
- [U7-PROJECT-FORMAT.md](./U7-PROJECT-FORMAT.md) — `project.json` consumed by hub
- [unified-layout.md](./unified-layout.md) — `Source/FUSE/Editor/` layout

## GUI-7: asset browser polish + timeline usability

- Asset Browser: list/grid toggle (`fuseAssetGridToggle`), type filter (`fuseAssetTypeFilter`), clickable breadcrumb (`fuseAssetBreadcrumb`), status line (`fuseAssetStatus`), size/date tooltips, context menu (Open, Copy Path, Copy Relative Path, Rename…, New Folder…, Refresh). `.fusemesh` classified as Mesh. Gate: `asset_browser`.
- Timeline: Ctrl+wheel zoom (anchored at cursor, 50 ms minimum), Shift+wheel pan, Fit (F); adaptive ruler; Loop (`fuseTimelineLoop`, L); frame snap 24/30/60 fps (`fuseTimelineSnap`; default 10 ms); previous/next key (`[` / `]`, buttons), Home/End. The toolbar no longer forces the window's minimum width. Covered in `shell_timeline`.

## GUI-8: play-mode polish, multi-viewport, screenshot options

- Play mode: the viewport gets a green (playing) / amber (paused) frame with a `PLAYING 3.6 s · 37 ticks` badge (session clock and tick count, frozen while paused). Play > Maximize Viewport on Play (`fuseActionMaximizeOnPlay`) hides the docks while playing and restores the layout on Stop. Covered in `shell_play`.
- Multi-viewport: View > Viewport Layout (`fuseActionViewportSingle/Two/Quad`, Ctrl+Alt+1..3; Alt+1..4 remain the view modes). The perspective view gets orthographic Top (X/Z), Front (X/Y) and Side (-Z/Y) views drawn with QPainter from the edit registry (mesh world AABBs, markers for other entities), so they need no extra swapchain. Click selects (Ctrl/Shift toggles), drag moves the selection in the view plane (one undo step), middle/right drag pans, the wheel zooms at the cursor, F frames the selection. The central widget is now a splitter that contains the viewport. Gate: `multi_viewport`.
- Startup options: `--show-dock <name>` (repeatable; `timeline` also keys the selection so lanes show), `--play`, `--viewport-layout single|two|quad`.
- Play from the current view: PIE keeps the editor camera by default (spawned objects stay in frame); Play > Play from Spawn Point (`fuseActionPlayFromSpawn`) restores the jump to the first spawn point, lifted 1.7 m to eye height. `RuntimeViewportHook::setPlayFromSpawn`.
- Level mesh bounds: the editor scene is rebuilt from the level's scene table, which drops the FECS block, so the hook reads the level's FECS once and copies each Mesh AABB onto the matching editor entity (`importLevelMeshBounds_`). `fillMissingMeshBounds` fills any remaining zero AABB from the registered GPU mesh, or from a Box/Sphere/Capsule/Plane collider. The ortho views now draw the ground as a box.

- Level placement: editor entities created from the level now take the scene table's position/rotation/scale (they were identity), so the GroundPlane shows its real 256 half-extent in the ortho views.
- Ortho auto-frame (startup / layout change, `OrthoViewWidget::frameAuto`) fits the selection, else everything except boxes larger than 100 units (ground planes, terrain); Home/frameAll still fits all. Gates: project_open checks the GroundPlane's 256 half-extent; multi_viewport checks auto-frame vs frameAll with a 512-unit ground.
