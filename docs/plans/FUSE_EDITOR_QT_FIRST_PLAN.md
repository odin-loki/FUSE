# FUSE Editor — Qt First, FUSE UI Later

Author: Odin Loch
Status: Draft — adopts decision D5 of [`FUSE_UI_PLAN.md`](FUSE_UI_PLAN.md): the editor stays on Qt 6 until FUSE UI can carry it
Scope: How the Qt editor keeps growing now without making the later move to FUSE UI more expensive, what has to move out of the Qt shell first, the parity checklist for the switch, and the part of the UI plan the switch actually depends on.

Related: [`../editor.md`](../editor.md), [`../architecture.md`](../architecture.md) (threading table), [`FUSE_MASTER_PLAN.md`](FUSE_MASTER_PLAN.md) §A3 and B6, [`FUSE_UI_PLAN.md`](FUSE_UI_PLAN.md) UI9.

---

## 0. Summary

The order is: **build the editor in Qt → get FUSE (engine + FUSE UI) working → redo the editor chrome in FUSE UI.**

That order only pays off if the Qt layer stays a thin shell. Anything the Qt code decides, reads from the scene or remembers on its own has to be rewritten in the port; anything in the Qt-free `fuse_editor_api` carries over unchanged.

The code is already most of the way there: `fuse_editor_api` holds ~13k lines of `.cpp`, the Qt shell is ~4.7k lines (sources and headers), and most docks already have a Qt-free panel model. The remaining porting cost sits in a handful of places — mainly a shared scene mutex that the Qt UI thread uses to call into the engine directly, an undo journal and file / project flows that live in `MainWindow`, hierarchy logic that duplicates `HierarchyModel`, and camera rules inside the viewport widget. Section 4 lists them; Section 5 turns them into work items Q1–Q12 that can land now, on Qt, with the existing gates staying green.

---

## 1. The sequence

| Stage | Editor | Engine / FUSE UI | Exit |
|---|---|---|---|
| **1. Now** | Qt shell keeps growing (B6 panels, gizmos, tools) under the rules in Section 3; extraction items Q1–Q12 land | Engine work continues; FUSE UI starts (UI0–UI2) | Shell rules enforced by lint; Q1–Q9 done |
| **2. FUSE UI matures** | Unchanged behaviour; new panels follow the rules | UI plan phases on the editor critical path (Section 7) | Every surface in Section 6 has the FUSE UI pieces it needs |
| **3. Switch** | `FUSE_EDITOR_SHELL=fuse` built beside the Qt shell; panels ported; twin gates green | — | Sign-off; Qt removed (Section 8) |

The Qt shell is the daily driver until Stage 3 finishes. Nothing in Stage 1 changes what the editor does — only where the code lives.

---

## 2. What is already right

| Fact | Where |
|---|---|
| Qt-free boundary library with the editor's real logic: command queue, undo stacks, scene files, PIE, gizmos, inspector schema, console, profiler, material and SDF panels, context menu, viewport embedding | `Source/FUSE/Editor/src/*.cpp` → `fuse_editor_api` (~13k lines; one Qt file, see L10) |
| Qt shell is a separate static library; tests drive the same code the binary runs | `fuse_editor_qt` (~4.7k lines in `src/qt/`) |
| Engine libraries cannot include Qt | `fuse_lint_c_no_qt_in_core` |
| Editor link closure is Qt-only (no Dear ImGui) | `fuse_lint_b6_editor_qt6_only` |
| Property edits, add / remove component, reparent, console, PIE transport go through `EditorHost::postFromUi` → `CommandQueue` → `gameTick()` | 12 post sites in `src/qt/` |
| Most docks already have a Qt-free model | `ConsolePanel`, `ProfilerPanel`, `MaterialEditorPanel`, `SdfSculptPanel`, `AssetBrowser`, `PropertyInspector`, `FeaturePaneBridge`, `EntityContextMenu`, `ViewportPanel`, `ViewportSceneView` |
| Small UI rules already live in the API | `panel_mask.hpp`, `panel_tab.hpp`, `dock_side.hpp`, `dock_slot.hpp`, `ui_scale.hpp`, `idle_frame.hpp`, `undo_shortcut.hpp` |
| 23 Qt gates describe the behaviour to match | Section 6 |

