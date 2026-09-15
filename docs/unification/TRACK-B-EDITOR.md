# Track B — Editor Panels (B6.2–B6.13)

**Status:** B6.2 undo stack + B6.3–B6.5 core panel stubs + B6.6–B6.8 inspector/material/sculpt API stubs + B6.9–B6.12 asset/profiler/console/play-mode stubs landed; **B6.13** Phase 6 integration gate + checklist complete  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B6.2–B6.13  
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
| `PlaySession` | `play_session.hpp` | B6.12 deepen — PIE start/stop, tick-while-playing, ECS dirty-flag restore |
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
- **PIE play session** — `PlaySession` wraps the controller with `EditorState` sync, `tick(dt)` while playing (paused sessions skip ticks), and per-entity `Transform::dirty` + `sceneModified` snapshot/restore on stop.

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
| `fuse_editor_phase6_integration` | `fuse_editor_phase6_integration_tests` | **B6.13** — `UndoStack` + `SceneHierarchyPanel` + `ViewportPanel` headless wiring |
| `fuse_editor_play_session` | `fuse_editor_play_session_tests` | B6.12 deepen — PIE session start/stop, tick-while-playing, dirty-flag restore |

---

## Gates

### B6.2 — Command System & Undo/Redo

- [x] `UndoCommand` interface with `execute` / `undo` / `description`
- [x] `UndoStack` LIFO undo/redo with merge hook for consecutive commands
- [x] `SetObjectNameCommand` — rename with full undo restore
- [x] `ReparentObjectCommand` — scene-graph reparent with parent restore
- [x] `fuse_editor_command_stack` — LIFO, merge collapse, rename/reparent unit tests
- [ ] `MAX_HISTORY` cap and oldest-command eviction (deferred)
- [ ] `TransformCommand` with drag merge (deferred — needs gizmo wiring)

### B6.3 — Viewport Panel

- [x] `ViewportPanel` headless API — camera state, mode, project label
- [x] `setDimensions` / `needsResize` resize flag
- [x] `tick(dt)` frame counter for game-thread stub
- [ ] Embedded Vulkan surface + framebuffer rebuild (U6 follow-up)
- [ ] Free-camera WASD input + entity picking (deferred)

### B6.4 — In-Viewport Gizmos

- [x] `GizmoSystem` headless drag stub — translate/rotate/scale modes
- [x] Axis-constrained drag delta accumulation
- [ ] Screen-space gizmo rendering + hover highlight (deferred)
- [ ] Snap-to-grid quantisation (deferred)

### B6.5 — Scene Hierarchy Panel

- [x] `HierarchyModel` flatten + parent/child index
- [x] `SceneHierarchyPanel` search filter (case-insensitive)
- [x] `reparentSelection` via `UndoStack` + post-command `refresh`
- [x] `fuse_editor_hierarchy_model` — flatten, search, reparent + undo
- [ ] Drag-and-drop reparent UI (Qt deferred to U6)
- [ ] Right-click create/delete entity menu (deferred)

### B6.6 — Property Inspector Panel

- [x] `PropertyInspector` component section model on FUSE ECS APIs (Qt-free)
- [x] `setTransformPosition` / `setSdfBlendAlpha` live edits via `CommandStack`
- [x] `fuse_editor_panels` — transform + SDF section coverage
- [ ] All component types render without crash (deferred — expand section list)
- [ ] Live renderer preview on slider drag (deferred)

### B6.7 — Material Editor Panel

- [x] `MaterialEditorPanel` catalog/selection + `MaterialEditState`
- [x] Roughness/metallic edits post `EditorCommand` via `CommandStack`
- [x] Optional RHI bridge methods (no-op when `FUSE_BUILD_VULKAN=OFF`)
- [ ] Live material preview render target (deferred)
- [ ] Save/load persistence cycle (deferred)

### B6.8 — SDF Sculpt Panel

- [x] `SdfSculptPanel` brush state — radius, operation, blend alpha
- [x] Stroke spacing suppression + symmetry flag
- [x] Brush stroke posts `EditorCommand` via `CommandStack`
- [ ] GPU SDF volume carve verification (deferred — needs B3.5 SVO)

### B6.9 — Asset Browser Panel

- [x] `AssetBrowser::init(project_root)` filesystem scan
- [x] Extension-based type classification + `setSearchFilter`
- [x] `fuse_editor_panels_b69_b612` asset browser coverage
- [ ] Qt grid/tree chrome (U6 follow-up)

### B6.10 — Profiler Panel

- [x] `ProfilerPanel::pushFrameData` 256-frame ring buffer
- [x] `setPaused(true)` freezes capture
- [x] Frame history count + latest-frame accessor
- [ ] Qt profiler plots (U6 follow-up)
- [ ] Core `FrameProfileData` scope events wired (see [TRACK-B-CORE-B16.md](./TRACK-B-CORE-B16.md))

### B6.11 — Console Panel

