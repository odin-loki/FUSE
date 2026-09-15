# Track B — Editor Panels (B6.2–B6.12)

**Status:** B6.2 undo stack + B6.3–B6.5 core panel stubs + B6.6–B6.8 inspector/material/sculpt API stubs + B6.9–B6.12 asset/profiler/console/play-mode stubs landed  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B6.2–B6.12  
**Threading:** [architecture-parallel.md](./architecture-parallel.md) §2.1–§4, [U6-EDITOR.md](./U6-EDITOR.md)

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `UndoCommand` / `UndoStack` | `Source/FUSE/Editor/include/fuse/editor/undo_stack.hpp` | B6.2 — reversible scene mutations + merge hook |
| `SetObjectNameCommand` / `ReparentObjectCommand` | same | Core hierarchy rename/reparent commands |
| `CommandStack` | `Source/FUSE/Editor/include/fuse/editor/command_stack.hpp` | `EditorCommand` envelope history for B6.6–B6.8 panel stubs |
| `ViewportPanel` | `Source/FUSE/Editor/include/fuse/editor/viewport_panel.hpp` | B6.3 — camera + resize stub |
| `GizmoSystem` | `Source/FUSE/Editor/include/fuse/editor/gizmo_system.hpp` | B6.4 — translate/rotate/scale drag stub |
| `HierarchyModel` / `SceneHierarchyPanel` | `hierarchy_model.hpp`, `scene_hierarchy_panel.hpp` | B6.5 — flatten, search, reparent via `UndoStack` |
| `EditorState` / `EditorScene` | `editor_state.hpp`, `editor_scene.hpp` | Selection + ECS registry for panel tests |
| `PropertyInspector` | `property_inspector.hpp` | B6.6 — component section model + live ECS edits |
| `MaterialEditorPanel` | `material_editor_panel.hpp` | B6.7 — catalog/selection + `MaterialEditState` |
| `SdfSculptPanel` | `sdf_sculpt_panel.hpp` | B6.8 — brush state + stroke spacing / symmetry |
| `AssetBrowser` | `asset_browser.hpp` | B6.9 — filesystem scan + type classification + search filter |
| `ProfilerPanel` | `profiler_panel.hpp` | B6.10 — 256-frame ring buffer of `FrameProfileData` |
| `ConsolePanel` | `console_panel.hpp` | B6.11 — log buffer, level/text filters, command exec stub |
| `PlayModeController` | `play_mode_controller.hpp` | B6.12 — scene snapshot / restore + `PlayModePhysicsState` flag |
| `EditorHost` | `editor_host.hpp` | `CommandQueue` (UI→game) + `UndoStack` (B6.2) |

**Not in scope:** Qt dock widgets, embedded Vulkan viewport, `QUndoStack` adapter, live material preview RT, real picking/rendering, Qt asset grid/profiler plots/console chrome.

---

## Design

### Three command paths

| Path | Type | Thread | Role |
|------|------|--------|------|
| UI → game envelope | `EditorCommand` + `CommandQueue` | UI posts, game drains | WP-08 async boundary |
| Panel property edits | `EditorCommand` + `CommandStack` | Game thread | B6.6–B6.8 stub history + pending queue |
| Undo/redo | `UndoCommand` + `UndoStack` | Game thread | B6.2 non-destructive scene graph edits |

`EditorHost` owns `CommandQueue` and `UndoStack`. B6.6–B6.8 panels accept `CommandStack&` for envelope recording.

### Headless panel stubs

`ViewportPanel`, `GizmoSystem`, and `SceneHierarchyPanel` are API-only stubs in `fuse_editor_api`. The optional Qt shell (`FUSE_BUILD_EDITOR`) keeps its existing placeholder widget.

### B6.9–B6.12 stubs