---

## 3. Rules for Qt code from now on

These apply to every change under `Source/FUSE/Editor/src/qt/`. R1, R2 and R5 get lint gates (Q11).

| # | Rule | Why |
|---|---|---|
| R1 | The shell includes only `fuse/editor/*`, `fuse/types.hpp` and `fuse/platform/thread.hpp`. No `fuse/ecs/*`, `fuse/project/*`, `fuse/cinematics/*` | If the shell knows engine types, the port has to re-learn them |
| R2 | The shell never takes the scene mutex and never calls `editorScene()`, `registry()`, `undoStack()` or `commandStack()` | One place owns threading (the controller, Section 3.1) |
| R3 | A slot translates a Qt event into one controller call. Any `if` about engine state belongs in the API | Decisions in slots are rewritten in the port |
| R4 | Every menu item, toolbar button and shortcut is an entry in an API action table (stable id, label key, default shortcut, enabled / checked query). The shell builds menus from the table | Both shells build identical menus from the same data |
| R5 | Each dock renders a Qt-free panel model; the widget holds no state the model does not have | The FUSE UI panel renders the same model |
| R6 | Persistent UI state is stored in FUSE formats, not Qt ones: dock layout and settings as JSON through `fuse::io`, theme as token JSON, shortcuts in the `ActionMap` file | `QSettings` and `QMainWindow::saveState()` blobs cannot be read by FUSE UI |
| R7 | Dialogs (`QFileDialog`, `QColorDialog`, `QMessageBox`, `QWizard`) are reached only through a `ShellServices` interface the controller calls; the flow (e.g. "unsaved changes → Save / Discard / Cancel → then open") lives in the API | Flows port for free; only the dialog implementation changes |
| R8 | New gates are written as a shell-neutral scenario plus a thin Qt driver (Q10) | The twin FUSE UI gate reuses the scenario |
| R9 | New user-facing strings go through string-table keys (optional until Q12) | `tr()` strings would need re-extraction |

### 3.1 The seam: `EditorController`

One Qt-free facade in `fuse_editor_api` that both shells call. It owns the scene mutex (moved out of `MainWindow` / `GameLoopThread`), exposes every user intent as a method, and publishes what panels display as plain structs.

```cpp
namespace fuse::editor {

/// Dialogs and prompts the shell implements (Qt today, FUSE UI later).
class ShellServices {
public:
    virtual ~ShellServices() = default;
    virtual std::optional<std::string> pickOpenFile(std::string_view title, std::string_view filter,
                                                    std::string_view directory) = 0;
    virtual std::optional<std::string> pickSaveFile(std::string_view title, std::string_view filter,
                                                    std::string_view directory) = 0;
    virtual std::optional<Rgba> pickColour(Rgba initial) = 0;
    virtual PromptAnswer askSaveDiscardCancel(std::string_view text) = 0;
    virtual void warn(std::string_view title, std::string_view text) = 0;
};

/// Everything a shell may do. The shell never sees the scene, the registry or the mutex.
class EditorController {
public:
    EditorController(EditorHost& host, ShellServices& services);

    // Files and projects (flows, prompts and recent files live here)
    void fileNew();  void fileOpen();  void fileOpenRecent(u32 index);
    void fileSave(); void fileSaveAs(); bool requestClose();
    void newProject(const NewProjectRequest& request);   // dimensions + modules
    // Edit
    void undo(); void redo();                              // one history across both stacks
    // Selection and hierarchy
    void select(std::span<const EntityRef> entities, EntityRef primary);
    void reparent(std::span<const EntityRef> entities, EntityRef newParent);
    void setHierarchyFilter(std::string_view text);
    // Viewport input (normalised; no Qt types)
    void viewportKey(platform::Key key, bool down);
    void viewportPointer(const ViewportPointerEvent& event);   // pick, look, context menu
    void viewportWheel(f32 notches);
    // Actions (menus, toolbar, shortcuts)
    void trigger(ActionId id);
    // What panels show — copied out, never references into the scene
    const EditorViewState& viewState() const;
    u64 viewStateVersion() const;
};

} // namespace fuse::editor
```