- [x] `ConsolePanel::addLog` with `fuse::log::Level`
- [x] Duplicate-line coalescing + level/text filters via `filteredLines()`
- [x] Command exec stub (`executeCommand`)
- [ ] Qt text view chrome (U6 follow-up)

### B6.12 — Play Mode & Scene Simulation

- [x] `PlayModeController` scene snapshot on `enterPlay`
- [x] Pause / resume / stop with snapshot restore
- [x] `PlayModePhysicsState` simulation-active flag
- [x] `fuse_editor_panels_b69_b612` play-mode lifecycle coverage
- [x] `PlaySession` PIE deepen — start/stop, tick while playing, dirty-flag restore (`fuse_editor_play_session`)
- [ ] `PhysicsManager` wiring during play (deferred — B4 integration)
- [ ] Qt play transport toolbar (U6 follow-up)

### B6.13 — Phase 6 Deliverables & Test Suite

**Status:** Headless integration test exercises `UndoStack` + `SceneHierarchyPanel` + `ViewportPanel` together; full production gates from P6 §6.13 remain deferred.

| Deliverable | Location | B6.13 status |
|-------------|----------|--------------|
| Phase 6 integration test | `Source/FUSE/Editor/tests/test_editor_phase6_integration.cpp` | **Done** — `fuse_editor_phase6_integration` (headless, no Qt) |
| Per-component unit tests | `Source/FUSE/Editor/tests/` | **Done** — B6.2–B6.12 targets listed above |
| CI umbrella run | `.github/workflows/fuse-umbrella-linux.yml` | **Done** — `FUSE_BUILD_EDITOR_API=ON`, `FUSE_BUILD_EDITOR=OFF` |

#### Checklist — scaffold landed (B6.2–B6.12) vs deferred (full P6 gates)

| Area | Item | Status | Notes |
|------|------|--------|-------|
| Command system | `UndoStack` LIFO + merge hook | **Done** | `fuse_editor_command_stack` |
| Command system | 100-command undo chain | **Deferred** | No stress harness yet |
| Command system | `MAX_HISTORY` eviction | **Deferred** | — |
| Viewport | Headless camera + resize stub | **Done** | `ViewportPanel` |
| Viewport | Framebuffer rebuild on resize | **Deferred** | Needs Vulkan surface |
| Gizmos | Headless drag delta stub | **Done** | `GizmoSystem` |
| Gizmos | Screen-space axis rendering | **Deferred** | — |
| Hierarchy | Flatten + search + reparent via undo | **Done** | `fuse_editor_hierarchy_model` |
| Hierarchy | Drag-and-drop UI | **Deferred** | Qt U6 |
| Inspector | ECS component sections (Qt-free) | **Done** | `fuse_editor_panels` |
| Material | Catalog + edit state stub | **Done** | `fuse_editor_panels` |
| Sculpt | Brush stroke + spacing | **Done** | `fuse_editor_panels` |
| Asset browser | Filesystem scan + filter | **Done** | `fuse_editor_panels_b69_b612` |
| Profiler | 256-frame ring buffer | **Done** | `fuse_editor_panels_b69_b612` |
| Console | Log buffer + filters | **Done** | `fuse_editor_panels_b69_b612` |
| Play mode | Snapshot enter/pause/resume/stop | **Done** | `fuse_editor_panels_b69_b612` |
| Integration | Undo + hierarchy + viewport in one test | **Done** | `fuse_editor_phase6_integration` |
| Editor startup | All panels initialise < 1 s | **Deferred** | Needs Qt dockspace |
| Theme / DPI | Dark theme + font scaling | **Deferred** | Qt U6 |

#### Integration test flow (headless)

`fuse_editor_phase6_integration` validates B6.2 + B6.5 + B6.3 wiring in one executable:

1. `fuse::core::initialize()`
2. Build scene graph (`root` → `group`, `child`)
3. `EditorHost` owns shared `UndoStack`; `SceneHierarchyPanel` reparents `child` under `group`
4. `ViewportPanel` ticks and records resize while hierarchy state is live
5. `SetObjectNameCommand` via shared undo stack; hierarchy search finds renamed node
6. Two-step undo restores name then parent; hierarchy `refresh` reflects final state

- [x] **B6.13** End-to-end test: `UndoStack` + `SceneHierarchyPanel` + `ViewportPanel` (`fuse_editor_phase6_integration`)
- [x] B6.2–B6.12 per-component CTest targets registered and green under umbrella CI
- [x] B6.2–B6.12 checklist documented in this file (above)
- [ ] Master-plan perf baselines (editor startup < 1 s, 60 fps viewport) — deferred to U6 Qt shell

---

## Related docs

- [U6-EDITOR.md](./U6-EDITOR.md) — WP-08 shell + `CommandQueue`
- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B6.1–B6.13
- [TRACK-B-ECS.md](./TRACK-B-ECS.md) — component types inspected by B6.6
- [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) — `MaterialSystem` bridge for B6.7
- [architecture-parallel.md](./architecture-parallel.md) — editor threading model
