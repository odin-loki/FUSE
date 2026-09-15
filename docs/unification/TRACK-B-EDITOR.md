# Track B — Editor Panels (B6.2–B6.13)

**Status:** B6.2 undo stack + B6.3–B6.5 core panel stubs + B6.6–B6.8 inspector/material/sculpt API stubs + B6.9–B6.12 asset/profiler/console/play-mode stubs landed; **B6.13** Phase 6 integration gate + checklist complete; **B6.12 deepen** — `PlaySession` tick accumulator, `PlayWorldSnapshot` ECS capture/restore, coalesced dirty restore on stop; **B6.2 deepen** — `CommandStack` `push`/undo/redo, coalescing (`propertyValueBefore` baseline), dirty tracking, `peekUndo`/`peekRedo`, `evictedCount`, full snapshot restore + `UndoStackSnapshot` depth rewind restore + dirty stub; **B6.4 deepen** — `GizmoSystem` ray axis/plane hit tests, local/world delta helpers, translate/rotate snap stubs, `CommandStack`/`EditorState` dirty marking; **B6.4 deepen follow-up** — `cycleGizmoMode`/`cycleMode`, screen dead-zone miss stub (`isScreenHitMiss`), scale grid snap (`scaleSnap`/`snapScale`), hit-test miss + snap grid tests; **B6.7 deepen** — material property bindings (`roughness`/`metallic`/`baseColor`/`shadingModel`), `editDirty`/`previewDirty` flags, `MaterialSystem` push/sync bridge tests; **B6.7 deepen follow-up** — `MaterialPropertyBinding` bind/get/set stubs, per-property dirty coalesce, `needsPanelRefresh`/`refreshPanel` helpers  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B6.2–B6.13  
**Threading:** [architecture-parallel.md](./architecture-parallel.md) §2.1–§4, [U6-EDITOR.md](./U6-EDITOR.md)

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `UndoCommand` / `UndoStack` / `UndoStackSnapshot` | `Source/FUSE/Editor/include/fuse/editor/undo_stack.hpp` | B6.2 — reversible scene mutations + merge hook; snapshot depth rewind restore + dirty stub |
| `SetObjectNameCommand` / `ReparentObjectCommand` | same | Core hierarchy rename/reparent commands |
| `CommandStack` / `CommandStackSnapshot` | `Source/FUSE/Editor/include/fuse/editor/command_stack.hpp` | `EditorCommand` envelope history for B6.6–B6.8 panel stubs; coalescing with `propertyValueBefore`, dirty tracking, `peekUndo`/`peekRedo`, snapshot restore (B6.2 deepen) |
| `ViewportPanel` | `Source/FUSE/Editor/include/fuse/editor/viewport_panel.hpp` | B6.3 — camera + resize stub |
| `GizmoSystem` | `Source/FUSE/Editor/include/fuse/editor/gizmo_system.hpp` | B6.4 — translate/rotate/scale drag stub; B6.4 deepen — ray hit tests, snap, delta helpers; B6.4 deepen follow-up — mode cycle, screen miss stub, scale snap |
| `HierarchyModel` / `SceneHierarchyPanel` | `hierarchy_model.hpp`, `scene_hierarchy_panel.hpp` | B6.5 — flatten, search, reparent via `UndoStack` |
| `EditorState` / `EditorScene` | `editor_state.hpp`, `editor_scene.hpp` | Selection + ECS registry for panel tests |
| `PropertyInspector` | `property_inspector.hpp` | B6.6 — component section model + live ECS edits |
| `MaterialEditorPanel` | `material_editor_panel.hpp` | B6.7 — catalog/selection + `MaterialEditState`; B6.7 deepen — property bindings + dirty flags |
| `MaterialPropertyBinding` | `material_property_binding.hpp` | B6.7 deepen follow-up — bind/get/set stubs, per-property dirty coalesce, panel refresh helpers |
| `SdfSculptPanel` | `sdf_sculpt_panel.hpp` | B6.8 — brush state + stroke spacing / symmetry |
| `AssetBrowser` | `asset_browser.hpp` | B6.9 — filesystem scan + type classification + search filter |
| `ProfilerPanel` | `profiler_panel.hpp` | B6.10 — 256-frame ring buffer of `FrameProfileData` |
| `ConsolePanel` | `console_panel.hpp` | B6.11 — log buffer, level/text filters, command exec stub |
| `PlayModeController` | `play_mode_controller.hpp` | B6.12 — scene snapshot / restore + `PlayModePhysicsState` flag |
| `PlaySession` / `PlayWorldSnapshot` | `play_session.hpp` | B6.12 deepen — PIE start/stop, tick accumulator, world snapshot capture/restore, coalesced dirty restore |
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

### B6.2 deepen — command stack + undo history