Q1 introduces it with the mutex inside (behaviour unchanged). Q9 then replaces the mutex reads with snapshots published by the game tick, so the UI thread never waits for a tick.

---

## 4. Where the Qt shell carries logic today

Measured on `main` at `e070dfa9`. Line references are to `Source/FUSE/Editor/src/qt/`.

| # | What | Where | Porting cost | Fix |
|---|---|---|---|---|
| L1 | **Two threading paths.** `editor.md` documents "UI posts `EditorCommand`s, game thread drains". In practice the UI thread also takes a shared `std::mutex` (29 sites) and calls `EditorHost` directly, while `GameLoopThread` holds the same mutex for the whole `gameTick()` | `main_window.cpp` (16), `editor_panels.cpp` (10), `viewport_placeholder_widget.cpp` (3), `game_loop_thread.cpp` | Every site is engine access the port must redo; the UI thread can block for a full tick | Q1 (one owner), Q9 (snapshots) |
| L2 | **Mutations on the UI thread under the mutex:** selection writes to `EditorState`, viewport picking, entity context-menu actions, new / open / save scene, new project, undo / redo, material roughness / metallic | `editor_panels.cpp` (`onItemSelectionChanged`, context-menu `activate`, `MaterialEditorWidget`), `viewport_placeholder_widget.cpp` (`pickAndSelect`, `activate`), `main_window.cpp` | Same as L1; also contradicts the documented rule | Q1, then commands in Q9 |
| L3 | **Unified undo journal** that interleaves the scene `UndoStack` and the property `CommandStack` so Edit > Undo follows edit order | `main_window.cpp` (`m_undoJournal`, `m_redoJournal`, `syncUndoJournal_`, 18 references) | Core editor behaviour living in a window class | Q2 |
| L4 | **File and project flows:** unsaved-changes prompt sequencing, recent-files list in `QSettings`, title / dirty text, default directories, and `createProject` writing module toggles into `project.json` then reopening | `main_window.cpp` (`maybeSaveChanges`, `createProject`, `addRecentFile`) | Flow logic plus Qt-only storage | Q3 |
| L5 | **Hierarchy logic duplicated:** row building from `ecs::Registry`, entity kind labels ("Mesh", "Point Light"…), filter, and the "keep a moved child under its moved ancestor" reparent rule — while `HierarchyModel` already exists in the API (over `Object` / `scene::Scene`) | `editor_panels.cpp` (`HierarchyWidget::refresh`, `entityLabel`, `reparentEntities`) | A second hierarchy implementation to rewrite | Q4 |
| L6 | **Viewport camera rules in the widget:** wheel = fly-speed ×1.2 per notch while looking, otherwise dolly by `moveSpeed × 0.25`; Qt key mapping; look state | `viewport_placeholder_widget.cpp` (`wheelEvent`, `setKey`) | Camera feel must be re-derived | Q5 |
| L7 | **Menus defined in code:** File / Edit / View / Play, shortcuts (`Ctrl+P` play, `F10` step), transport toolbar | `main_window.cpp` | Re-typed in the port | Q6 |
| L8 | **Engine types in the shell:** `ecs::Transform`, `SDFObject`, `Mesh`, lights, `Camera`, `project::ProjectManifest`, `cinematics::TimelineMs`, `cinematics/timeline_loader.hpp` | `editor_panels.cpp`, `property_pane_widget.cpp`, `main_window.cpp`, `viewport_placeholder_widget.cpp`, `seq_preview_pane_widget.*` | Shell coupled to engine headers (R1) | Q1–Q5, Q7 |
| L9 | **Qt-only persistent state:** dock layout via `QMainWindow::saveState()`, recent files via `QSettings`, theme colours hard-coded in C++ | `main_window.cpp`, `editor_theme.cpp` | Users lose layout and settings at the switch | Q8 |
| L10 | **Qt inside the "Qt-free" library:** `viewport_vulkan_surface_qt.cpp` is compiled into `fuse_editor_api` (privately linking `Qt6::Gui`) when Qt is found | `Editor/CMakeLists.txt`, `src/viewport_vulkan_surface_qt.cpp` | The boundary library is not actually Qt-free | Q11 (move it to `fuse_editor_qt`) |

