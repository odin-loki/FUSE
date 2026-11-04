# Qt → FUSE UI editor parity

Status of the UI9 switch ([FUSE_EDITOR_QT_FIRST_PLAN.md](../plans/FUSE_EDITOR_QT_FIRST_PLAN.md) §6, §11): every
`fuse_editor_qt_*` gate and its `fuse_editor_ui_*` twin, then the feature areas the gates do not fully cover.
Qt is removed only after every twin is green, the desktop accessibility adapters are in, and the owner signs off.

Measured on native Windows 11 (MSVC Release, RTX 3090, Qt 6.11 msvc2022_64), 2026-11-05:
`ctest -L qt` and `ctest -L fuse_editor_ui` in one tree configured with `FUSE_EDITOR_SHELL=fuse`.
Linux (Xvfb / lavapipe) was not re-run for this pass. The Windows desktop was shared with other build agents, so
the Qt gates that sample real screen pixels or wall-clock budgets varied between runs (noted per row).

Totals: FUSE `ctest -L fuse_editor_ui` 68 / 68 pass (twins, FUSE-only gates and `fuse_editor_ui_a11y`). Qt `ctest -L qt`
(full run with `idle_frames`): 44 / 46 gates pass; `theme` and `live_present` fail as noted; `fuse_lint_b6_editor_qt6_only`
is not built in this tree.

Twins use the Qt gate's suffix: `fuse_editor_qt_<name>` → `fuse_editor_ui_<name>`.

## Gate matrix

| Qt gate | What it checks | FUSE twin | Qt (Windows) | FUSE (Windows) |
|---|---|---|---|---|
| startup | 13 docks built, viewport central, < 1 s | `startup` | pass (1.2 s against the 1 s budget once, while the machine built in parallel) | pass |
| layout | default layout, region sizes | `layout` | pass | pass |
| theme | P6 colour tokens | `theme` | flaky: hover samples need the real cursor over a frontmost window (failed 2 of 3 full runs on the shared desktop) | pass |
| font_1 / _1_25 / _1_5 / _2 | font metrics scale with DPR | `font_1` … `font_2` | pass | pass |
| wasd | fly camera response | `wasd` | pass | pass |
| context_menu / _dpr1_5 | placement incl. flips and DPR | `context_menu`, `context_menu_dpr1_5` | pass (one SegFault in one full run; passes alone) | pass |
| fuse_api | panels drive the editor APIs | `fuse_api` | pass | pass |
| project_open | open replaces the world | `project_open` | pass | pass |
| present_adopt | Qt adopts the FUSE VkInstance, real swapchain | `present_adopt` (runs the zero-copy `gpu_viewport` check: the presenter adopts the engine device) | pass | pass |
| live_present | live editor presents, resize, input, teardown | `live_present` | fail: 0 validation errors after the renderer fix; the on-screen pixel checks fail while other windows cover the gate window | pass |
| ui_frame | UI frame budget | `ui_frame` | pass (failed once under load) | pass |
| idle_frames | no spikes, memory cap | `idle_frames` | pass | pass |
| project_settings | settings dialog round-trip, import, cook on import | `project_settings` | pass | pass |
| edit_tools | select all, copy / paste, search, rename, light icons, frame, duplicate, colliders, recent, group, view modes, open script, navigation | `edit_tools` (+ `colliders`, `entity_group`, `view_modes`, `open_script`, `navigation`, `recent_projects` run as sub-gates) | pass | pass |
| terrain | terrain create / sculpt / undo | `terrain` | pass | pass |
| asset_browser | grid, filter, breadcrumb, context menu | `asset_browser`, `asset_browser_polish` | pass | pass |
| multi_viewport | ortho views | `multi_viewport` | pass | pass |
| play_camera | PIE keeps the camera; Play from Spawn; Stop restores | `play_camera` | pass | pass |
| lighting | sun / ambient / fog panel, undo, level environment | `lighting` | pass | pass |
| preferences | prefs dialog, shortcut conflicts, persistence | `preferences`, `shortcut_rebind` | pass | pass |
| build_package | package, cook, launchers, Shipping excludes, archive, dialog, e2e player run | `build_package` | pass | pass |
| icon | window + executable icon | `icon` | pass | pass |
| e15_console | console, Lua REPL, filters | `e15_console` | pass | pass |
| e15_transport | Play / Pause / Step / Resume / Stop | `e15_transport` | pass | pass |
| e20_inspector | typed editors, add / remove component | `e20_inspector` | pass | pass |
| e20_reparent | reparent keeps pose, undo | `e20_reparent` | pass | pass |
| e20_file_menu | New / Save As / Recent, dirty prompt | `e20_file_menu` | pass | pass |
| e20_wizard | New Project wizard | `e20_wizard` | pass | pass |
| shell_docks | every panel dock + central viewport | `shell_docks` | pass | pass |
| shell_toolbar | New / Open / Save / Undo / Redo with icons, enable state | `shell_toolbar` | pass | pass |
| shell_undo | inspector edit + toolbar Undo / Redo | `shell_undo` | pass | pass |
| shell_play | Play / Stop, banner, clock, Maximize on Play | `shell_play` (+ `play_frame`) | pass | pass |
| shell_assets | folder navigation, scene open | `shell_assets` | pass | pass |
| shell_entity | Entity menu create / duplicate / delete / undo | `shell_entity` | pass | pass |
| shell_gizmo | move / snap / rotate / local / scale, one undo per drag | `shell_gizmo` (+ `viewport_gizmo`) | pass | pass |
| shell_material | catalog, assign, edits, merged undo, rename, presets, usage, select users, shader / texture, sidecar | `shell_material` (+ `material_editor`) | pass | pass |
| shell_sculpt | sculpt stroke = one undo, Paint | `shell_sculpt` (+ `sdf_sculpt`) | pass | pass |
| shell_overlay | gizmo overlay over the Vulkan viewport, drag on it | `shell_overlay` (vector overlay in the UI draw list; no native child window needed) | pass | pass |
| shell_timeline | keys, retime, delete, cubic / tangents, curve strip, navigation, loop, sequencer link | `shell_timeline` (+ `timeline`, `timeline_nav`, `sequencer`) | pass | pass |
| shell_layout | hide / float docks, save, reset, restore | `shell_layout` | pass | pass |
| shell_profiler | summary, over-budget count, pause, clear | `shell_profiler` (+ `profiler`) | pass | pass |
| shell_project | sample project populates hierarchy, title, grab | `shell_project` | pass | pass |