- **Push / undo / redo** — `CommandStack::push` aliases `execute`; empty-stack undo/redo are no-ops; new `push` after undo clears the redo branch; `peekUndo()` / `peekRedo()` expose menu labels.
- **Coalescing** — consecutive `SetProperty` commands with the same `target` + `propertyName` collapse into one undo step (slider/drag edits); `propertyValueBefore` preserves the pre-drag baseline; `coalescedCount()` tracks merges and round-trips in snapshots.
- **Dirty tracking** — `isDirty()` / `markClean()` / `dirtyRevision()` on both `CommandStack` and `UndoStack` mirror document-modified state for save prompts.
- **Snapshot restore** — `CommandStack::captureSnapshot()` / `restoreSnapshot()` copies undo/redo payloads for PIE checkpoints; `UndoStackSnapshot` captures depth + descriptions and `restoreSnapshot()` rewinds live undo/redo depth (command cloning still deferred).
- **MAX_HISTORY** — both `CommandStack` and `UndoStack` cap at 256 entries; oldest commands evicted on overflow (`evictedCount()` on both stacks).

### Headless panel stubs

`ViewportPanel`, `GizmoSystem`, and `SceneHierarchyPanel` are API-only stubs in `fuse_editor_api`. The optional Qt shell (`FUSE_BUILD_EDITOR`) keeps its existing placeholder widget.

### B6.9–B6.12 stubs

- **Asset browser** — `AssetBrowser::init(project_root)` scans with `std::filesystem`; entries classified by extension; `setSearchFilter` narrows results.
- **Profiler panel** — `ProfilerPanel::pushFrameData` writes a 256-frame ring buffer; `setPaused(true)` freezes capture.
- **Console panel** — `ConsolePanel::addLog` uses `fuse::log::Level`, coalesces duplicate lines, supports level/text filters via `filteredLines()`.
- **Play mode** — `PlayModeController` snapshots `fuse::scene::Scene` on `enterPlay` and restores on `stop`; `PlayModePhysicsState` tracks simulation-active flag until `PhysicsManager` wiring lands.
- **PIE play session** — `PlaySession` wraps the controller with `EditorState` sync, `tick(dt)` while playing (paused sessions skip ticks), `tickAccumulator()` dt sum, `PlayWorldSnapshot` ECS capture/restore on start/stop, coalesced `Transform::dirty` marking during play, and per-entity dirty + `sceneModified` snapshot/restore on stop.

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
| `fuse_editor_panels` | `fuse_editor_panels_tests` | B6.6–B6.8 inspector/material/sculpt API; B6.7 deepen property bindings + dirty flags; B6.7 deepen follow-up bind/get/set + dirty coalesce + unbound |
| `fuse_editor_command_stack` | `fuse_editor_command_stack_tests` | B6.2 `UndoStack` LIFO, merge, rename/reparent, `MAX_HISTORY`, 100-step chain, empty-stack no-ops, dirty stub, depth-rewind snapshot restore; `CommandStack` push/undo/redo, coalescing baseline, `peekUndo`/`peekRedo`, redo-branch clear, dirty tracking, snapshot restore, `evictedCount` |
| `fuse_editor_hierarchy_model` | `fuse_editor_hierarchy_model_tests` | B6.5 flatten, search, reparent + undo |
| `fuse_editor_panels_b69_b612` | `fuse_editor_panels_b69_b612_tests` | B6.9–B6.12 asset/profiler/console/play-mode stubs |
| `fuse_editor_phase6_integration` | `fuse_editor_phase6_integration_tests` | **B6.13** — `UndoStack` + `SceneHierarchyPanel` + `ViewportPanel` headless wiring |
| `fuse_editor_play_session` | `fuse_editor_play_session_tests` | B6.12 deepen — PIE start/stop cycle, tick accumulator, world snapshot roundtrip, empty world, coalesced dirty + clean restore on stop |
| `fuse_editor_gizmo_system` | `fuse_editor_gizmo_system_tests` | B6.4 deepen — mode switch, axis pick extremes, snap helpers, delta roundtrip, dirty flags; B6.4 deepen follow-up — hit-test miss, snap grid, `cycleMode` |

---

## Gates

### B6.2 — Command System & Undo/Redo

