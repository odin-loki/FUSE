# Qt → FUSE UI editor parity

> **State after the merge into main (2026-11-12).** This document was written on the `claude/magical-bassi-815825` branch and describes that branch's UI code. The merge kept main's UI architecture; the branch UI features it refers to are in the tree as unbuilt source until they are ported ([`docs/ui/archive/PORT_magical_bassi.md`](../ui/archive/PORT_magical_bassi.md)). `-DFUSE_EDITOR_SHELL=fuse` does not compile on main until the items marked "editor" in that list are ported, so the `fuse_editor_ui_*` results below are the branch's.

Status of the UI9 switch ([FUSE_EDITOR_QT_FIRST_PLAN.md](../plans/FUSE_EDITOR_QT_FIRST_PLAN.md) §6, §11): every
`fuse_editor_qt_*` gate and its `fuse_editor_ui_*` twin, then the feature areas the gates do not fully cover.
Qt is removed only after every twin is green, the desktop accessibility adapters are in, and the owner signs off.

Measured on native Windows 11 (MSVC Release, RTX 3090, Qt 6.11 msvc2022_64), 2026-11-05:
`ctest -L qt` and `ctest -L fuse_editor_ui` in one tree configured with `FUSE_EDITOR_SHELL=fuse`.
The Windows desktop was shared with other build agents, so the Qt gates that sample real screen pixels or wall-clock
budgets varied between runs (noted per row).

Totals (round 5, 2026-11-05, split docking + profiler GPU feed): `ctest -L fuse_editor_ui` 79 / 79 pass on Windows
(RTX 3090) and on Linux (WSL2, GCC 13, Xvfb + lavapipe; `ui_frame` needed a rerun once the shared machine was idle).
The new gate is `dock_split`.

Totals (blocker pass, 2026-11-05, after merging the round-4 UI tip):
- Windows: FUSE `ctest -L fuse_editor_ui` 78 / 78 pass (twins, FUSE-only gates, the blocker gates below and
  `fuse_editor_ui_a11y`).
- Linux (WSL2 Ubuntu, GCC 13, Xvfb + lavapipe, AccessKit built through rustup): `ctest -L fuse_editor_ui` 78 / 78 pass (the same set with `fuse_editor_ui_a11y_linux` in place of
  `fuse_editor_ui_a11y`; GPU gates on lavapipe; `float_window` / `native_dialogs` under their own Xvfb).
- Qt `ctest -L qt` (previous pass, full run with `idle_frames`): 44 / 46 gates pass; `theme` and `live_present` fail as
  noted; `fuse_lint_b6_editor_qt6_only` is not built in this tree.

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

The FUSE column also passes on Linux (Xvfb / lavapipe) for every row.

Counts: 46 Qt gates, 46 twins with the same name (0 missing), plus FUSE-only gates (`view_modes`, `navigation`,
`recent_projects`, `entity_group`, `colliders`, `open_script`, `play_frame`, `scene_viewport`, `gpu_viewport`,
`project_hub`, `sequencer`, `timeline_nav`, `asset_browser_polish`, `fuse_editor_controller_api`,
`fuse_editor_panel_models`), the blocker gates (below) and `fuse_editor_ui_a11y` / `fuse_editor_ui_a11y_linux`.

### Blocker gates (label `fuse_editor_ui`)