---

## 5. Extraction work (Stage 1, on Qt)

Each item keeps all existing `fuse_editor_qt_*` gates green and adds a headless gate in `fuse_editor_api` tests, so the behaviour is pinned down independently of any toolkit. Sizes are relative (S < M < L).

| Item | Size | Content | New gates |
|---|---|---|---|
| **Q1 — `EditorController` + `ShellServices`** | M | Introduce the facade (Section 3.1). Move the scene mutex and `GameLoopThread`'s tick lock behind it. Route every L1 / L2 site through controller methods. No behaviour change | `fuse_editor_controller_api` (headless); all Qt gates |
| **Q2 — One undo history** | S | Move the journal into the API: `EditorHost::undo()`, `redo()`, `undoLabel()`, `redoLabel()` interleave both stacks by edit order | `fuse_editor_undo_unified` |
| **Q3 — File and project flows** | M | New / open / recent / save / save-as / close / new project (with modules) as controller flows using `ShellServices` for prompts; recent files and last directories in a JSON settings file | `fuse_editor_file_flow` (scripted `ShellServices`); `fuse_editor_qt_e20_file_menu`, `_e20_wizard` |
| **Q4 — Hierarchy view model** | M | Extend `HierarchyModel` (or add an ECS-backed `HierarchyView`) with labels, filter, reparent rule, selection sync; `HierarchyWidget` renders its rows | `fuse_editor_hierarchy_view`; `fuse_editor_qt_e20_reparent` |
| **Q5 — Viewport input in the API** | S | `ViewportPanel::onKey / onPointer / onWheel` with normalised input (`platform::Key`, buttons, wheel notches, DPR-scaled positions); camera rules move with them; Qt maps its events only | `fuse_editor_viewport_input`; `fuse_editor_qt_wasd`, `_context_menu` |
| **Q6 — Action table** | S | `EditorActions`: id, label key, default shortcut, enabled / checked queries, transport state. Menus, toolbar and shortcuts generated from it; shortcuts bindable through `ActionMap` | `fuse_editor_actions`; `fuse_editor_qt_fuse_api`, `_e15_transport` |
| **Q7 — Inspector value formatting** | S | Value ↔ text conversion and editor-kind selection per `PropertyFieldDesc` in the API; the widget only instantiates the matching control | Extends `fuse_editor_qt_e20_inspector` |
| **Q8 — Toolkit-neutral state** | M | `DockLayout` model (panel id, side, tab group, size, floating rect, visible) saved as JSON; theme tokens JSON generated from the P6 colours in `editor_theme.cpp`; Qt applies both. One-time import of the existing `QSettings` recent-files list | `fuse_editor_dock_layout_roundtrip`, `fuse_editor_theme_tokens`; `fuse_editor_qt_layout`, `_theme` |
| **Q9 — Snapshot reads** | L | The game tick publishes `EditorViewState` (hierarchy rows, inspector sections and values, title / dirty, transport, undo labels, console lines, profiler frames) double-buffered with a version counter. UI reads never lock; writes become commands. The mutex is deleted | `fuse_editor_view_state` (+ TSan nightly); `fuse_editor_qt_idle_frames`, `_ui_frame`; UI-thread max block time reported |
| **Q10 — Shell-neutral gate scenarios** | M | Split each Qt gate into a scenario (steps and assertions in terms of action ids, panel ids, row labels, field names) and a `ShellDriver` with a Qt implementation (QTest). FUSE UI later supplies a driver on `UiTestDriver` | Existing gates re-expressed; no new behaviour |
| **Q11 — Boundary lints and cleanup** | S | Move `viewport_vulkan_surface_qt.cpp` into `fuse_editor_qt` (L10). New lint `editor-shell`: R1 includes, R2 forbidden calls / mutex in `src/qt/`. Update `docs/editor.md` and the Qt row of the threading table in `architecture.md` to describe the controller | `fuse_lint_editor_shell` |
| **Q12 — String keys** (optional) | S | Editor strings as `StringTable` keys; Qt converts at the edge | `fuse_editor_strings_complete` |