Counts: 46 Qt gates, 46 twins with the same name (0 missing), plus FUSE-only gates (`view_modes`, `navigation`,
`recent_projects`, `entity_group`, `colliders`, `open_script`, `play_frame`, `scene_viewport`, `gpu_viewport`,
`project_hub`, `sequencer`, `timeline_nav`, `asset_browser_polish`, `fuse_editor_controller_api`,
`fuse_editor_panel_models`) and `fuse_editor_ui_a11y` (below).

### Accessibility (sign-off prerequisite)

| Check | Gate | Status |
|---|---|---|
| Editor window exposes menus, toolbar buttons, panel pages and inspector fields to UI Automation; a UIA client edits the Position field (ValuePattern), invokes Undo (InvokePattern) and opens the Entity menu (ExpandCollapse) | `fuse_editor_ui_a11y` (Windows, AccessKit) | pass |
| `fuse_editor_fuse` attaches `WindowAccessibility` to its window | — (app wiring) | done |
| Linux AT-SPI on the editor window | none yet (the AccessKit adapter has its own `fuse_ui` gates) | not gated |
| macOS NSAccessibility | none | not available |

## Feature coverage beyond the gates

| Area | Qt shell | FUSE shell | Status |
|---|---|---|---|
| Menus (File, Edit, Entity, Play, View) | QMenuBar | action table, same objectNames as stable ids | parity |
| File: Project Settings, Build / Package, Import Asset | dialogs | `projset.*`, `package.*` dialogs; import + cook | parity (import picks one file; Qt picks several) |
| Edit: Cut / Copy / Paste / Select All / Deselect All / Find | QAction | action table (text fields keep Ctrl+A/C/V/X) | parity |
| Entity: prefabs (save, instantiate, revert / apply overrides, variant, update, unlink) | QAction → prefab commands | same commands from the action table | parity, no gate in either shell |
| Shortcuts, rebinding, conflicts | QKeySequenceEdit | Preferences shortcut row | parity |
| DPI variants | QT_SCALE_FACTOR | font scale 1–2 | parity |
| Context menus | viewport, hierarchy, asset browser | viewport, asset browser, panel tab bar | gap: no hierarchy context menu (F2 rename and Entity menu instead) |
| Drag and drop | hierarchy reparent, dock drag | Parent to Active / Clear Parent buttons; panel move through the tab menu | gap: no DnD in the hierarchy or for docks |
| Docking | QDockWidget: hide, float (top-level window), drag to any area, tabify | per-panel hide, move between sides, float, saved / restored layout | gap: floating panels are in-window frames, not separate OS windows |
| Undo / redo surfaces | menu, toolbar, shortcuts | same | parity |
| Console | QPlainTextEdit + REPL | list + line edit | parity |
| Profiler | flame graph widget | chart (CPU history vs budget) | gap: no flame graph view |
| Sequencer | preview pane | scrub + link + preview text | parity with the Qt pane |
| Timeline | canvas with key drag and curve strip | ruler scrub, key buttons (select / retime / delete), interpolation and tangents, curve chart | gap: keys are retimed with buttons, not dragged |
| Material editor | form + preview | same fields + preview, rename, presets, usage | parity |
| Asset browser | list / grid, menu, rename | same | parity |
| Lighting panel | sun / ambient / fog | same | parity |
| Colour picking | QColorDialog | `DialogServices::pickColour` returns the initial colour | gap: no colour dialog in the FUSE shell |
| File dialogs | QFileDialog (native) | FUSE UI file dialog | gap: not the OS dialog (UI1 native dialogs exist but are not wired to the shell) |
| Viewport | embedded QVulkanWindow | zero-copy external texture | FUSE ahead (no surface hand-off) |
| Screen readers | Qt accessibility | AccessKit adapter (UIA gated) | Windows parity; Linux / macOS not gated |

## Blockers for removing Qt

1. Floating panels are in-window frames; Qt floats docks into their own top-level windows.
2. No drag-and-drop for hierarchy reparenting or dock placement (buttons / menus instead).
3. No hierarchy context menu, no colour dialog, no native OS file dialog in the FUSE shell.
4. Profiler has no flame graph; timeline keys cannot be dragged.
5. Accessibility is gated on Windows only; Linux (AT-SPI) and macOS need editor-level gates.
6. This pass ran natively on Windows only; the Linux Xvfb run of the new twins is pending.
7. The Qt reference gates `theme` (hover) and `live_present` (screen pixels) still need a dedicated, unobscured
   desktop on Windows; `live_present`'s swapchain semaphore reuse error was fixed in the renderer (one present
   semaphore per swapchain image).
8. Owner sign-off.