| Gate | What it checks | Windows | Linux (Xvfb) |
|---|---|---|---|
| `float_window` | a floated panel is its own OS window (owned borderless tool window: Win32 `WS_EX_TOOLWINDOW`, X11 utility window), out of the main draw list, placed at origin + frame rect, may leave the main window; posted OS clicks / drags reach it (title drag moves it, keys type into its fields through the main window); dragged over the left dock it shows the drop zones and docks; a tab dragged out of the main window becomes a tool window; its Dock button docks it | pass | pass |
| `dock_split` | split docking: `splitPanel` puts a panel in a new tab stack under another (column split on the left); a tab dragged onto the left edge zone of the right stack shows the zone (`dropEdge`, half-stack preview "Split Left") and splits it on release; dragged onto a stack's centre it tabifies ("Add as Tab"); a nested column split inside the row split; a splitter handle between stacks resizes by pointer; save / reset / restore gives the same stacks, tabs, layout text and stack rects (±1.5 px); stacks whose panels leave go and their splits collapse; tab menu Split Down; Reset Layout returns to one stack per side | pass | pass |
| `dock_drag` | tab drag-out floats under the pointer; drop zones (`dock.dropzone.left/right/bottom`, hot zone highlighted) dock on release; release elsewhere stays floating; Ctrl suppresses docking; Escape cancels; resize grip; float size in the saved layout; drop strip along an emptied side | pass | pass |
| `hierarchy_dnd` | row drag onto a row reparents (drop mark, status text), undo / redo, cycles refused, drop below the rows = top level, the multi-selection moves together, Escape cancels, a plain click only selects | pass | pass |
| `hierarchy_menu` | right click / Shift+F10 menu: Rename, Create Empty / Cube / Point Light Child (one undo step), Duplicate, Delete (+ undo), Save Selection as Prefab, Revert / Apply / Unlink (disabled when not a prefab), Clear Parent (disabled at top level); empty space: create items | pass | pass |
| `color_dialog` | FUSE UI colour dialog (colorPicker, hex field, old / new swatches): OK applies to the sun through the Lighting panel, Cancel / Escape keep it, alpha comes back | pass | pass |
| `native_dialogs` | Windows: Save As / Open through IFileSaveDialog / IFileOpenDialog owned by the editor window, cancel, folder picker, ChooseColor (opt-in) end to end. Linux without zenity: the FUSE UI file / colour dialogs take over | pass | pass |
| `tab_order` | Tab walks menu bar, toolbar, left dock, viewport bar / viewport, right dock, bottom dock in reading order, each region once, no hidden stops (dock splitters are stops between regions); Shift+Tab walks back; a dock's tab bar is a stop and Right switches its panel | pass | pass |
| `profiler_flame` | Profiler Chart / Flame Graph switch; nested scopes, click info, Pause / Clear | pass | pass |
| `timeline_drag` | key drag retime live, snapped to the Snap rate (Shift: free), one "Move key" undo step, Escape cancels, no-op drags record nothing; Ctrl+Right on the selected key retimes it by a frame (one step) | pass | pass |

Tab order (checked by `tab_order`, after the keyboard-only work made dock titles and scroll bars stops): menu bar
(File … View) → toolbar → left dock (dock title, tab bar, hierarchy filter / buttons / tree) → splitter → viewport
tools and viewport → splitter → right dock (title, tab bar, inspector) → splitter → bottom dock (title, tab bar,
console). Scroll bars only become stops when their area overflows; a floated panel's widgets follow in its frame.

### Accessibility (sign-off prerequisite)