Suggested order: Q11 lint (warn-only) → Q1 → Q2 → Q6 → Q3 → Q5 → Q4 → Q7 → Q8 → Q10 → Q9 → lint to enforcing → Q12.

---

## 6. Parity checklist for the switch

Every row must work in the FUSE UI shell, with its twin gate green, before Qt is removed. "UI plan" names the FUSE UI tier from [`FUSE_UI_PLAN.md`](FUSE_UI_PLAN.md) §5.

### Surfaces

| Surface | Qt today | API model | FUSE UI pieces (UI plan) |
|---|---|---|---|
| Main window: central viewport plus 9 docks (Hierarchy, Projects, Asset Browser, Inspector, Material Editor, SDF Sculpt, Console, Profiler, Sequencer); save / restore / reset layout | `QMainWindow`, `QDockWidget` | `DockLayout` (Q8), `panel_mask` | Tier B: MainWindow, dock system |
| File menu: New Scene, Open Scene, Open Recent, Save, Save As, New Project, Quit | `QMenuBar`, `QAction` | Action table (Q6), file flows (Q3) | Tier B: MenuBar, Menu |
| Edit menu: Undo, Redo | `QAction` | Unified history (Q2) | Tier B |
| View menu: per-dock toggles, Reset Layout | `toggleViewAction()` | `DockLayout` | Tier B |
| Play menu + transport toolbar: Play, Pause, Resume, Step, Stop | `QToolBar` | `PlayModeController`, action table | Tier B: ToolBar |
| Status bar | `QStatusBar` | View state | Tier B |
| Project hub and New Project wizard (location, name, dimensions, modules) | `QWizard`, `QListWidget` | Q3 `NewProjectRequest` | Tier B: Wizard; Tier C: ListView |
| Hierarchy: search, multi-select, drag-and-drop reparent, context menu | `QTreeWidget`, `QMenu` | `HierarchyModel` (Q4), `EntityContextMenu` | Tier C: TreeView; Tier B: ContextMenu |
| Inspector: typed editors (float, vec3, Euler, colour, enum, bool, asset path), Add Component, Remove | `QDoubleSpinBox`, `QCheckBox`, `QComboBox`, `QLineEdit`, `QToolButton`, `QColorDialog`, `QFileDialog` | `PropertyInspector`, component schema (`PropertyFieldDesc`, `ComponentKindDesc`), Q7 | Tier D: PropertyGrid, ColorPicker; Tier A controls |
| Console: log view with level colours, filters, repeat counts, history, Lua REPL | `QPlainTextEdit`, `QLineEdit` | `ConsolePanel` | Tier D: TextEdit (read-only, virtualised); Tier A: LineEdit |
| Asset browser | `QListWidget` | `AssetBrowser` | Tier C: GridView / ListView |
| Profiler with flame graph | `QWidget` paint | `ProfilerPanel`, `ProfilerFlameGraph` | Tier D: Chart / Painter |
| Material editor (roughness, metallic) | `QFormLayout`, spin boxes | `MaterialEditorPanel` | Tier A |
| SDF sculpt (brush op, radius, GRIA α) | `QComboBox`, spin boxes | `SdfSculptPanel` | Tier A |
| Sequencer preview | `SeqPreviewPaneWidget` | `ViewportSeqPreviewSample` (stub), cinematics `.seq` import | Tier D: Timeline editor |
| Viewport: embedded Vulkan present, fly camera (RMB + WASDQE, wheel), picking, context menu, resize → swapchain recreate | `QVulkanInstance` + `QWindow` container + 5-step handshake | `RuntimeViewportHook`, `ViewportPanel`, Q5 | UI4 embedded render-graph texture (zero-copy; the handshake goes away) |
| Unsaved-changes prompt, warnings | `QMessageBox` | Q3 flows | Tier B: MessageBox |
| File dialogs | `QFileDialog` | `ShellServices` | UI1 native dialogs (`vendor/nativeFileDialogs`) |
| Theme (P6 dark spec colours) | `QPalette`, `QProxyStyle` | Theme tokens (Q8) | UI2 style engine + theme |
| Bundled font at 1×, 1.25×, 1.5×, 2× | Qt resource + `QFont` | — | UI3 text, DPI |
| App icon | Qt resource + `.rc` | `Branding/` | UI5 application shell |