- **Asset browser** — `AssetBrowser::init(project_root)` scans with `std::filesystem`; entries classified by extension; `setSearchFilter` narrows results.
- **Profiler panel** — `ProfilerPanel::pushFrameData` writes a 256-frame ring buffer; `setPaused(true)` freezes capture.
- **Console panel** — `ConsolePanel::addLog` uses `fuse::log::Level`, coalesces duplicate lines, supports level/text filters via `filteredLines()`.
- **Play mode** — `PlayModeController` snapshots `fuse::scene::Scene` on `enterPlay` and restores on `stop`; `PlayModePhysicsState` tracks simulation-active flag until `PhysicsManager` wiring lands.

---

## Build

`fuse_editor_api` builds with `FUSE_BUILD_EDITOR_API=ON` (default in CI). Tests require no Qt. Play mode requires `FUSE_BUILD_PROJECT=ON` (default) for `fuse_scene`.

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_PROJECT=ON \
  -DFUSE_BUILD_EDITOR_API=ON \
  -DFUSE_BUILD_EDITOR=OFF \
  -DFUSE_BUILD_VULKAN=ON

cmake --build build
ctest --test-dir build --output-on-failure -R fuse_editor
```

| Condition | Behaviour |
|-----------|-----------|
| `FUSE_BUILD_EDITOR_API=OFF` | No `fuse_editor_api` target |
| `FUSE_BUILD_VULKAN=OFF` | Material panel RHI bridge methods no-op; ECS/sculpt tests still run |
| `FUSE_BUILD_EDITOR=ON` + Qt6 | Desktop shell links `fuse_editor_api` (panels not yet wired into docks) |

---

## Tests

| CTest name | Binary | Validates |
|------------|--------|-----------|
| `fuse_editor_command_queue` | `fuse_editor_api_tests` | UI→game command queue |
| `fuse_editor_host` | `fuse_editor_host_tests` | `EditorHost::gameTick()` drain |
| `fuse_editor_panels` | `fuse_editor_panels_tests` | B6.6–B6.8 inspector/material/sculpt API |
| `fuse_editor_command_stack` | `fuse_editor_command_stack_tests` | B6.2 `UndoStack` LIFO, merge, rename/reparent |
| `fuse_editor_hierarchy_model` | `fuse_editor_hierarchy_model_tests` | B6.5 flatten, search, reparent + undo |
| `fuse_editor_panels_b69_b612` | `fuse_editor_panels_b69_b612_tests` | B6.9–B6.12 asset/profiler/console/play-mode stubs |

---

## Gates

### B6.2–B6.5

- [x] `UndoStack` with execute/undo/redo + merge hook
- [x] `ViewportPanel` headless API (camera, resize flag, tick)
- [x] `GizmoSystem` headless drag stub
- [x] `HierarchyModel` + `SceneHierarchyPanel` with search + reparent command
- [ ] Embedded viewport + real gizmo rendering (follow-up)

### B6.6–B6.8

- [x] **B6.6** `PropertyInspector` on FUSE ECS APIs (Qt-free)
- [x] **B6.7** `MaterialEditorPanel` stub with optional RHI bridge
- [x] **B6.8** `SdfSculptPanel` brush + stroke command stub
- [ ] Qt 6 dock wiring for panels (follow-up under `FUSE_BUILD_EDITOR`)

### B6.9–B6.12

- [x] **B6.9** `AssetBrowser` on FUSE APIs (filesystem model, no Qt)
- [x] **B6.10** `ProfilerPanel` frame history ring buffer
- [x] **B6.11** `ConsolePanel` log buffer + filters + exec stub
- [x] **B6.12** `PlayModeController` snapshot enter/pause/resume/stop
- [ ] Qt chrome for asset grid, profiler plots, console view, play transport (U6 follow-up)

---

## Related docs

- [U6-EDITOR.md](./U6-EDITOR.md) — WP-08 shell + `CommandQueue`
- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B6.1–B6.13
- [TRACK-B-ECS.md](./TRACK-B-ECS.md) — component types inspected by B6.6
- [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) — `MaterialSystem` bridge for B6.7
- [architecture-parallel.md](./architecture-parallel.md) — editor threading model