| Check | Gate | Status |
|---|---|---|
| Editor window exposes menus, toolbar buttons, panel pages and inspector fields to UI Automation; a UIA client edits the Position field (ValuePattern), invokes Undo (InvokePattern) and opens the Entity menu (ExpandCollapse) | `fuse_editor_ui_a11y` (Windows, AccessKit) | pass |
| `fuse_editor_fuse` attaches `WindowAccessibility` to its window | — (app wiring) | done |
| Linux AT-SPI on the editor window: menus, toolbar buttons and panels found; the Position entry read through AT-SPI and edited (focus by AT-SPI, typed on the UI thread: AccessKit's Linux adapter has no text write); Undo clicked; Entity menu opened and closed | `fuse_editor_ui_a11y_linux` (dbus-run-session + Xvfb, AccessKit) | pass |
| macOS NSAccessibility | none | not available |

## Feature coverage beyond the gates

| Area | Qt shell | FUSE shell | Status |
|---|---|---|---|
| Menus (File, Edit, Entity, Play, View) | QMenuBar | action table, same objectNames as stable ids | parity |
| File: Project Settings, Build / Package, Import Asset | dialogs | `projset.*`, `package.*` dialogs; import + cook | parity (import picks one file; Qt picks several) |
| Edit: Cut / Copy / Paste / Select All / Deselect All / Find | QAction | action table (text fields keep Ctrl+A/C/V/X) | parity |
| Entity: prefabs (save, instantiate, revert / apply overrides, variant, update, unlink) | QAction → prefab commands | same commands from the action table and the hierarchy context menu | parity (`hierarchy_menu` saves one) |
| Shortcuts, rebinding, conflicts | QKeySequenceEdit | Preferences shortcut row | parity |
| DPI variants | QT_SCALE_FACTOR | font scale 1–2 | parity |
| Context menus | viewport, hierarchy, asset browser | viewport, hierarchy (rename, create child, duplicate, delete, prefab, clear parent; Shift+F10), asset browser, panel tab bar | parity |
| Drag and drop | hierarchy reparent, dock drag | hierarchy row drag (selection, top level, undo); tab drag-out and floating title drag onto drop zones | parity |
| Docking | QDockWidget: hide, float (top-level window), drag to any area, tabify, split a side (`splitDockWidget`) | per-panel hide, move, float as an owned OS tool window (headless: in-window frame), Dock / Close, resize grip, drag back to dock; several tab stacks per side in nested splitters: drag onto a stack's edge zone splits it, onto its centre tabifies (preview `dock.dropzone.split`), tab menu Split Right / Down; saved / restored layout incl. splits and float size | parity (`dock_split`) |
| Undo / redo surfaces | menu, toolbar, shortcuts | same | parity |
| Console | QPlainTextEdit + REPL | list + line edit | parity |
| Profiler | flame graph widget | chart (CPU history vs budget, GPU ms series) and flame graph (Chart / Flame switch, merged call paths, click info, `gpu.viewport` root with one bar per render-graph pass); GPU time from the scene viewport's GpuProfiler, passes bucketed into G-buffer / depth / shadows / DDGI / SDF / deferred | parity; FUSE ahead on GPU passes (`profiler`, `gpu_viewport` need non-zero GPU history on the RTX 3090 and lavapipe) |
| Sequencer | preview pane | scrub + link + preview text | parity with the Qt pane |
| Timeline | canvas with key drag and curve strip | ruler scrub, key drag (snap, Shift free, Escape, one undo step), Ctrl+Left / Right retime, key buttons, interpolation and tangents, curve chart | parity |
| Material editor | form + preview | same fields + preview, rename, presets, usage | parity |
| Asset browser | list / grid, menu, rename | same | parity |
| Lighting panel | sun / ambient / fog | same | parity |
| Colour picking | QColorDialog | modal FUSE UI colour dialog (colorPicker, hex, old / new); ChooseColor / zenity with `DialogOptions::nativeColorDialog` | parity |
| File dialogs | QFileDialog (native) | OS dialogs (IFileDialog / zenity) owned by the editor window; FUSE UI file dialog when headless or without one | parity |
| Popup menus | QMenu top-level windows | OS-window popups (`NativeOptions::osPopups`, on in `fuse_editor_fuse`) | parity |
| Viewport | embedded QVulkanWindow | zero-copy external texture | FUSE ahead (no surface hand-off) |
| Screen readers | Qt accessibility | AccessKit adapter (UIA and AT-SPI gated) | Windows and Linux parity; macOS not gated |

## Blockers for removing Qt

Resolved in the blocker pass (gates above): floating panels as OS windows (1), hierarchy and dock drag and drop (2),
hierarchy context menu, colour dialog and native file dialogs (3), profiler flame graph and timeline key drag (4),
Linux AT-SPI editor gate (5), Linux run of every twin (6).

Resolved in round 5: split docking (several tab stacks per side, nested splitters, edge-zone split and centre-zone
tabify by drag, saved layouts; gate `dock_split`).

Remaining:
1. macOS: no native window backend for the FUSE shell, so no NSAccessibility gate.
2. The Qt reference gates `theme` (hover) and `live_present` (screen pixels) still need a dedicated, unobscured
   desktop on Windows.
3. Owner sign-off.