### Gates (23 today → twins)

| Qt gate | Checks | Twin |
|---|---|---|
| `fuse_editor_qt_startup` | All panels and docks initialise in < 1 s | `fuse_editor_ui_startup` |
| `fuse_editor_qt_layout` | Default layout: viewport, hierarchy, inspector, console visible | `fuse_editor_ui_layout` |
| `fuse_editor_qt_theme` | Every P6 spec colour applied | `fuse_editor_ui_theme` |
| `fuse_editor_qt_font_1`, `_font_1_25`, `_font_1_5`, `_font_2` | Bundled font, metrics scale with DPR, no missing glyphs | `fuse_editor_ui_font_*` |
| `fuse_editor_qt_wasd` | Same-frame key response, constant motion, no jitter | `fuse_editor_ui_wasd` |
| `fuse_editor_qt_context_menu`, `_context_menu_dpr1_5` | Menu geometry matches `EntityContextMenu::placement()` incl. edge flips and DPR | `fuse_editor_ui_context_menu*` |
| `fuse_editor_qt_fuse_api` | Panels drive the editor APIs (undo, selection, inspector, console, PIE) | `fuse_editor_ui_fuse_api` |
| `fuse_editor_qt_project_open` | Open Project loads the default world; a second open replaces it | `fuse_editor_ui_project_open` |
| `fuse_editor_qt_present_adopt` | Qt adopts the FUSE `VkInstance`; real swapchain; 0 validation errors | Replaced by `fuse_editor_ui_viewport_zero_copy` (no adoption needed) |
| `fuse_editor_qt_live_present` | Live editor presents the viewport; resize; forwarded input; clean teardown | `fuse_editor_ui_live_present` |
| `fuse_editor_qt_ui_frame` | UI render < 2 ms / frame | `fuse_editor_ui_frame` — target: full 9-dock repaint ≤ 2 ms (Qt measured 4.1–4.4 ms) |
| `fuse_editor_qt_idle_frames` | No spikes over 10,000 idle frames; memory < 256 MiB | `fuse_editor_ui_idle_frames` (+ zero frames when idle in app mode) |
| `fuse_editor_qt_icon` | Window and executable icon | `fuse_editor_ui_icon` |
| `fuse_editor_qt_e15_console` | Console → Lua REPL → output; filters, counts, history | `fuse_editor_ui_e15_console` |
| `fuse_editor_qt_e15_transport` | Play / Pause / Step / Resume / Stop; enable state follows `EditorState` | `fuse_editor_ui_e15_transport` |
| `fuse_editor_qt_e20_inspector` | Typed editors, one undo step per drag, add / remove component | `fuse_editor_ui_e20_inspector` |
| `fuse_editor_qt_e20_reparent` | Drag-and-drop reparent keeps world pose; undo restores | `fuse_editor_ui_e20_reparent` |
| `fuse_editor_qt_e20_file_menu` | New → edit → Save As → Open Recent; dirty title; prompt paths | `fuse_editor_ui_e20_file_menu` |
| `fuse_editor_qt_e20_wizard` | Wizard writes `project.json`; project loads | `fuse_editor_ui_e20_wizard` |

With Q10 done, each twin is the same scenario run through the FUSE UI `ShellDriver`.

---

## 7. What "FUSE working" has to include before the switch

The editor is a desktop application, so the switch does **not** wait for mobile, game UI or the designer. Its critical path through [`FUSE_UI_PLAN.md`](FUSE_UI_PLAN.md):