- [x] `UndoCommand` interface with `execute` / `undo` / `description`
- [x] `UndoStack` LIFO undo/redo with merge hook for consecutive commands
- [x] `SetObjectNameCommand` — rename with full undo restore
- [x] `ReparentObjectCommand` — scene-graph reparent with parent restore
- [x] `fuse_editor_command_stack` — LIFO, merge collapse, rename/reparent unit tests
- [x] `MAX_HISTORY` cap and oldest-command eviction (`UndoStack` + `CommandStack`)
- [x] 100-command undo chain stress test
- [x] `CommandStack` `push`/undo/redo with empty-stack no-ops and redo-branch clear on new push
- [x] `CommandStack` coalescing for consecutive `SetProperty` edits (`propertyValueBefore` baseline)
- [x] `CommandStack` dirty tracking + `captureSnapshot` / `restoreSnapshot` (full payload round-trip)
- [x] `CommandStack` `peekUndo` / `peekRedo` for menu labels
- [x] `UndoStackSnapshot` depth rewind restore + dirty stub (`isDirty` / `markClean` / `dirtyRevision`)
- [x] `evictedCount()` on `CommandStack` and `UndoStack`
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
- [x] `GizmoSpace` local/world + `applyTranslateDelta` / `applyRotateDelta` / `applyScaleDelta` helpers
- [x] Ray vs axis segment / plane hit-test stubs (`hitTestAxisSegment`, `hitTestAxisPlane`, `pickAxisFromRay`)
- [x] Translate grid snap + rotate angle snap stubs (`GizmoSnapSettings`, `snapTransform`)
- [x] Scale grid snap stub (`scaleSnap`, `snapScale`)
- [x] `cycleGizmoMode` / `GizmoSystem::cycleMode` for translate → rotate → scale cycling
- [x] Screen dead-zone hit-test miss stub (`isScreenHitMiss`) + segment/plane miss coverage
- [x] `CommandStack` + `EditorState::sceneModified` dirty marking on `endDrag`
- [x] `fuse_editor_gizmo_system` — mode switch, hit-test miss, snap grid, axis pick extremes, delta roundtrip
- [ ] Screen-space gizmo rendering + hover highlight (deferred)

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
- [x] Roughness/metallic/base-color/shading-model edits post `EditorCommand` via `CommandStack`
- [x] Material property commands bind `target` handle to material id for `CommandStack` coalescing
- [x] `previewDirty` + `editDirty` flags — sync/push clears; property edits set both
- [x] Optional RHI bridge — `syncFromMaterialSystem` / `pushToMaterialSystem` (`fuse_editor_panels` when `FUSE_BUILD_VULKAN=ON`)
- [ ] Live material preview render target (deferred)
- [ ] Save/load persistence cycle (deferred)

#### B6.7 deepen — property bindings + dirty flags

- **Property bindings** — `setRoughness`, `setMetallic`, `setBaseColor`, `setShadingModel` post `material.*` `SetProperty` envelopes with material-id `target` for slider coalescing.
- **Dirty flags** — `previewDirty` marks preview RT stale; `editDirty` tracks unsaved authoring edits until `pushToMaterialSystem` or `syncFromMaterialSystem`.
- **Tests** — `fuse_editor_panels` covers bindings, coalescing, dirty flags, and optional `MaterialSystem` round-trip when Vulkan is enabled.

#### B6.7 deepen follow-up — `MaterialPropertyBinding`

- **`MaterialPropertyBinding`** — `bind`/`unbind` to a `MaterialEditState`; per-property `get*`/`set*` stubs post the same `material.*` envelopes as the panel helpers.
- **Dirty coalesce** — repeat edits to the same property increment `coalescedDirtyCount()` while a single property dirty bit stays set until `markPanelRefreshed()`.
- **Panel refresh** — `needsPanelRefresh()` / `refreshPanel()` on `MaterialEditorPanel`; unbound get/set returns false without posting commands.
- **Tests** — `fuse_editor_panels` adds bind/get/set round-trip, dirty coalesce, and unbound coverage.

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
- [x] `PlaySession` PIE deepen — start/stop cycle, tick accumulator, `PlayWorldSnapshot` roundtrip, empty world, coalesced dirty + clean restore on stop (`fuse_editor_play_session`)
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
| Command system | 100-command undo chain | **Done** | `fuse_editor_command_stack` |
| Command system | `MAX_HISTORY` eviction | **Done** | `UndoStack` + `CommandStack` |
| Command system | `CommandStack` coalescing + dirty + snapshot | **Done** | `fuse_editor_command_stack` |
| Viewport | Headless camera + resize stub | **Done** | `ViewportPanel` |
| Viewport | Framebuffer rebuild on resize | **Deferred** | Needs Vulkan surface |
| Gizmos | Headless drag delta stub | **Done** | `GizmoSystem` |
| Gizmos | Ray hit tests + snap + delta helpers | **Done** | `fuse_editor_gizmo_system` |
| Gizmos | Screen-space axis rendering | **Deferred** | — |
| Hierarchy | Flatten + search + reparent via undo | **Done** | `fuse_editor_hierarchy_model` |
| Hierarchy | Drag-and-drop UI | **Deferred** | Qt U6 |
| Inspector | ECS component sections (Qt-free) | **Done** | `fuse_editor_panels` |
| Material | Catalog + edit state stub | **Done** | `fuse_editor_panels` |
| Material | Property bindings + dirty flags + `MaterialSystem` bridge | **Done** | `fuse_editor_panels` (B6.7 deepen) |
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