| UI plan phase | Needed for the editor switch | Not needed for the switch |
|---|---|---|
| UI0 | All | — |
| UI1 Platform services | Desktop only (Windows, Linux, macOS): text input and IME, wheel, clipboard, cursors, multiple windows and popups, DPI change, native file dialogs | Android / iOS surfaces and lifecycle |
| UI2 Core | All | — |
| UI3 Text | All (the console, inspector and future script editor need real text editing) | — |
| UI4 GPU renderer | App path, damage tracking, embedded render-graph texture (the viewport) | World-space panels, UI-T0 |
| UI5 Application shell | Desktop: on-demand loop, multiple windows, settings | Mobile lifecycle |
| UI6 Widgets | Tiers A, B, C; from Tier D: PropertyGrid, ColorPicker, Chart / Painter, read-only virtualised TextEdit, Timeline (for the sequencer) | Calendar, Video, Markdown view, NodeGraph (until a panel needs it) |
| UI6 Accessibility | Adapters for Windows, macOS, Linux **before Qt is deleted** — Qt provides screen-reader support today, so removing it without adapters is a regression | Mobile adapters |
| UI7 Game UI | — | All |
| UI8 Declarative / designer | Runtime UI inspector is useful while porting | Designer, `.fuseui` (the editor can be built in C++) |

Toolkit-independent editor features (gizmos, more panels, play-mode work) keep landing in Stage 1 on Qt, inside the rules in Section 3.

---

## 8. The switch (Stage 3)

1. Add `FUSE_EDITOR_SHELL` (`qt` default, `fuse`). Both shells link the same `fuse_editor_api` and the same `EditorController`; `fuse_editor_fuse` is a new target beside `fuse_editor_qt`.
2. Bring the FUSE UI shell up in this order, each step with its twin gates: main window, docks and action-table menus → viewport widget → console → hierarchy → inspector → project hub and wizard → asset browser → profiler → material editor → SDF sculpt → sequencer.
3. The editor UI context runs on the editor UI thread and talks only to `EditorController`, so the "UI posts, game thread drains" rule is unchanged.
4. When all 23 twins are green and accessibility adapters are in: sign-off, then remove `fuse_editor_qt`, the Qt tests and the Qt `find_package` calls.
5. Update the rules that name Qt: `fuse_lint_b6_editor_qt6_only` becomes an `editor-shell` lint that allows FUSE UI; `coding-standards.md` ("Qt owns chrome"), `editor.md`, the threading table in `architecture.md`, and master plan §A3 get a superseded note pointing here.
6. The editor becomes buildable for tablets (Android, iOS) once UI1 mobile and UI5 mobile land — a new target, not part of the switch.

Fallback if the whole-shell switch stalls: host individual FUSE UI panels inside Qt docks through the same `QWindow` container path the viewport uses, and move panel by panel. It costs extra integration work that is thrown away afterwards, so it is not the default.

---

## 9. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| The Qt shell keeps growing faster than the seam | Port cost grows | Rules R1–R9; `editor-shell` lint enforcing after Q9; review checklist |
| `EditorController` becomes a god object | Hard to maintain | One facade over per-area controllers (files, selection, viewport, actions); the facade only forwards |
| Snapshot views are one frame behind | Visible lag in panels | One frame at editor rates is below perception; inspector drags already coalesce through commands |
| Two shells during Stage 3 | Double maintenance | Shared controller and scenarios; Stage 3 kept short; Qt removed promptly after sign-off |
| Accessibility regression when Qt goes | Editor unusable with screen readers | Desktop adapters are a hard gate for Qt removal (Section 7) |
| Qt licensing while the Qt editor is distributed | LGPL-3 obligations for binaries you ship | Link Qt dynamically, ship its licence notices, allow replacement — standard for LGPL use; ends with Qt removal |

---

## 10. First steps

1. Q11 in warn-only mode, so the size of the problem is visible in CI.
2. Q1: `EditorController` + `ShellServices` with the mutex inside; route the 29 mutex sites through it.
3. Q2 and Q6 (small, high value): one undo history and the action table.
4. From here on, every new Qt panel follows R1–R9.
