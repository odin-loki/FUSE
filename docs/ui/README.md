# FUSE UI

What is not done and what has never been run on real hardware: [`KNOWN_GAPS.md`](KNOWN_GAPS.md).

FUSE's own GUI framework ([plan](../plans/FUSE_UI_PLAN.md)). One retained core, styled with a CSS subset, drawn
through a backend-neutral draw list, plus an immediate-mode front-end on the same primitives. Engine rules apply:
handles, not pointers; `std::expected`; one owner thread (other threads `post()`); no steady-state allocation.

Status (2026-10-29): headless core (UI2) with all nine gates green, `.fuseui` documents and Lua bindings, MINIMAL
text plus an opt-in FULL profile (HarfBuzz + FreeType, SheenBidi bidi, libunibreak line breaking, shaped-run cache),
Tier A and Tier B widgets including splitters, dock area, item models and a table view (UI6), a Vulkan compute
backend for the draw list feeding both the runtime UI stage (`UiFrameSource`) and app windows (UI4), desktop text
input / IME / wheel / clipboard on Win32 and X11 (UI1), the `fuse_app` application shell with real OS windows and a
Vulkan swapchain presenter (UI5), and the game UI library `fuse_ui_game` (UI7). Relight's dev menu runs on the
immediate-mode front-end. See the plan's status table for what is still open.

## Layout of the code

| Path | Target | What |
|---|---|---|
| `Source/FUSE/UI/Text` | `fuse_text` | Fonts (stb_truetype; FreeType in FULL), bundled Open Sans, UTF-8, `ITextShaper` (`MinimalShaper`, `HarfBuzzShaper` in FULL), wrap/ellipsis layout, glyph atlas, `TextEditBuffer` |
| `Source/FUSE/UI/Core` | `fuse_ui_core` | `UiContext` (tree, style, layout, paint, input, focus, signals, timers, animation, a11y), `DrawList` + `CpuRasterizer`, `Path` / `Painter` / `SvgImage` / built-in icons, `StyleEngine`, `immediate::ImmediateUi`, `platform_bridge.hpp` |
| `Source/FUSE/UI/Widgets` | `fuse_ui_widgets` | Tier A + Tier B widgets + default dark theme (`widgets::install`) |
| `Source/FUSE/UI/Doc` | `fuse_ui_doc`, `fuse_ui_cook`, `fuse_ui_extract_strings` | `.fuseui` JSON documents, binding model, expression bytecode; the cooker and the string-extraction CLI |
| `Source/FUSE/UI/L10n` | `fuse_ui_l10n_lib` | CLDR plural rules, ICU-style message formatting, locale number / date / time / currency formatting, string-table file format (standard library only, no ICU) |
| `Source/FUSE/UI/Script` | `fuse_ui_script` | Lua `ui` table (vendored Lua 5.2) |
| `Source/FUSE/UI/Os` | `fuse_ui_os` | `fuse::ui::os` seam (`fuse/ui/os.hpp`): files, environment, clocks, dynamic libraries, helper processes, file watcher, instance lock. Default backend `Os/src/os_std.cpp` (standard library + POSIX / Win32). Built with `fuse_ui_extras` |
| `Source/FUSE/UI/Extras` | `fuse_ui_extras` | Undo framework, declarative properties and state machines, PDF writer, desktop integration, tray icon and global menu, system web view, charts library, 3D data visualisation (option `FUSE_BUILD_UI_EXTRAS`; see [Extras](#extras-fuse_ui_extras)) |
| `Source/FUSE/UI/Render` | `fuse_ui_render`, `fuse_ui_vk` | GPU draw data packing, `ui.draw_list` compute kernel, `UiLayerGpu` render-graph pass and `UiFrameSource` (in tree with the Vulkan RHI) |
| `Source/FUSE/UI/App` | `fuse_app`, `fuse_app_native` | `Application` / `AppWindow` on-demand loop, settings, command line, single instance; native X11 / Win32 windows and the Vulkan swapchain presenter; samples `hello_app`, `notes_app`, `widget_gallery`, `game_menu` |
| `Source/FUSE/UI/Game` | `fuse_ui_game` | HUD widgets, world markers, `UiSurface` world panels, gamepad navigation, input arbitration, prompts, time domains, locale table, subtitles, dialogue; samples `hud_demo`, `world_panel_demo` |
| `Source/FUSE/UI/Sql` | `fuse_ui_sql` | `SqliteModel`: an `ItemModel` over a SQLite query (option `FUSE_BUILD_UI_SQL`) |
| `Source/FUSE/UI/A11y` | `fuse_ui_a11y` | Accessibility bridge (snapshot diff, action queue); AccessKit adapter when `FUSE_UI_ACCESSKIT_DIR` is set |
| `Source/FUSE/UI/Designer` | `fuse_ui_designer_lib`, `fuse_ui_designer` | `.fuseui` designer prototype (UI8) |
| `Source/FUSE/Core/include/fuse/platform/text_services.hpp` | `fuse_core` | clipboard, text-input begin/end, IME candidate placement |
| `Source/FUSE/UI/tests` | gates | `fuse_ui_*` ctest executables and `fuse_ui_sample` |
| `Source/FUSE/Core/include/fuse/platform/events_v2.hpp` | header | `KeyV2` / `EventV2` with lossless v1 mapping |

`fuse_ui_core` does not depend on `fuse_core`; the platform bridge is header-only.

The tree also holds source files that no target builds yet: the UI work of the merged branch
`claude/magical-bassi-815825` that still has to be ported onto this code (cooked / MSDF / variable
fonts, the SVG icon cooker, AccessKit window adapters, GPU effects, `ProjectHost`, their tests
and goldens). [`archive/PORT_magical_bassi.md`](archive/PORT_magical_bassi.md) lists every one of them, what blocks
it and how to verify it once ported. Nothing in this document describes those files unless it says so.

The same holds for the merged branch `gui-framework`: its `Extras/` modules that compile against this code are built
(the row above); the rest of its UI work is in the tree unbuilt (a second Wayland backend kept for its drag-out code, Cocoa /
UIKit / Android backends with a Java activity, the CSS engine additions, GPU paths / filters / HDR, FORGE script
bindings, rich text, SVG documents, media, more widgets). [`archive/PORT_gui_framework.md`](archive/PORT_gui_framework.md)
is the list. Files of that branch whose path was already taken on main are kept under
`Source/FUSE/UI/_port/gui-framework/<same relative path>`; nothing builds from `_port/`. `docs/gui/` and `scripts/gui/`
are that branch's own plan, test notes and scripts, kept as an archive.

## Build and test

In tree it builds with `FUSE_BUILD_UI=ON` (default). Standalone, which is quickest:

```sh
cmake -S Source/FUSE/UI -B build-ui -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-ui && ctest --test-dir build-ui
build-ui/tests/fuse_ui_sample out.png          # renders the Tier A sample via the CPU rasteriser
```

Regenerate the raster golden after an intended visual change:
`cd Source/FUSE/UI/tests && FUSE_UI_UPDATE_GOLDEN=1 ../../../../build-ui/tests/fuse_ui_cpu_raster_golden`.

Options: `FUSE_BUILD_UI_WIDGETS`, `FUSE_BUILD_UI_DOC`, `FUSE_BUILD_UI_SCRIPT`, `FUSE_BUILD_UI_VK`, `FUSE_BUILD_UI_TESTS`,
`FUSE_UI_TEXT` (`MINIMAL` default; `FULL` fetches HarfBuzz 10.1.0 + FreeType 2.13.3 at configure time, so it needs
network or a populated `FETCHCONTENT_BASE_DIR`; `COOKED` falls back to MINIMAL), `FUSE_UI_ACCESSIBILITY`,
`FUSE_UI_PROFILE`, `FUSE_BUILD_UI_APP`, `FUSE_BUILD_UI_GAME` (both ON by default); `FUSE_BUILD_APP` (UI9) and
`FUSE_BUILD_UI_DESIGNER` are reserved. The HUD golden is regenerated the same way as the raster golden with
`fuse_ui_game_hud_golden` (`tests/golden/hud.png`, `hud_full.png` in FULL).

FULL text: `cmake -S Source/FUSE/UI -B build-ui-full -G Ninja -DFUSE_UI_TEXT=FULL -DCMAKE_BUILD_TYPE=Release`. It has
its own raster golden (`tests/golden/sample_full.png`). GPU parity runs on any Vulkan 1.2 device; for software use
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json` (lavapipe).

## Retained use

```cpp
#include <fuse/ui/ui.hpp>
#include <fuse/ui/widgets/widgets.hpp>
using namespace fuse::ui;

auto font = fuse::text::Font::bundled().value();
UiContext ui({.viewport = {800, 600}, .font = &font});
widgets::install(ui);                                  // registers classes + default theme
ui.addStyleSheet(".card { padding: 16px; } :root { --accent: #4c8dff; }");

NodeHandle card = widgets::panel(ui, ui.root(), "card");
ui.addClass(card, "card");
NodeHandle ok = widgets::button(ui, card, "Apply", "apply");
ui.connect(ok, SignalKind::Clicked, [](UiContext&, NodeHandle, const SignalArgs&) { /* ... */ });

// per frame
ui.pointerMove({x, y}); ui.pointerDown({x, y}); ui.keyDown(Key::Tab); ui.textInput("é");
ui.update(dt);                                         // commands → timers → anims → style → layout → paint
if (ui.drawList().size()) { /* hand ui.drawList() to a backend, or CpuRasterizer for tests */ }
```

From another thread: `ui.post(fn, node, a, b)` (bounded queue, applied at the next `update()`).

### Platform input (UI1)

```cpp
#include <fuse/ui/platform_bridge.hpp>
fuse::ui::usePlatformClipboard(ui);                    // Ctrl+C/X/V in line edits use the OS clipboard
PlatformEvent ev;
while (pump.pollEvent(ev)) fuse::ui::feed(ui, ev, pump, dpiScale);   // keys, text, IME pre-edit, wheel, pointer
fuse::ui::syncTextInput(ui, window, dpiScale);         // starts/stops IME and places the candidate window
```

### Tier B widgets

Popups live in an overlay layer (`widgets::overlayLayer`), the root's last child, outside layout. Each popup is a
viewport-sized scope (the scrim) that traps focus; outside click or Esc closes it and focus returns to the owner.

```cpp
NodeHandle bar = widgets::menuBar(ui, ui.root());
NodeHandle file = widgets::menuBarMenu(ui, bar, "File", "New\nOpen\n-\nQuit");
ui.connect(file, SignalKind::Submitted, [](UiContext&, NodeHandle, const SignalArgs& a) { /* a.value = index, a.text */ });
NodeHandle list = widgets::listView(ui, parent, items);           // virtualised: paints visible rows only
NodeHandle tree = widgets::treeView(ui, parent, "Scene\n\tCamera\n\tLights");  // tabs = depth (models: treeModelView)
NodeHandle tabs = widgets::tabWidget(ui, parent, "General\nRender");
widgets::label(ui, widgets::tabPage(ui, tabs, 0), "...");
NodeHandle dlg = widgets::dialog(ui, "Delete asset?");
widgets::button(ui, widgets::dialogBody(ui, dlg), "Delete");
widgets::openDialog(ui, dlg);                                     // modal; Esc or closeDialog hides it
widgets::setTooltip(ui, someButton, "Explains the button");
```

With Tier B installed (`widgets::install` does both tiers), a combo box opens a popup list instead of cycling.

### The rest of the catalogue

`groupBox` (optionally checkable), `stack`, `toolBar` / `toolBarAction`, `statusBar` / `statusMessage`, `mainWindow`
(menu bar + tool bar + dock area + status bar), `setContextMenu`, `messageBox`, `wizard`, `popover`, `toast`,
`headerView`, `textEdit` (word wrapped, undo, clipboard, IME), `plainTextEdit`, `richText` (inline markup with
links) and `doubleSpinBox`. Gate: `fuse_ui_tier_b3`, one `fuse_ui_widget_<name>` ctest per widget.

The last four pieces of the catalogue (gates `fuse_ui_widget_chart_flame`, `_dock_tabs`, `_dock_float`,
`_dock_layout_tabs_float`, `_minimap`, `_ammo_counter`):

- **Flame graph**: `chart(ui, parent, ChartKind::Flame)` + `chartSetFlame(ui, chart, spans)` with
  `FlameSpan {name, start, duration, depth, color}`. Bars are stacked by depth (0 at the top) and scaled to the total
  time range; a label is cut to its bar. Hover sets `value()` to the span index (-1 outside), a click emits
  `Submitted {value = index, text = name}`. `chartFlameSpanRect` / `chartFlameSpanAt` give the geometry.
- **Tabbed docks**: `dockTabify(ui, dock, withDock)` puts `dock` in `withDock`'s region as a tab. The group shows a
  tab strip instead of title bars and only the current dock's body; `dockRaise` (or a click on a tab, Left / Right on
  the strip) changes it, `dockCurrent` / `dockTabBar` / `dockTabRect` query it. `dockTo` takes a dock out of its group.
- **Floating docks**: `dockFloat(ui, dock, rect)` moves the dock into the overlay layer as a panel that is dragged by
  its title bar; `dockTo` docks it again; `dockFloating` / `dockFloatRect` query it. A floating dock holds no tabs, and
  it is a panel inside the window, not an OS window.
- **Dock drag and drop**: dragging a docked dock's title bar, or its tab in a tab strip, more than 8 px tears it off
  into a floating dock that follows the pointer. While the pointer is over the dock area, indicators show for Left /
  Right / Bottom (area edges), Center (centre region) and Tab (middle of the docked dock under the pointer); the one
  under the pointer is hot and a translucent preview shows where the dock will land. Releasing on a hot zone docks it
  (`dockTo`) or makes it the current tab of that dock's group; anywhere else it stays floating. Escape (the dragged
  dock's title holds the focus during the drag), `dockCancelDrag` or a cancelled touch puts it back where it came from
  (region and order, tab group / index / current tab, or floating rect). `dockDragged`, `dockDropZone`,
  `dockDropTarget`, `dockDropPreview` report the drag; `dockDropIndicator(ui, area, zone, tabWith)` is where a zone's
  indicator sits. Works with the mouse and with touch. Gates `fuse_ui_dock_drag_*`.
- `dockSaveLayout` adds `tab <id> <group> <index> current|back` and `float <id> <x> <y> <w> <h> open|closed` lines
  only when docks are tabbed or floating; layouts written before still restore, and restoring one un-tabs and docks
  the docks it names.
- **Minimap** (`fuse/ui/game/game_extras.hpp`): `minimap(ui, parent, id)`, `setMinimapRange` (world units that reach
  the frame), `setMinimapHeading(ui, map, degrees, rotateMap)` (heading-up or north-up), `setMinimapShape` (circle /
  square), `setMinimapImage` (rasteriser image index, drawn north-up) and `minimapSetBlips` (`MinimapBlip {pos, color,
  kind}`, position relative to the player, +x east, +y north). Blips beyond the range sit on the frame edge;
  `minimapBlipPos` returns where a blip is drawn.
- **Ammo counter**: `ammoCounter(ui, parent, id)`, `setAmmo(ui, node, inMagazine, magazineSize, reserve)` shows
  "12 / 90" and one pip per round (a bar above `kAmmoMaxPips` = 40), turns to the danger colour at or below 25 % of
  the magazine (`setAmmoLowFraction`), `setAmmoReload(ui, node, progress01)` shows a reload bar. A count change emits
  `ValueChanged`.

### Dock splits, tree line geometry, timeline clip drag

Ported from `claude/magical-bassi-815825` onto main's dock model (the FUSE editor shell calls these); gates
`fuse_ui_dock_split_{split,split_groups,tree_line,timeline_clip}`, golden `fuse_ui_port_golden_dock_split`.

- **Dock splits** (`QMainWindow::splitDockWidget`): `dockSplit(ui, dock, target, DockEdge::Left|Right|Top|Bottom, px)`
  puts `dock` in a new pane beside `target`. A side region (and the centre) then holds a tree of nested splitters
  (class `docksplit`) whose leaves are docks; a split of the same direction gains a pane instead of nesting, a split
  left with one pane collapses, panes and handles hide while their docks are closed. `DockEdge::Center` makes `dock`
  a tab of `target`'s group (`dockTabify` + `dockRaise`). A tabbed target is split as a whole group. `dockOpen`
  re-shows a closed dock where it is, `dockRemove` destroys one (collapsing what it leaves), `dockSideOf` /
  `dockSplitOf` query the place. Splits work with what main already had: `dockTo`, `dockFloat`, `dockTabify` and a
  title-bar drag take a dock out of its pane, and Escape during a drag puts it back in its pane with its size.
  The drop zones of a drag are unchanged (Left / Right / Bottom / Center / Tab): a split is made from code, not by
  dropping on a dock's edge.
- `dockSaveLayout` writes `fusedock 2` when the layout has splits: one `tree <side> <node>...` line per side
  (`d:<id>`, or `h[` / `v[` + `p <px> <node>...` per pane + `]`). Layouts without splits are still written as
  `fusedock 1`, byte for byte as before. `dockRestoreLayout` reads both; it flattens the area's splits first.
- **Tree line geometry**: `treeLineAt(ui, tree, pos)` (line under a point, -1 outside the rows) and
  `treeLineRect(ui, tree, line)` (the row of a shown line; empty inside a collapsed subtree): drag-and-drop targets
  and drop marks on a `treeView`.
- **Timeline clips**: a press on a clip selects it (white outline) and a clip drag reports itself: `Event {value = 5}`
  on press, `6` per move, `7` on release, with `timelineClipDrag(ui, tl)` giving track, clip, the pointer time at
  press / now (unsnapped), the clip start at press and whether the drag is still on. Shift moves a clip without
  snapping. `timelineSelectClip` / `timelineSelectedClip`; Ctrl+Down / Up select the next / previous clip, Ctrl+Left
  / Right move the selected clip by the snap step (Ctrl+Shift: a second) and emit `Event {value = 2}`; Escape clears
  the selection. (The core also offers unconsumed input events to `Event` slots with `value = 0`.)

### Qt-parity options (`widgets/edit_options.hpp`)

Ported from `gui-framework`. Options are per-node records set by a call; a widget no option was set on behaves and
paints exactly as before (its gates and goldens did not change). Golden: `fuse_ui_port_golden_edit_options`.

```cpp
NodeHandle pw = widgets::lineEdit(ui, parent);
widgets::setLineEditOptions(ui, pw, {.echo = widgets::EchoMode::Password, .maxLength = 32, .placeholder = "Password"});
widgets::setLineEditOptions(ui, port, {.validator = widgets::intValidator(1, 65535), .fixup = widgets::intFixup(1, 65535)});
widgets::setLineEditOptions(ui, ip, {.inputMask = "000.000.000.000;_"});
std::string_view secret = widgets::lineEditText(ui, pw);        // ui.text(pw) is the mask, never the secret
widgets::setCompleter(ui, edit, {.items = "Apple\nApricot\nBanana"});                 // popup; Inline / UnfilteredPopup
NodeHandle combo = widgets::editableComboBox(ui, parent, "Red\nGreen\nBlue", 0, widgets::InsertPolicy::InsertAtBottom);
widgets::setSpinBoxOptions(ui, spin, {.prefix = "$ ", .suffix = " ea", .specialValueText = "Free", .wrapping = true});
widgets::setTabsClosable(ui, widgets::tabWidgetBar(ui, tabs), true);                    // Event "tabCloseRequested"
widgets::setTabsMovable(ui, bar, true);                                                 // Event "tabMoved"
```

- **Line edit** (gate `fuse_ui_lineedit`): echo modes (`Normal`, `NoEcho`, `Password`, `PasswordEchoOnEdit`; no copy
  / cut while masked; the node text, the accessibility value and inspectors only ever see the mask), `maxLength` in
  code points, `placeholder`, `readOnly`, validators (`intValidator`, `doubleValidator`, `regexValidator`: Invalid
  edits are rejected, Enter submits only Acceptable text, `fixup` may repair it), input masks (QLineEdit syntax),
  `EditingFinished` on Enter / focus loss. `regexValidator` runs on a built-in partial matcher (literals, classes,
  groups, alternation, quantifiers); a pattern with backreferences or lookaround is not supported and then accepts
  everything (`regexPartialMatch(..., &supported)` tells). Assistive technology setting the value goes through the
  options too.
- **Completer** (`fuse_ui_completer`): `setCompleter(ui, edit, {items | model + column, mode, caseSensitive, filter,
  maxVisibleItems, wrapAround, sorting, icon})`; popup under the field with the focus kept in the field, inline
  completion, unfiltered popup; `Highlighted` / `Activated` signals; `complete`, `setCompletionPrefix`,
  `completionCount` / `completionAt`, `completerPopup`.
- **Editable combo box** (`fuse_ui_editcombo`, class `editcombo`): line edit + inline completer + drop button; insert
  policies, `maxCount`, duplicates, validator, model column (`setComboModel`), per-item icons and user data,
  `comboInsertItem` / `comboAddItem` / `comboRemoveItem`, `comboFindText` / `comboFindData`.
- **Spin box** (`fuse_ui_spinbox`): with options a spin box takes typed entry (focus selects the value; selection,
  clipboard, Escape reverts, Enter / focus out commits), prefix / suffix, `decimals`, `wrapping`,
  `specialValueText`, adaptive decimal stepping, `keyboardTracking`, `correction`, accelerated auto-repeat of the
  arrow buttons, `textFromValue` / `valueFromText`; `spinBoxText`, `spinBoxCleanText`, `spinBoxStepBy`,
  `spinBoxStepEnabled`, `spinBoxSelection`. A spin box without options still steps once per press and takes no typing.
- **Tab bar** (`fuse_ui_tab_options_{closable,movable,scroll,pages}`): closable tabs, movable tabs (live reorder; the
  pages of a tab widget follow), `setTabEnabled`, scroll buttons when the tabs overflow (the current tab is kept in
  view) or `setTabUsesScrollButtons(false)` to shrink the tabs instead, wheel / Home / End, `tabBarMoveTab`,
  `tabBarTabRect`, `tabWidgetAddPage` / `tabWidgetRemovePage`. Not ported: tab position (South / West / East),
  elide modes, per-tab icons and tooltips.

Not ported from the branch's §3.7 (its code stays parked): `dateEdit` / `timeEdit` format sections and
`DateTimeEditOptions`, `CalendarOptions` (week numbers, date range), tree-view drag and drop / scrolling /
multi-selection, table multi-row and cross-view drags, header resize modes and the header menu, list icon mode,
`splitterSizes`, tool bar overflow / movable / floating tool bars, and the MIME-typed drag payloads
(`widgets_dnd.cpp`: main has its own drag and drop).

### Extra widgets (`widgets/extra.hpp`)

Ported from `gui-framework`'s Extras into `fuse_ui_widgets` (part of `install()` / `installClasses`, the default
theme, the accessibility tree and `.fuseui` documents). Gates `fuse_ui_extra_{buttonbox,dialog_keys,input_dialog,
toolbox,lcd,keysequence,mdi,graphics,vkeyboard,classes}`, golden `fuse_ui_port_golden_extra`. The options above and
these widgets are document classes / properties too (`editcombo`, `lcdnumber`, `keysequenceedit`, `toolbox`, `mdiarea`
+ `mdiwindow`, `graphicsview`, `vkeyboard`, `dialogbuttonbox`; `placeholder` / `echo` / `inputMask` ... on `lineedit`,
`prefix` / `suffix` ... on `spinbox`, `closable` / `movable` on `tabwidget`; reference in `doc/document.hpp`, gate
`fuse_ui_port_doc`), they are on the "More" page of the `widget_gallery` sample, and steady frames over all of them
allocate nothing (`fuse_ui_port_zero_alloc`).

- `dialogButtonBox(ui, parent, StdButton::Ok | StdButton::Cancel, {.layout})`: standard and custom buttons with roles
  in the Windows / macOS / KDE / GNOME order; `Clicked {role}` then `Submitted {1 | 0}` on the box.
- Dialog keys: `setDialogDefaultButton` / `setDialogRejectButton` (a button box sets both). Escape presses the reject
  button; an Enter nothing inside consumed presses the default button (a focused push button presses itself, a line
  edit keeps Enter for `Submitted`). `messageBox` now answers Escape with its Cancel / No / Close / Abort button when
  it has one. `setWizardValidator(ui, wizard, fn, user)` blocks Next / Finish per page.
- `inputDialog(ui, title, prompt, initial)`: `Submitted {text, value = 1}` on OK / Enter, `{value = 0}` on Cancel /
  Escape; `inputDialogEdit` is its field (set a validator on it).
- `toolBox(ui, parent, "General\nAdvanced")` + `toolBoxPage` / `setToolBoxIndex` / `toolBoxAddPage`.
- `lcdNumber(ui, parent, digits)`: seven-segment display of `value()` or `setLcdText`; `setLcdBase` 2 / 8 / 10 / 16.
- `keySequenceEdit(ui, parent, Shortcut)`: records a key chord (`keySequence`, `setKeySequence`).
- `mdiArea` + `mdiSubWindow(ui, area, title, rect)` / `mdiBody`: move by the title bar, resize by the corner grip,
  press to activate, x to close; `mdiTile`, `mdiCascade`, `mdiActivate`, `mdiClose`. Panels inside the area, not OS
  windows.
- `GraphicsScene` (rect / ellipse / line / text / polygon items, z order, hit testing, selection, uniform-grid index)
  and `graphicsView(ui, parent, &scene)`: wheel zoom about the pointer, drag to move the selection, rubber-band
  selection on empty space (Ctrl adds), Alt / middle drag pans, arrows move, Delete removes; `fitInView`,
  `viewToScene` / `sceneToView`, `viewRubberBand`. Non-circular ellipses and polygons are drawn as vector paths.
- `virtualKeyboard(ui, parent)`: QWERTY, one-shot shift / caps lock, a symbols page; the keys type into the text
  widget that had the focus (`virtualKeyboardAutoShow` per frame shows it only while a text widget is focused).

### Documents and Lua

`.fuseui` files are JSON trees with `{expr}` and `{=two-way}` bindings (see `Doc/include/fuse/ui/doc/document.hpp` for
the format and `tests/data/settings.fuseui` for an example); `fuse_ui_script` exposes the same context and model to Lua
as a global `ui` table. A Lua handler (`ui.on`) is released, with its registry reference and its connection, once
its node is gone: `LuaUi` sweeps on `update()`, `ui.on` and `ui.destroy` (`handlerCount()`; gate `fuse_ui_script_bind`).

A node's `"class"` is a registered widget class or a **document class**: the compound widgets and the widgets that
need data are created by name too, with their own properties (the full list is in `document.hpp`):

```json
{ "class": "tabwidget", "tabs": ["Audio", "Video"], "children": [
    { "class": "groupbox", "text": "Volume", "children": [
        { "class": "slider", "min": 0, "max": 1, "value": "{=audio.master}", "tooltip": "Master volume" } ] },
    { "class": "tableview", "columns": ["Name", "Size"], "rows": [["scene.fuse", 1204], ["hero.png", 88]] } ] }
```

`tabwidget` (child k = page k), `stack` (`"value"` = page, bindable), `splitter`, `dockarea` / `dock`, `groupbox`,
`scrollarea`, `toolbar`, `statusbar`, `mainwindow`, `dialog` (`"visible"` opens / closes it), `propertygrid`,
`tableview` / `modellistview` / `gridview` over inline data, `treemodelview` over inline `"items"` (the tabs at the
start of a line are its depth, the tabs after them separate the cells; `"columns"`, `"editable"`, `"expanded"`),
`svgimage` (`"icon": "folder"` names a built-in icon, `"svg"` is inline source), `chart`, `nodegraph`, `curveeditor`,
`timeline`, `calendar`, `datetimeedit`, `colorpicker`, `codeedit`, `markdown`, `fontpicker`, `viewport`, `video`, ...;
any node takes `"tooltip"` and `"contextMenu"` (also the views and inputs that use the press themselves: a right
press makes a view's row current, then opens the menu), a `button` / `toolbutton` takes `"icon"`. Message box,
wizard, popover, toast and file dialog open from code.
`doc::registerDocumentClass` adds your own (the game HUD widgets, for instance). Compound widgets are rebuilt on a
hot reload; plain widgets with an id inside them keep their identity. The cooker's registration list
(`registrationList`) resolves document classes to the widget classes they build. Cooked blobs carry
`kCookedVersion` (3 now: it moves whenever the property set or the stored form changes; a blob of another version is
refused, so re-cook after updating). `App/samples/ui/gallery.fuseui` uses every one of them. Gates `fuse_ui_doc_*`.

## Application shell (UI5)

```cpp
#include <fuse/app/app.hpp>
#include <fuse/app/native.hpp>
fuse::app::Application app{{.name = "Hello", .argc = argc, .argv = argv}};
fuse::app::NativePlatform native(app);                      // declare after the Application; installs the OS event source
auto& w = app.createWindow({.title = "Hello", .width = 800, .height = 600});
if (auto r = w.load("ui/main.fuseui", &model); !r) return 1;
native.show(w);                                             // X11 / Wayland / Win32 window + Vulkan swapchain (GDI / wl_shm / XPutImage without Vulkan)
return app.run();                                           // sleeps until input, a timer or an animation needs a frame
```

Without a native platform the app runs headless (tests, `--screenshot=`). `PresenterKind` reports what was picked
(`Vulkan` draws the draw list on the GPU with `UiVkRenderer`, or uploads CPU-rasterised damage with
`VkDrawMode::CpuUpload`; `Software` blits). `NativePlatform::capture` reads back what the OS shows (used by `fuse_app_native_window`).
Under Xvfb set `VK_ICD_FILENAMES` to lavapipe; under Wine the Win32 path and the Vulkan presenter both run.

### Project files: `kind: app` (in tree)

`project.json` schema 2 describes a UI application (`fuse/project/manifest.hpp`; schema 1 files load unchanged):

```json
{ "schemaVersion": 2, "name": "my_tool", "kind": "app",
  "dimensions": { "enable3D": false, "enable2D": false, "enableUI": true },
  "ui": { "root": "ui/main.fuseui", "styles": ["ui/theme.css"], "theme": "system", "redraw": "onDemand" } }
```

`fuse::app::ProjectHost` (`fuse/app/project_host.hpp`, target `fuse_app_project`; needs `fuse_project` and
`fuse_core`, so it exists only in the engine tree) loads the manifest, brings up only what the plan asks for (the
shell and `AppPresent`; no worlds, renderer or physics), opens the declared windows (one titled after the project
when none is listed) with `ui.root`, adds `ui.styles`, applies `ui.theme` (`system` follows the OS through
`setFollowSystemTheme`, `dark` / `light` pin the widget theme) and `ui.redraw`, and presents the first frame.
`fuse_app_host <project dir | project.json> [--headless] [--first-frame] [--screenshot=out.png]` is the runner.
A `kind: hybrid` project needs the engine game side (`fuse_app_project_hybrid`, `hybridGameSide()`, runner
`fuse_hybrid_host`); an app-only host refuses it with a message.

```cpp
auto host = fuse::app::ProjectHost::start({.projectPath = "MyTool"});
if (!host) return std::fprintf(stderr, "%s\n", host.error().c_str()), 1;
return (*host)->run();
```

### Platforms: Android, macOS, iOS, FreeBSD

`fuse_app_native` has one window backend per platform; `NativePlatform` is the same API on all of them.

| Platform | Backend | Present | Entry |
|---|---|---|---|
| Linux (X11), Windows | `App/src/native.cpp` | Vulkan swapchain; GDI on Win32 without Vulkan | `main` |
| Linux / FreeBSD (Wayland) | `App/src/native_wayland.cpp` (protocol layer) + `native_wayland_platform.cpp`, built into the same library as the X11 backend and chosen at run time; `libwayland-client.so.0` (and optionally `libxkbcommon.so.0`, `libwayland-cursor.so.0`) are loaded at run time, so no Wayland headers or libraries at build time | Vulkan swapchain (`VK_KHR_wayland_surface`) when the build finds a Vulkan loader, else the CPU raster through `wl_shm` | `main` |
| FreeBSD (X11) | `App/src/native.cpp` (same backend as Linux; `libX11.so.6` is loaded at run time) | Vulkan swapchain (`VK_KHR_xlib_surface`) when the build finds a Vulkan loader, else the CPU raster presenters only | `main` |
| macOS | `App/src/native_apple.mm` (Cocoa `NSWindow`, `NSTextInputClient` IME, `NSPasteboard`) | Vulkan over a `CAMetalLayer` (MoltenVK) when the build finds a Vulkan loader, else the CPU raster as the layer contents | `main` |
| iOS | `App/src/native_apple.mm` (UIKit `UIWindow`, touch, software keyboard through `UIKeyInput`) + `entry_ios.mm` | same as macOS | `UIApplicationMain`, which runs your `FUSE_APP_MAIN` |
| Android | `App/src/native_android.cpp` (NativeActivity + NDK app glue, touch / mouse / keys, soft keyboard, clipboard over JNI) + `entry_android.cpp` | Vulkan (`VK_KHR_android_surface`), else the CPU raster through `ANativeWindow_lock` | `android_main`, which runs your `FUSE_APP_MAIN` |

```cpp
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"
#include "fuse/app/main.hpp"   // FUSE_APP_MAIN: main on desktop; called by the platform entry point on Android / iOS

int FUSE_APP_MAIN(int argc, char** argv) {
    fuse::app::Application app{{.name = "hello", .argc = argc, .argv = argv}};
    fuse::app::NativePlatform native(app);
    auto& window = app.createWindow({.title = "Hello"});
    native.show(window);
    return app.run();
}
```

```cmake
fuse_app_add_executable(hello hello.cpp)   # executable; .app bundle on iOS; libhello.so for NativeActivity on Android
```

```bash
# Android (NDK r26+): libraries, gates and the sample apps as lib<name>.so
cmake -S Source/FUSE/UI -B build-ui-android -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 && cmake --build build-ui-android
# macOS (optional Vulkan presenter: brew install molten-vk vulkan-headers vulkan-loader)
cmake -S Source/FUSE/UI -B build-ui-macos -G Ninja && cmake --build build-ui-macos
# iOS simulator
cmake -S Source/FUSE/UI -B build-ui-ios -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 && cmake --build build-ui-ios
# FreeBSD 14 (base clang / libc++; optional Vulkan presenter: pkg install vulkan-headers vulkan-loader)
pkg install cmake ninja && cmake -S Source/FUSE/UI -B build-ui-freebsd -G Ninja && cmake --build build-ui-freebsd
```

Things that differ on these platforms:

- **One OS window** on Android and iOS: the first window shown gets the surface, later ones stay headless.
- **Scale (HiDPI).** The UI works in logical units (dp / points): style, layout, events, hit testing, `rect()` and
  `AppWindow::size()` never see pixels. The frame is rasterised at the display's physical pixel size:
  `UiContext::setRenderScale(s)` makes `drawList()` (and popup surfaces) device pixels = logical x `s`, with glyphs
  shaped at the logical font size and rasterised at font size x `s`, so text is as sharp as the screen. Widgets and
  custom `paint` callbacks keep drawing in logical units. `AppWindow::setScale` / `WindowDesc::dpiScale` set it for a
  window; `image()`, `writePng()` and the presenters then work at `pixelSize()`. On macOS / iOS / Android the backend
  sets the display's scale (`NativeOptions::scale` overrides the factor); X11 / Win32 windows run at scale 1.
  `NativePlatform::scale()` and `safeArea()` report the scale and the area clear of notches and system bars.
- **Resources.** `AppWindow::load("ui/main.fuseui")` falls back to `fuse::app::readResource`, which reads APK assets
  on Android and the bundle's `Resources` on iOS / macOS. Settings live under `fuse::app::dataRoot()` on mobile.
- **Fonts.** Open Sans is embedded in the binary on Android / iOS (`FUSE_TEXT_EMBED_FONT`). The fallback faces
  (Noto, Twemoji) load from `fonts/<vendor path>` in the assets / bundle when you ship them.
- **Lifecycle.** Without a surface (Android `TERM_WINDOW`) the presenter goes headless and comes back with the next
  surface; a paused / inactive app blocks in the event pump and renders nothing.
- **macOS modifiers.** Command reports as `kModCtrl` (Cmd+C / V / Z drive the widgets' shortcuts), Control as `kModSuper`.
- **Touch.** Fingers go through `UiContext::touchDown / touchMove / touchUp / touchCancel` (a mouse on Android keeps
  `pointerDown / Move / Up` and the wheel). A tap clicks. A drag past the slop (8 px, `setTouchScroll`) cancels the
  press (`PointerUp` with `Event::cancel`, no `Click`) and pans the nearest ancestor that can scroll along the drag
  axis 1:1: a scroll container (`overflow: scroll`), or a list / table / grid / text edit / code edit, which receive
  Wheel events in whole rows. A container at its end in the drag direction hands the gesture to its ancestor.
  Releasing in motion flings (velocity from the last 100 ms, exponential decay, stops at the extents, no
  overscroll); `needsUpdate()` / `nextWakeSeconds()` keep the on-demand loop running until it stops, and a new touch
  stops it without clicking. Widgets that own their drags keep them: `WidgetClass::capturesDrag` (`kCapturesDrag`:
  slider, dial, scroll bar, splitter handle, dock title, colour picker, node graph, curve editor, timeline,
  viewport, `kineticScrollArea`, the game sticks / slots) or a per-press `touchDrag` hook (table scroll bar and
  column edges, header sections, video seek bar); `setCapturesDrag(node, true)` does it for one node. On touch a
  drag over a text / code edit scrolls it; selecting by dragging is a mouse feature. Lists and tables select the row
  on the press, so a pan that starts on a row selects it. One finger drives the UI; there is no pinch.
- Popups are overlays inside the window (`osPopups` is X11 / Win32 / Wayland only). Packaging: see below.

These backends are compiled in CI (`fuse-core-android.yml`, `fuse-core-ios.yml`) and have not been run on a device.

### Platform services (plan §4.13)

One platform-neutral API (`fuse/app/services.hpp`, on `AppWindow` / `NativePlatform`), one implementation per
backend, and an honest `false` / no-op where a platform cannot do it. Everything on `AppWindow` also works headless
(the window is then its own window system), which is what the gates drive.

```cpp
fuse::app::NativePlatform native(app);            // scale 0 = the display's (see below)
auto& w = app.createWindow({.title = "Editor"});
w.setMinimumSize({480, 320});
w.setEventHandler({&state, [](void* u, fuse::app::AppWindow& w, const fuse::app::WindowEvent& e) { /* FocusLost, Moved ... */ }});
w.setCloseGuard({&state, [](void* u, fuse::app::AppWindow&) { return !static_cast<State*>(u)->unsaved; }});
w.setFollowSystemTheme(true);                     // light / dark + accent, now and on change
w.setMenu(menuDesc, {&state, &onMenu});           // macOS: the menu bar; elsewhere: an in-window widgets::menuBar
native.show(w);
w.setFullscreen(true);  w.maximize();  w.minimize();  w.restore();  w.setPosition({100, 80});  w.activate();
native.openFileDialog(w, {.title = "Open", .filter = "*.fuseui"}, {&state, &onPath}); // native, else widgets::fileDialog
fuse::app::openUrl("https://example.org");
```

- **Mouse cursors.** A node asks for a cursor with the `cursor` style property (`auto | default | text | pointer |
  ew-resize | col-resize | ns-resize | row-resize | nwse-resize | nesw-resize | move | crosshair | not-allowed | wait |
  none`; `fuse::ui::Cursor`). It is not inherited: `UiContext::cursor()` walks up from the pressed node (a drag keeps
  its cursor) or the hovered one and takes the first `WidgetClass::cursor(ui, node, pos)` hook or `cursor` style that
  is not `auto`. The default theme sets `text` on line / text / code edits, `pointer` on links and `not-allowed` on
  anything `:disabled`; splitter handles, header section edges and table column edges answer through the hook.
  `setCursorOverride(Cursor::Wait)` is the busy cursor. The backends apply it after each event pump: X11 (themed
  cursors through an optional `libXcursor`, else the core cursor font), Win32 (`WM_SETCURSOR`), macOS (`NSCursor`);
  iOS / Android have none.
- **Clipboard.** `show()` installs the system clipboard as the window's `ClipboardHooks`; also
  `NativePlatform::setClipboardText / clipboardText`. Formats (`ClipboardItem` in `services.hpp`):
  `setClipboard({items})` with one item per format (`kClipboardText`, `kClipboardImage` = `image/png` carrying a
  `fuse::ui::Image` of straight RGBA, or any MIME type with raw bytes; an item with `provide` is made lazily, when it is
  first pasted), `setClipboardImage / clipboardImage`, `clipboardData(mime, bytes)`, `clipboardFormats()`,
  `clipboardOwned()`, and `setPrimarySelectionText / primarySelectionText` (X11 PRIMARY). X11: the `CLIPBOARD`
  selection with `TARGETS`, `UTF8_STRING` / `text/plain`, `image/png`, custom atoms and INCR in both directions (no
  clipboard-manager hand-over when the app exits); Win32: `CF_UNICODETEXT`, `CF_DIBV5` + a `PNG` format, registered
  formats named by the MIME type, lazy items made at once; macOS / iOS / Android: text only.
- **HiDPI.** `NativeOptions::scale`: 0 = the display's scale, 1 = physical pixels, anything else forces it. X11:
  `$FUSE_UI_SCALE`, else XSETTINGS `Xft/DPI` (followed live: when the desktop's DPI changes every window keeps its
  logical size and is re-rendered at the new scale, `WindowEvent::ScaleChanged`), else `Xft.dpi / 96`, else
  `$GDK_SCALE`, else 1 (one scale for the connection). Win32: per-monitor
  DPI awareness, `GetDpiForWindow`, `WM_DPICHANGED`. The OS window is logical size x scale pixels, the frame is
  rendered at that size (`AppWindow::resizePixels`), pointer positions are divided by the scale and OS popup windows
  use it. `NativePlatform::postClick` and `screenOrigin` stay in window / screen pixels.
- **Window management** is on `AppWindow`: `setTitle`, `setMinimumSize / setMaximumSize`, `setMode` (`maximize`,
  `minimize`, `restore`, `setFullscreen`), `setVisible`, `setPosition`, `activate`, the queries `mode()`, `visible()`,
  `position()`, `active()`, and `WindowEvent`s (focus gained / lost, moved, mode changed, shown / hidden, close
  requested / vetoed, theme changed, scale changed). `tryClose()` is the user's close and asks the close guard;
  `requestClose()` stays unconditional. X11: maximise / fullscreen / minimise / activate are EWMH requests, so without
  a window manager the mode stays as it is. iOS / Android: one fullscreen surface, every request answers "cannot".
- **System theme and displays.** `Application::systemTheme()` holds the light / dark preference and accent the backend
  reports (`NativePlatform::systemTheme()` queries now). Win32: registry + change messages; macOS: effective
  appearance + accent, with notification; iOS: trait collection; Android: night mode (the activity needs
  `android:configChanges="uiMode"`); X11: XSETTINGS (`Net/ThemeName`, `Gtk/ApplicationPreferDarkTheme`, with change
  notification while the settings manager that ran at start keeps running; it also sets the cursor theme and size,
  `NativePlatform::cursorThemeSize()`), else `$GTK_THEME`, else `gsettings` once, else Unknown. `AppWindow::applyTheme` / `setFollowSystemTheme` re-colour the default theme with one token sheet
  (`themeTokensCss`, replaced in place on every call; see "Themes"). `NativePlatform::displays()` lists bounds, work area, scale and refresh rate where known.
- **File dialogs.** `NativePlatform::fileDialog` is the modal native one: Win32 `GetOpenFileNameW` /
  `GetSaveFileNameW` / `SHBrowseForFolderW`, macOS `NSOpenPanel` / `NSSavePanel`, Linux / BSD `zenity` or `kdialog`
  when installed; otherwise `Unavailable`. `openFileDialog` tries it and falls back to `widgets::fileDialog`.
- **Menus.** `MenuDesc` (titles, items, shortcuts, submenus, separators, enabled, checkable / checked) builds an
  `NSMenu` tree on macOS and the in-window `widgets::menuBar` elsewhere; activation calls the `MenuHandler` with the
  item id either way, and `AppWindow::menuShortcut` handles shortcuts the UI did not consume. The in-window bar shows
  checkable items as `[x]` / `[ ]` and does not grey out disabled items (they do not fire).
- **Shell.** `openUrl` (xdg-open / `ShellExecuteW` / `NSWorkspace` / `UIApplication` / an `ACTION_VIEW` Intent) and
  `revealFile` (Explorer / Finder select the file; Linux / BSD open its folder; false on mobile).
- Without Vulkan an X11 window now shows the CPU raster (`XPutImage`, `PresenterKind::Software`).
- **Window-less dialogs** (free functions in `native.hpp`, `std::expected<..., PlatformError>`): `openFileDialog`
  (several filters, `multiple`), `saveFileDialog`, `pickFolderDialog`, `colorDialog`, `messageBox`,
  `revealInFileManager`. Win32: `IFileOpenDialog` / `IFileSaveDialog`, `ChooseColorW`, `MessageBoxW`; Linux / BSD:
  `zenity`, else `kdialog` (`messageBox`: zenity only); `Unavailable` without a helper and on macOS / iOS / Android.
  `fuse/app/platform.hpp` and `fuse/app/clipboard.hpp` are compatibility headers over `services.hpp` / `native.hpp`
  for code written against the `claude/magical-bassi-815825` branch (the FUSE-UI editor shell).
- **OS drag and drop** (`NativeOptions::dragAndDrop`, on by default; X11 XDND v5, Win32 OLE; Wayland: in only). In: files or text
  dragged from another application hover and drop on the window's drop targets through `UiContext::dragExternalMove /
  dropExternal` (`DragData::type` is `kDragFiles` with LF-separated local paths in `text`, or `kDragText`); the other
  application is always told "copy". Out (X11 / Win32): a drag started in the UI (`beginDrag`, a drag source) whose data is
  `kDragText` / `kDragFiles` is offered to the application under the pointer once it leaves the app's windows
  (`NativePlatform::externalDragActive()`), and the source gets `DragEnd` with what the other side did
  (`UiContext::dragFinishExternal`). Other data types stay inside the app.
- **OS tool windows** (`NativeOptions::osWindows`, off by default; X11 and Win32, ignored on Wayland). `UiContext::setOsWindows(true)`
  takes overlay nodes with the class `os-window` out of the window's draw list and into `windowSurfaces()`;
  `widgets::dockFloat` gives floating docks that class, so with the option on a floated dock is its own borderless OS
  window that can leave the main window (title drag, drop zones, `dockTo` work as before; keys stay with the main
  window). Off, and on the other platforms, floating docks stay in-window panels. Read-back:
  `toolWindowCount / toolWindow / toolWindowScreenRect / captureToolWindow`.
- **Client-side window chrome** (`fuse/app/chrome.hpp`). `WindowDesc::frame = WindowFrame::Custom` (resizable from
  `resizeBorder` px edges) or `Borderless` removes the OS frame; `titleBar(window, parent, title)` adds a title-bar row
  whose buttons run `AppWindow::command(Minimize | ToggleMaximize | Close)`, and `chromeHitTest(window, pos)` says what
  a point is (caption, an edge or corner, a button, client). X11: `_MOTIF_WM_HINTS`, drags handed to the window
  manager with `_NET_WM_MOVERESIZE`, or done by the backend when there is no window manager; Win32:
  `WM_NCCALCSIZE` / `WM_NCHITTEST`; Wayland: no decoration is asked for and caption / edge presses become
  `xdg_toplevel.move` / `resize`. On macOS / iOS / Android the frame kind is ignored.
- **Saved state.** `AppWindow::saveUiState / restoreUiState` (focus, text-field text, values, check states, scroll
  offsets of nodes with an id) and `saveWindowState / restoreWindowState` (position, size, mode) write to a
  `Settings`; `Application::saveState() / restoreState(bytes)` and `saveStateTo(path) / restoreStateFrom(path)` do it
  for every window plus `StateHooks`. Restore before `NativePlatform::show` so the OS window is created with the saved
  geometry. Android: the state is kept in the activity's saved-instance state (`APP_CMD_SAVE_STATE`) and handed back
  by `show()` (cross-compiled only, never run on a device).
- **Frame pacing.** `AppWindow::setMaxFrameRate(hz)` caps a window whose presenter does not wait for the display
  (`Presenter::vsynced` false): a `Continuous` window sleeps between frames instead of spinning. 0 = unlimited.
- **Synthetic input** for tests: `postClick`, `postPointerMove`, `postPointerButton`, `postClose` (X11 / Win32 through the OS; Wayland inside the process).

#### Wayland (Linux / FreeBSD)

The same `NativePlatform` runs on Wayland; nothing in the application changes. Which window system is used is
decided when the `NativePlatform` is constructed:

1. `NativeOptions::backend` (`NativeBackend::X11` / `Wayland`) when not `Auto`,
2. else `$FUSE_UI_BACKEND` = `x11` | `wayland`,
3. else Wayland when `$WAYLAND_DISPLAY` is set, `libwayland-client.so.0` loads and a compositor with `wl_compositor`,
   `wl_shm` and `xdg_wm_base` answers,
4. else X11 (`$DISPLAY`, which under a Wayland session is XWayland).

A backend forced by 1 or 2 that cannot start leaves the platform unavailable (`available()` false, `lastError()`
says why, windows stay headless); only the automatic choice falls back. `NativePlatform::backend()` reports the one
in use. Every ctest gate other than the two Wayland ones runs with `FUSE_UI_BACKEND=x11`.

```cpp
fuse::app::NativePlatform native(app, {.backend = fuse::app::NativeBackend::Wayland}); // or leave it Auto
if (native.backend() == fuse::app::NativeBackend::Wayland) { /* nativeDisplay() is the wl_display*, nativeWindow(w) the wl_surface* */ }
```

What the Wayland backend does:

- **Windows.** `xdg_toplevel` per `AppWindow`; title, app id (the `Application` name), size limits, maximise,
  fullscreen, restore, minimise, hide / show, close through the close guard. The compositor's configure sets the
  size and the mode (`AppWindow::mode()`, `WindowEvent::ModeChanged`), its `activated` state is `AppWindow::active()`.
  Coming back from maximise / fullscreen restores the size from before. `resizeWindow` sets the size directly (the
  client owns it on Wayland).
- **Present.** With Vulkan: a swapchain on the `wl_surface`. Without: the CPU raster in `wl_shm` buffers (XRGB8888,
  three rotating buffers), throttled by `wl_surface.frame`: while the compositor has not shown the last frame the
  next one is held back and drawn when the callback arrives (or after 100 ms without one, e.g. an occluded window).
- **HiDPI.** `wp_fractional_scale_v1` + `wp_viewporter` when offered (the window's scale is the compositor's
  preferred one, e.g. 1.5; the buffer is logical size x scale and a viewport maps it back), else the integer
  `wl_output` scale. `NativeOptions::scale` or `$FUSE_UI_SCALE` force a factor (fractional ones need
  `wp_viewporter`). A change arrives as `WindowEvent::ScaleChanged`.
- **Input.** `wl_pointer` (buttons, wheel steps, smooth finger scrolling in pixels), `wl_keyboard` with the
  compositor's xkb keymap through libxkbcommon and key repeat at the compositor's rate (without libxkbcommon: a US
  layout table), `wl_touch` into the multi-touch path (`UiContext::touchDown / touchMove / touchUp` with the touch
  id, so taps, pans, flings, pinch and rotate work), `zwp_text_input_v3` pre-edit and commit while a text field has
  focus.
- **Cursors.** `wp_cursor_shape_v1` when offered, else theme cursors through libwayland-cursor (`$XCURSOR_THEME`,
  `$XCURSOR_SIZE`); `Cursor::Hidden` hides it.
- **Clipboard and drops.** `wl_data_device`: text (`text/plain;charset=utf-8`, `UTF8_STRING`, `text/plain`) and the
  formats API (`setClipboard`: images as `image/png`, any MIME type, lazy items; `clipboardFormats`,
  `clipboardData`, `clipboardOwned`); the primary selection through `zwp_primary_selection_v1` where the compositor
  has it (`setPrimarySelectionText` / `primarySelectionText`). A drop of `text/uri-list` or text from another client goes to `UiContext::dragExternalMove / dropExternal`
  (`kDragFiles` with one path per line, or `kDragText`).
- **Decorations.** Server side through `xdg-decoration` when the compositor has it (KDE, sway, labwc). Without it
  (GNOME, weston) a System-frame window gets the client-side chrome when it is shown: its frame becomes
  `WindowFrame::Custom` and `titleBar()` is added as the first child of the root (32 px, inside the window's size;
  the in-window menu bar goes below it). `NativeOptions::clientDecorations = false` turns that off; an app that
  builds its own chrome (`WindowDesc::frame` not `System`) is left alone. Limits: `KNOWN_GAPS.md`.
- **Window chrome.** For `WindowFrame::Custom` / `Borderless` windows and the fallback above: a left press on the
  caption is `xdg_toplevel.move`, on an edge or corner `xdg_toplevel.resize` (the compositor does the drag), a
  caption double click toggles maximise, the edges show resize cursors.
- **Not there**: OS tool windows (`osWindows` is ignored, `toolWindow*` answer 0 / false), drags out of the app
  (`externalDragActive()` is false), XSETTINGS (`cursorThemeSize()` is `$XCURSOR_SIZE` or 24, 0 with
  `wp_cursor_shape_v1`).
- **Popups.** In-window overlays by default; with `NativeOptions::osPopups` each popup is an `xdg_popup` placed by
  an `xdg_positioner` relative to the window (submenus on the menu's popup), grabbed with the press that opened it.
- **Not possible by protocol**, and reported as such: `setPosition` and `activate` return false, `position()` and
  `screenOrigin` stay 0, minimise sends the request but `mode()` never reads `Minimized`, `postClick` and the other
  synthetic input calls are delivered inside the process, `capture()` reads the app's last committed buffer.

The macOS / iOS parts are compiled in CI only; the Win32 parts are cross-compiled with MinGW and have not been run
(that includes all of `App/src/native_platform_win32.inl`: tool windows, chrome, OLE drag and drop, clipboard formats,
dialogs). The Wayland backend has met headless weston and the gate's own compositor only. X11 window-manager paths (`_NET_WM_MOVERESIZE`, decorations off) were exercised without a window manager only.

### Samples

| Sample | Shows | Headless gate |
|---|---|---|
| `hello_app`, `notes_app` | a document with bindings; settings, text editing | `fuse_app_hello_sample`, `fuse_app_notes_sample` |
| `widget_gallery [--page=N]` | every widget of the catalogue on seven tabbed pages (`ui/gallery.fuseui` plus the call-only widgets and the game HUD page), with icon buttons and SVG images, a multi-selection table beside an editable tree table, rich text in real bold / italic and an Edit menu built from an `ActionRegistry`: the manual test bed for a device | `fuse_app_widget_gallery_page0..6` |
| `game_menu [--pad=script]` | main / pause / settings menu on `ui/game_menu.fuseui`: bindings, tabs, sliders, key rebinding, confirm dialog; keyboard, gamepad navigation (`GamepadNav`; left / right change the focused slider or tab), mouse, touch | `fuse_app_game_menu_sample` (a scripted session over all four input paths, every step checked) |

All are `fuse_app_add_executable` targets with `fuse/app/main.hpp` (`int FUSE_APP_MAIN(int, char**)`), load their documents through
`fuse::app::readResource` on Android / iOS, and take `--screenshot=<png>`. `game_menu` has no gamepad backend of its
own: feed your platform's pad into the `GamepadState` in its loop (`--pad=udlrabs.` plays a script).

### Packaging and installing

- **Install / `find_package`.** `cmake --install <build> --prefix <prefix>` installs the static libraries, the public
  headers (`include/fuse/...`) and `lib/cmake/FuseUI/FuseUIConfig.cmake`. A project elsewhere does
  `find_package(FuseUI REQUIRED)` and links `FuseUI::widgets`, `FuseUI::doc`, `FuseUI::app`, `FuseUI::app_native`, ...
  (`FuseUI_TARGETS` lists them; they carry includes, definitions, C++23 and their dependencies). Unless configured with
  `-DFUSE_TEXT_EMBED_FONT=ON`, the default font is still read from the source tree's `vendor/fonts`. Gate
  `fuse_ui_install_consumer` installs, then builds and runs `tests/consumer` against the install tree only.
- **Apple bundles.** `fuse_app_add_executable` makes an iOS `.app` with `App/apple/Info.plist.ios.in` (`UILaunchScreen`,
  orientations; id / versions from `FUSE_APP_BUNDLE_ID_PREFIX`, `FUSE_APP_BUNDLE_VERSION`,
  `FUSE_APP_BUNDLE_SHORT_VERSION`) and, with `-DFUSE_APP_MACOS_BUNDLE=ON`, a macOS `.app` with
  `Info.plist.macos.in` and an icon built from `Source/FUSE/Branding/fuse_256.png` (sips + iconutil).
  `fuse_app_bundle_resources(target BASE dir FILES ...)` copies files into the bundle's `Resources` keeping their
  path under `BASE`; the samples ship `ui/*.fuseui` and the fallback fonts that way. Written on Linux: these rules
  have not been run on macOS yet.
- **Android.** `App/android/` is a Gradle project template (`build.gradle`, `settings.gradle`, the manifest):
  `gradle -PfuseApp=hello_app assembleDebug` builds `lib<app>.so` through `externalNativeBuild` from this CMake project
  and packs `assets/ui` + `assets/fonts`. It has never produced an APK (the machine it was written on has no Android
  SDK): expect to adjust versions. `fuse_app_stage_assets(target OUT dir UI_DIR dir FONTS)` (target `<name>_assets`)
  stages the same assets tree from CMake; gate `fuse_app_stage_assets`.

## Extras (`fuse_ui_extras`)

From the `gui-framework` branch. Link `fuse_ui_extras` (it brings `fuse_ui_widgets` and `fuse_ui_os`). Built by default
on Linux, FreeBSD and Android; `-DFUSE_BUILD_UI_EXTRAS=ON` on Windows and Apple platforms, where it has not been
verified (the sources compile with MinGW-w64; MSVC and Apple builds were not tried, and the Objective-C++ backends are
not built at all).

| Header (`fuse/ui/extras/`) | Namespace | What |
|---|---|---|
| `undo.hpp` | `fuse::ui` | `UndoCommand`, `UndoStack` (merge, macros, clean state, limit), `UndoGroup` |
| `declarative.hpp` | `fuse::ui::decl` | `Property<T>` with tracked bindings, `bindText / bindValue / bindCss / bindVisible`, `Effect`, hierarchical `StateMachine` |
| `desktop.hpp` | `fuse::ui::desktop` | notifications, `openUrl`, printers and print jobs through helper programs (`notify-send`, `xdg-open`, `lp`); each call has a `*Command()` twin returning the argv. `Notifier`: the native `org.freedesktop.Notifications` service over D-Bus (action buttons, `onAction` / `onClosed`, replace, close); `notify()` uses it first and the helper program otherwise |
| `tray.hpp`, `dbus.hpp` | `fuse::ui`, `fuse::ui::dbus` | `TrayIcon` (with `showMessage` / `onMessageClicked`), `GlobalMenuBar`: StatusNotifierItem + dbusmenu over a runtime-loaded libdbus on Linux / BSD, `Shell_NotifyIcon` on Windows (compiled, not run) |
| `pdf.hpp` | `fuse::ui::pdf` | `PdfDocument` (PDF 1.4: vector shapes, standard fonts, images), `renderDrawList` (a draw list as a vector page: rectangles, lines, glyphs, images, vector paths as `m / l / c / h` with fill, even-odd fill, stroke width / cap / join / dashes, path clips; a gradient is written as the mean of its stops; nine-patches and layer effects are not written) |
| `richtext.hpp` | `fuse::ui::rich`, `fuse::ui::widgets` | `RichDocument` (character formats per byte, block formats per paragraph), `RichTextEditor` (headless editing), HTML and Markdown import / export, the `richtextedit` widget (see below) |
| `audio_meter.hpp` | `fuse::ui::widgets` | `audioLevelMeter`: per-channel peak / RMS bars in dBFS with release ballistics, peak hold and a latched clip lamp, fed interleaved float samples (`audioLevelMeterPush`); no audio device needed |
| `gesture.hpp` | `fuse::ui::gesture` | stand-alone `GestureRecognizer` over raw touch points (tap, double tap, long press, pan with offset and velocity, swipe with a direction, pinch with scale and rotation) for input that does not go through `UiContext::touch` |
| `webview.hpp` | `fuse::ui::web` | `WebView` on the system engine loaded at run time (WebKitGTK; WebView2 with `FUSE_UI_WEBVIEW2_DIR`, untested); `available()` is false without one |
| `charts.hpp` | `fuse::ui::charts` | `Chart` data model, `render` to a `DrawList`, `chartView` widget: line, area, bar, scatter, pie / donut, candlestick, box, waterfall, contour, radar, treemap, sankey, gauge, funnel; axes, legend, tooltips, zoom / pan, model binding, decimation (min-max, LTTB), transitions, PNG export; `Chart3D` / `chart3dView` |
| `datavis3d.hpp`, `fuse/ui/mesh3d.hpp` | `fuse::ui::dv3d`, `fuse::ui` | 3D surface / scatter / bars / volume slices projected on the CPU into draw-list paths (BSP ordering, picking); `Mesh3D` + `rasterMesh` CPU mesh rasteriser |

```cpp
#include "fuse/ui/extras/charts.hpp"
namespace ch = fuse::ui::charts;
NodeHandle view = ch::chartView(ui, parent, "sales");
ch::Chart& c = *ch::chartOf(ui, view);
ch::Series s;
s.type = ch::SeriesType::Line;
s.name = "2026";
s.y = {3, 5, 4, 8};
c.series.push_back(std::move(s));
ch::chartChanged(ui, view);
```

**Rich text editor.** One rich-text model: `rich::RichDocument`. `richTextEdit(ui, parent)` edits it (Ctrl+B / I / U,
Tab / Shift+Tab indent, Ctrl+Z / Y, clipboard as plain text, Ctrl+click on a link emits `Submitted`), and the
read-only `richText` view of the widget library renders it: `doc.toRichTextMarkup(px)` writes the view's markup
(`setRichText(ui, view, widgets::richTextMarkup(ui, editor))`), `fromRichTextMarkup` reads it. The editor paints with
the same text stack as the view: the node's font family with the real bold / italic face when the family has one
(`UiContext::fontFor`), the font's underline and strike-through metrics, and it measures every fragment in its own
face. Block formats (alignment incl. justify, bullet / ordered lists with nesting, headings, indent) live in the
document and the editor only: view markup flattens them (list markers become text, headings a size + bold, alignment
is dropped). Undo goes through `UndoStack`: the editor's own, or the application's with
`richEditor(ui, n)->setUndoStack(&appStack)`, so one Edit > Undo covers the document window; commands are named
(Typing, Delete, Format, Paragraph) and a typing run merges into one.

```cpp
#include "fuse/ui/extras/richtext.hpp"
NodeHandle ed = widgets::richTextEdit(ui, parent);
widgets::setRichMarkdown(ui, ed, "# Title\n\nSome **bold** text.");
rich::RichTextEditor& e = *widgets::richEditor(ui, ed);
e.selectAll(); e.setAlign(rich::Align::Center); e.setColor(Color::rgb(0xE0703A));
std::string html = widgets::richHtml(ui, ed);
```

Limits on main: charts and 3D graphs draw through main's `Path` / `fillPath` / `strokePath` (adapter
`Extras/src/path_compat.hpp`), so on the GPU path they are pre-rasterised on the CPU like every other path. The GPU
mesh pass (`ui_vk_mesh`) and the SIMD min-max kernels are not built. `desktop` and the clipboard / file-dialog parts
of `fuse::ui::os` need helper programs and work on POSIX desktops only; `os::runProcess` returns an error on Windows,
iOS and Android. `fuse::ui::desktop::openUrl` exists next to `fuse::app::openUrl` (`services.hpp`); prefer the latter
in applications. `test_extras.cpp` runs the web view gate without WebKitGTK as a skip. Still parked: the branch's
`widgets_extra`, SVG documents (`extras/svg.hpp`; main's `SvgImage` is the SVG reader), system video and audio output
(`extras/media.hpp`), and `extras/i18n.hpp` (its `.po` reader lives on as `l10n::parsePo`, see Localisation).

`<fuse/gui.hpp>` (`Source/FUSE/UI/Public/include`, target `fuse::gui` inside this build) is one umbrella header over
the core, widgets, documents, the application shell and, when built, the Extras (`FUSE_GUI_HAS_EXTRAS`), with the
namespace `fuse::gui`; `Public/examples/gui_quickstart.cpp` is the smallest program on it.

## Game UI (UI7)

```cpp
#include <fuse/ui/game/game_ui.hpp>
namespace g = fuse::ui::game;
widgets::install(ui); g::install(ui);
ui.setCss(ui.root(), "layout", "anchor");                  // game screens position by anchors
g::GameUi hud(ui);                                          // HUD layer: safe area, kill feed, subtitles, dialogue, pools
hud.setSafeArea({48, 27, 48, 27});
NodeHandle hp = g::hudBar(ui, hud.safeArea(), 1.f, "hp");  // damage trail drains in game time
g::setHudBar(ui, hp, 0.55f);
hud.setCamera(viewProj, eye);  u32 m = hud.addMarker({8, 2, -30}, "Beacon");  // edge-clamped, distance-faded
hud.floatText({x, y}, "-42", Color::rgb(0xFF5C5C));  hud.feed("Odin > Raider");  hud.say("Mira", "Keep low.");
hud.clock().paused = true;                                  // game-time HUD freezes; menus and subtitles keep running
// per frame
hud.update(realDt); nav.update(padState, realDt); ui.update(realDt);
```

- `GamepadNav` maps d-pad / stick to spatial `navigate()` with a 0.4 s delay and 0.12 s repeat, A to Enter and B to
  Escape. `InputArbiter` decides per device whether the UI or the game gets input (interactive HUD nodes and
  `.ui-blocker` panels take the pointer, a held press stays with the UI, focused text fields take the keyboard, menu
  mode takes everything). Device prompts (`GameUi::prompt`) swap glyphs on `noteDevice`.
- `UiSurface` is a UI context placed in the world (centre, right / up axes, size). `surfaceRaycast` maps a ray to
  panel pixels, `surfacePointer` feeds it as pointer input and `surfaceRender` re-rasterises on damage.
- `StringTable` holds per-locale `key=value` tables; bound nodes are re-texted on `setLocale` + `refresh`. Lookups
  fall back to the fallback locale (the first one added, or `setFallbackLocale`), then to the key; `tr(key, args)`
  formats the value as an ICU-style message (see Localisation below).
- The sound hook (`setSoundHook`) receives Move / Accept / Back / Open / Close; wiring it to `fuse_audio` and theme
  sound names is the game's job for now.

## Localisation

Two layers exist. `fuse/text/i18n.hpp` in `fuse_text` (see "Fonts, text styles and i18n" below) has the plural
categories, `formatPlural` patterns, `formatNumber` and pseudo-localisation that `StringTable::trPlural` uses.
`fuse_ui_l10n_lib` is the larger library merged from the `claude/magical-bassi-815825` branch: full ICU-style
messages, dates, times, currencies, calendar names, the string-table file format and the extraction tool. Both are
built and gated; folding the first into the second is an open item in
[`archive/PORT_magical_bassi.md`](archive/PORT_magical_bassi.md).

`fuse_ui_l10n_lib` (`fuse/ui/l10n/l10n.hpp`, namespace `fuse::ui::l10n`) has no ICU dependency. Locale tags are
matched case-insensitively, region first (`de-CH`, `pt_PT`, `en-IN`), then the language; digits are always Latin.

```cpp
namespace l10n = fuse::ui::l10n;
l10n::formatMessage("en", "{n, plural, =0 {No items} one {# item} other {# items}}", {{"n", 1000}}); // "1,000 items"
l10n::formatMessage("ru", "{n, plural, one {# файл} few {# файла} many {# файлов} other {# файла}}", {{"n", 22}});
l10n::formatMessage("en", "{g, select, female {She} other {They}} left on {d, date, long}",
                    {{"g", "female"}, {"d", l10n::Date{2024, 3, 5}}});                        // "She left on March 5, 2024"
l10n::formatNumber("de", 1234.5);       // "1.234,5"     formatNumber("hi", 1234567) -> "12,34,567"
l10n::formatPercent("tr", 0.5);         // "%50"
l10n::formatDate("ja", {2024, 3, 5}, l10n::DateStyle::Long); // "2024年3月5日"
l10n::pluralCardinal("ar", l10n::PluralOperands::fromDouble(3)); // Plural::Few
l10n::formatTime("en", {15, 5});        // "3:05 PM"     de "15:05", ko "오후 3:05"; HourCycle::H12 / H23 force a clock
l10n::formatCurrency("de", 1234.5, "EUR"); // "1.234,50 €"  en "€1,234.50", nl "€ 1.234,50", ja JPY "￥1,234"
l10n::formatMonthYear("ru", 2024, 3);   // "март 2024 г."   weekdayName("de", 1, DayWidth::Short) -> "Mo."
l10n::setUiLocale(ui, "de");            // fuse/ui/l10n/ui_locale.hpp: widgets (the calendar) format for "de"
```

- Plural rules (CLDR 44, cardinal and ordinal) with the operands n, i, v, f, t, so `"1.0"` and `1.5` pick the decimal
  categories: en, de, nl, sv, fi, da, nb, et, gl, fr, es, it, pt, pt-PT, ru, uk, pl, cs, sk, ar, he, tr, el, hu, bg,
  hi, bn, fa, ja, zh, ko (and the root rule, other only, for anything else). Ordinals: en, fr, it, sv, uk, hi, bn;
  others are other only. The compact-exponent operands (c / e) are not supported.
- Messages follow ICU MessageFormat: `{name}`, `{n, number[, integer|percent]}`, `{d, date[, short|medium|long]}`,
  `{n, plural, [offset:k] =0 {..} one {..} other {..}}`, `{n, selectordinal, ..}`, `{g, select, .. other {..}}`,
  nesting, `#` (the plural number minus the offset, locale-formatted; literal inside a nested select, as in ICU),
  ICU apostrophe quoting (`''` is `'`; `'{...}'` is literal; a lone `'` is literal). Arguments are text, numbers
  or `Date`; a decimal string (`"1.50"`) keeps its visible fraction digits for plural selection and `#`. A missing
  argument prints as `{name}`; a malformed pattern is an error (`formatMessageOr` returns the pattern instead).
- Numbers: decimal / grouping separators, minimum grouping digits (es, pl, pt-PT: `1234` stays ungrouped), Indian
  grouping (hi, en-IN), minus signs (U+2212 for sv, fi), percent placement; half-even rounding. Dates (gregorian
  short / medium / long and month names): en, en-GB, en-IN, de, fr, es, it, pt, nl, sv, ru, pl, tr, ja, zh, ko, ar;
  other locales use en. Calendar names (same locales, plus pt-PT): wide and short (column-header) weekday names,
  stand-alone month names (ru, pl nominative), the month-year header (CLDR yMMMM: es "marzo de 2024", ru
  "март 2024 г.", ko "2024년 3월") and the first day of the week by region (Sunday: en / en-US / en-IN, pt (BR), ja,
  zh, ko; Saturday: ar; Monday elsewhere, including en-GB and pt-PT). Skeletons and non-Latin digits are not done.
- Time of day (CLDR short / medium time patterns): 12-hour with the locale's day period for en, en-IN, hi
  (`3:05 PM` / `3:05 pm`, narrow no-break space), ko (`오후 3:05`), ar (`3:05 م`); 24-hour elsewhere, zero-padded
  (`09:05`: en-GB, de, fr, it, pt, nl, sv, ru, pl, tr, zh) or not (`9:05`: es, ja, cs, he; fi `15.05`).
  `HourCycle::H12` / `H23` override the clock; `uses24HourClock`, `dayPeriodName`.
- Currencies (`formatCurrency(locale, amount, "EUR")`): the locale's pattern (symbol first: en, en-GB, en-IN, hi,
  tr, ja, zh, ko, and with a no-break space pt, nl, de-AT, de-CH; after the number with a no-break space: de, fr, es,
  it, pt-PT, sv, fi, ru, uk, pl, cs, ar, he), its separators and grouping, the negative form (`-$1.00`; nl
  `€ -1,00`; de-CH `CHF-1.00`), the symbol as the locale writes it (en `$` / `¥`, en-GB `US$`, fr `$US`, it `USD`,
  ja `￥`, zh `¥` for CNY, sv `kr`, pl `zł`, ru `₽`, ...; CLDR root symbols otherwise, an unknown code prints itself)
  with CLDR currency spacing (a letter beside the digits gets a no-break space: `CHF 1’234.50`, `XYZ 1.00`), and the
  currency's fraction digits (JPY / KRW 0, KWD / BHD 3, else 2), rounded half-even. Accounting formats and
  compact / plural currency names are not done.
- Widgets: `UiContext::setLocale(tag, &hooks)` gives widgets a locale and its data through `LocaleHooks` (function
  pointers), so core and widgets link no locale tables; `l10n::setUiLocale(ui, tag)` (header-only
  `fuse/ui/l10n/ui_locale.hpp`) installs the fuse_ui_l10n ones. The calendar then shows the localised month header and
  weekday names and starts its weeks on the locale's first day. Without hooks (or with an empty tag) it stays English,
  Monday first. The date-time edit shows ISO numbers (`2024-03-05 15:05`), so it has no names to localise; its
  calendar popup follows the locale.

`StringTable` (in `fuse_ui_game`) on top of it:

```cpp
StringTable st;
st.add("en", "items={n, plural, one {# item} other {# items}}\ngreet=Hello, {name}!");
st.add("de", "items={n, plural, one {# Gegenstand} other {# Gegenstände}}");
st.setLocale("de");
st.tr("items", {{"n", 3}});        // "3 Gegenstände", formatted with the locale the string came from
st.tr("greet", {{"name", "Ann"}}); // "Hello, Ann!" (fallback locale: the first added, or setFallbackLocale)
st.tr("greet");                    // raw value
st.trPlural("items", 3);           // the fuse/text/i18n.hpp plural patterns ("one {# item} other {# items}")
st.bind(ui, label, "items", {{"n", 3}}); // re-texted on refresh()
```

Table files are one `key=value` per line, `#` comment lines, escapes `\\ \n \t` (and `\=`, a leading `\#` in keys).

### gettext `.po` catalogues

`l10n::parsePo(text)` reads a gettext catalogue into string-table entries, so translations made with gettext tools
(Poedit, Weblate, `msgmerge`) feed the same `StringTable`:

```cpp
auto cat = l10n::parsePo(poText);                       // std::expected<PoCatalog, MessageError>
table.add(cat->language, l10n::poToTable(*cat));        // "Language:" of the header, e.g. "pl"
table.tr("%n file", {{"n", 3}});                        // "3 pliki"
table.tr(l10n::poKey("menu", "Open"));                  // an entry with msgctxt "menu"
```

`msgid` is the key (with a `msgctxt`: `poKey(context, msgid)`), `msgstr` the value. A plural entry becomes one ICU
plural message on the argument `n`: which `msgstr[i]` serves which CLDR category is worked out by evaluating the
catalogue's own `Plural-Forms` expression against the language's CLDR rule; `%n` / `%d` become `#`, ICU syntax
characters in a translation are quoted. Fuzzy and untranslated entries are skipped (counted in the result), a form
left empty falls back to the source text, a malformed file fails with the byte offset of the line. Singular values are
taken as they are (`%1`-style positional arguments are not rewritten; ICU messages use `{0}`).

### `fuse_ui_extract_strings`

```
fuse_ui_extract_strings --out=de.strings [--locale=de] [--source] [--fn=tr,...] [--no-locations] [--drop-obsolete]
                        <files or directories>...
```

Scans `.fuseui` documents (constant `text` except line / text edits, `items`, `a11yName`; strings with a `{`
binding are skipped) and C++ sources (`.cpp .h .hpp ...`) for `tr("...")` calls whose first argument is a string
literal or adjacent literals (escapes, `u8` and raw literals decoded; comments and other strings ignored), and writes
the table sorted by key with `#: file:line` (or `file:node id`) location comments. An existing output file is merged:
translations are kept, untranslated keys are written as `#? key` (ignored by `StringTable`, so lookups fall back),
keys new since the last run are flagged `#, new`, and translations whose key left the sources move to
`#~ key=value` at the end (restored if the key comes back; `--drop-obsolete` removes them). `--source` writes new
keys as `key=key` for the source-language table. A summary and the new / obsolete keys are printed; exit code 1 on
an unreadable input or a broken document. The output is deterministic for the same inputs.

Documents are translated at load time with the same keys: pass a `doc::Translator` (source string -> translation;
empty keeps the source) to `Document::load` / `loadCooked`, e.g. the active `StringTable`:

```cpp
auto doc = doc::Document::load(ui, json, &model, {}, [&st](std::string_view k) { return st.tr(k); });
st.setLocale("fr");
doc->retranslate();   // re-applies the table to every translatable string; returns how many nodes changed
```

Exactly what the extractor collects is translated (constant `text` except line / text edit contents, each `items`
entry, `a11yName`; strings with a `{` binding are not). `reload()` translates the new tree with the same translator;
`setTranslator({})` + `retranslate()` shows the source strings again.

## Data and editor widgets (UI6 Tier C/D)

- `setTableDelegate(table, col, CellDelegate)` paints a column with your own callback; `SortFilterProxy` wraps an `ItemModel` with a filter predicate and a sort column (header clicks sort).
- `gridView(parent, model, cellW, cellH)` is a virtualised icon grid; `gridColumns`, `gridFirstVisibleRow`.
- `propertyGrid(parent, std::span<const PropertyDesc>)` builds grouped rows (bool, int, float, string, enum, colour); read with `propertyValue`, collapse with `setPropertyGroupOpen`.
- `colorPicker`, `colorPickerColor`, `setColorPickerColor`; `calendar`, `calendarDate` (`daysFromCivil`/`civilFromDays` helpers); `chart(parent, ChartKind)` + `chartSetSeries`; `markdownView` + `setMarkdown` (headings, paragraphs, lists, code, quotes).
- Tier D editor widgets: `fontPicker`, `fileDialog`, `nodeGraph`, `curveEditor`, `timeline`, `viewportWidget` (forwards input to your callback, paints your image), `codeEdit` (line numbers, highlighting), `dateTimeEdit` (`value()` = days since 1970, `dateTimeEditMinutes`), `videoWidget` (frame-source stub).
- `fuse_ui_sql::SqliteModel`: `open`/`attach`, `setQuery`, `sortBy`, `refresh`; pass `model()` to `tableView`. Only the visible window of rows is fetched.
- Platform feel (`platform_feel.hpp`): `setHapticsHandler`, `ScrollProfile::desktop()/ios()/android()`, `kineticScrollArea` (drag/fling with rubber band; a drag never clicks).
- Accessibility. The core keeps the tree: `UiContext::snapshotA11y` returns one `A11ySnapshotNode` per accessible node with role (button ... menu, menu item, dialog, tree / tree item, table / row / cell / column header, tab list / tab / tab panel, tool bar, status bar, tooltip, alert), name, value (numeric with range, or `valueText`: a text field's content, a combo's selection, a date), states (focused, checked, disabled, expanded, selected, modal), live region, tree level, position in set and the actions it accepts. Widgets that paint their own items (list, tree, table, grid view, tab bar, dock tabs, header, rich-text links) expose the VISIBLE ones as virtual children (`child` id, stable while the item exists; a 1M-row table is a handful of entries). The item views of `modelview.hpp` do it in one place: a table is a header row of column headers plus rows of cells (selected rows or, with cell selection, selected cells from the `SelectionModel`; check cells are `checked` and toggle on Click; editable cells take SetValue; hidden or scrolled-out columns are left out), a model list is a list of list items, a `treeModelView` is a tree of tree items with level and expanded / collapsed (Expand / Collapse; a tree table's items carry their cells; ids follow the item, not the row), and the in-place editor is an ordinary child named after its column. `UiContext::performA11yAction(node, action, value, child, text)` is the one way in for assistive technology and tests: Focus, Click, Increment / Decrement, SetValue (number or text), Expand / Collapse, Select, ScrollUp / Down / Left / Right, ScrollIntoView; it asks the widget class first (`WidgetClass::a11y` / `a11yAction` are the hooks for your own classes). `setA11yLive(node, Polite | Assertive)` marks a live region (status-bar messages and toasts are marked already). `dumpA11y(out, actions)` prints the tree for gates. Bridge (`fuse/ui/a11y/a11y.hpp`): `Bridge::sync` diffs snapshots, `idOf(node, child)` / `resolve` map 64-bit platform ids, `perform` / `enqueue` / `drain` run actions on the UI thread, `takeAnnouncements` returns what live regions said since the last call. AccessKit needs a Rust toolchain and a prebuilt accesskit-c (the original backend was tested with 0.22.3; the roles, states and actions added since are behind `FUSE_UI_ACCESSKIT_EXTENDED`, default on, and have not been compiled against accesskit-c: build with `-DFUSE_UI_ACCESSKIT_EXTENDED=0` if your accesskit-c lacks a symbol); without AccessKit the bridge still builds. Gates `fuse_ui_a11y_tree_dump`, `fuse_ui_a11y_bridge`, `fuse_ui_a11y_family_*`.
- Designer: `fuse_ui_designer [file.fuseui] [--save=out] [--screenshot=png]`. `DesignDocument` saves exactly what it loaded; keys starting with `x-` are kept for tools and stripped from the live preview.
- `dockSaveLayout(dock)` returns a text layout (`fusedock 1`); `dockRestoreLayout(dock, text)` checks the whole text before applying it and ignores unknown panel ids.

## Model / view (plan §4.11)

Header `fuse/ui/widgets/modelview.hpp` (included by `widgets.hpp`). `tableView`, `modelListView` and `treeModelView`
are one virtualised view class: the `setView*` / `view*` calls work on all three, the selection calls also on
`gridView`. Models, delegates and shared selection models are not owned and must outlive their views.

- **Models.** `ItemModel` is still `rows` + `data` for a flat model. Optional members add typed roles (`role`: Edit,
  Decoration, CheckState, ToolTip, Foreground, Background, TextAlign, User), `flags` (`kItemEditable`,
  `kItemCheckable`, ...), `setData`, a hierarchy (`childCount` / `childAt` / `hasChildren` / `itemData` over opaque
  `ItemId`s, `kRootItem` = the invisible root), lazy children (`canFetchMore` / `fetchMore`) and a `notifier`.
  `StringTreeModel` is the ready-made tree (`add`, `remove`, `setCell`), as `StringTableModel` is for tables.
- **Notifications.** A model with a `ModelNotifier` calls `rowsInserted` / `rowsRemoved` / `rowsMoved` / `dataChanged`
  / `reset` after changing its data; views then keep the selection and the first visible row on the same rows, and
  a `dataChanged` outside the window repaints nothing. Without a notifier, call `tableModelChanged(view)`.
- **Tree.** `treeModelView(parent, &model)`: only expanded nodes are tracked, so a node with a million children costs
  one record; lazy children are fetched when a node opens and as its end scrolls into view. `setTreeRowExpanded`,
  `setTreeItemExpanded`, `treeItemAt(row)`, `treeRowDepth`, `treeRowOf`, `treeCollapseAll`. Keys: arrows, Home / End,
  PageUp / PageDown, Right / Left to expand / collapse / step in / step out, type-ahead. More than one column with a
  header makes it a tree table. The string `treeView` now collapses any line (no 64-row limit).
- **Selection.** `setViewSelectionMode(view, SelectionMode::Single | Multi | Extended | None, SelectionUnit::Rows |
  Cells)`; `viewSelectionModel(view)` gives the `SelectionModel` (ranges, `isSelected`, `selectedRows`, current /
  anchor, `subscribe`). Extended: click, Ctrl+click, Shift+click, Shift+arrows, Ctrl+A, Space. Share one model
  between views of the same flat model with `setViewSelectionModel`. In the default plain single mode `value()` is
  the selected row as before; with a selection model `value()` is the *current* row. `ValueChanged` fires on every
  selection or current change. A tree's selection is in visible rows (`treeItemAt` maps them to ids).
- **Editing.** Cells whose `flags` say `kItemEditable` open an editor on double click, F2 or typing
  (`setViewEditTriggers`); Enter commits through `setData(ItemRole::Edit)`, Escape cancels, Tab / Shift+Tab move to
  the next / previous editable cell. The editor is one child node over the cell. Delegates choose it:
  `lineEditDelegate()` (default), `spinBoxDelegate(&spec)`, `comboBoxDelegate(&items)`, or your own `createEditor` /
  `setEditorData` / `commit` on `CellDelegate`. `kItemCheckable` cells with a CheckState role toggle in place.
  `viewEdit`, `viewEditor`, `viewCommitEdit`, `viewCancelEdit`.
- **Columns and rows.** Columns wider than the view scroll horizontally (Shift + wheel, Left / Right, the bottom bar;
  `setViewScrollX`). `setViewColumnsMovable` (header drag), `moveViewColumn`, `setViewColumnHidden`,
  `resizeViewColumnToContents` (also a double click on a header edge), `setViewFrozenColumns`, `setViewRowHeight`,
  `setViewAlternatingRows`, `setViewSortIndicator`, `viewCellRect`. Cell text is one line; a clipped cell shows its
  text in a tooltip (`setViewToolTips`). `setViewContextMenu(view, fn)` is called on a right press with the cell.
- **Proxies.** `SortFilterProxy`: `sortBy` + `addSortKey` (stable multi-key sort), `setFilterAllColumns`,
  `setFilterWildcard` (`*`, `?`); proxies stack (`SortFilterProxy b(&a.model())`) and rebuild when the source reports
  a change. `TreeFilterProxy` filters a tree and keeps the ancestors of matches.
- **Async.** `AsyncModel(ui, columns, pageRows, maxPages)`: `setSource(fetch, user)`, `setRowCount`. Rows that are
  not cached show a placeholder and are requested once per page; answer with `deliver(ticket, cells)` from any
  thread. The page is committed on the UI thread at the next `update()` and only views showing those rows repaint.

```cpp
widgets::StringTreeModel scene("Name\tKind");
ItemId lights = scene.add(widgets::kRootItem, "Lights\tgroup");
scene.add(lights, "Sun\tdirectional");
NodeHandle tree = widgets::treeModelView(ui, parent, &scene.model());
widgets::setViewSelectionMode(ui, tree, widgets::SelectionMode::Extended);
scene.add(lights, "Fill\tpoint");                       // the view updates through the notifier
```

## Drawing: paths, Painter, SVG, icons

Headers `fuse/ui/path.hpp`, `draw_list.hpp`, `painter.hpp`, `svg.hpp`, `icons.hpp` (all in `fuse_ui_core`).

**Paths.** `Path` has `moveTo / lineTo / quadTo / cubicTo / arcTo / close`, `addRect / addRoundedRect / addEllipse /
addCircle / addArc / addPolyline / addPath`, `Path::fromSvg("M10 10 L...")` (fails with the byte offset on bad data)
and `bounds()`. `Affine` is the 2x3 matrix (`translation`, `scaling`, `rotation`, `*`, `invert`).

**Draw list ops.** `DrawList::fillPath(path, brush, FillRule, xf)`, `strokePath(path, StrokeStyle, brush, xf)`
(width, butt / round / square caps, miter / round / bevel joins, miter limit, dashes + offset),
`gradientRect(rect, Gradient, radii)` (N-stop `Gradient::linear / radial / conic`, pad / repeat / reflect),
`ninePatch(image, dst, insets, src)` / `imageRect(image, dst, src)`, `pushClipPath(path)`,
`pushClipRounded(rect, radii)` (per-corner), `pushLayer(opacity, bounds)`; all end with `popClip()`. Paths are stored
unflattened and flattened by the rasteriser in device pixels (0.05 px), so `setRenderScale` / `DrawList::scale` keep
them sharp. Coverage is exact-area antialiasing; small paths are cached by shape (`pathCacheStats()`), so an icon
drawn again at a whole-pixel position costs a lookup (~0.005 ms at 24 px, ~0.03 ms from scratch).

**Painter** (QPainter equivalent) records into a draw list with a state stack:

```cpp
static void paint(const PaintContext& pc, DrawList& dl) {   // a widget class's paint callback
    Painter p(pc, dl);                                       // node opacity + render scale
    p.translate(pc.content.x, pc.content.y);
    p.setBrush(Gradient::linear({0, 0}, {120, 0}, stops));
    p.setPen(Color::rgb(0xFFFFFF), 1.5f);
    p.drawRoundedRect({0, 0, 120, 40}, 8);
    p.save();
    p.rotate(0.3f);
    p.clipPath(star);                                        // closed by restore()
    p.drawEllipse({60, 20}, 30, 12);
    p.restore();
    drawIcon(p, Icon::Search, {4, 12, 16, 16}, pc.style.color);
    p.drawText("label", {26, 0, 90, 40}, pc.style.color, 13);
}
```

`save / restore`, `translate / scale / rotate / transform / setTransform`, `setOpacity`, `setBrush` (colour or
gradient), `setPen`, `setFillRule`, `clipRect / clipRoundedRect / clipPath`, `beginLayer(opacity) / endLayer`,
`drawPath / drawRect / drawRoundedRect / drawEllipse / drawCircle / drawLine / drawPolyline / drawPolygon / drawArc`,
`fillPath / strokePath / fillRect` with an explicit brush, `drawText`, `drawImage`, `drawNinePatch`. It never
allocates once warm. Limits: text follows translation and scale but is drawn upright under rotation / skew (glyphs
come from the atlas); `drawImage` / `drawNinePatch` return `false` and draw nothing under rotation, skew or mirroring;
a brush set through `setBrush(Gradient)` keeps 16 stops.

**SVG.** `SvgImage::parse(xml)` -> `std::expected<SvgImage, SvgError>`; `image.render(painter, rect, currentColor)`.
Subset: `svg` (viewBox), `g`, `defs`, transforms, `path / rect / circle / ellipse / line / polyline / polygon`,
`fill / stroke` (names, `#hex`, `rgb()`, `none`, `currentColor`, `url(#gradient)`), `fill-rule`, opacities, stroke
width / cap / join / miter limit / dashes, `style=""`, `linearGradient / radialGradient`. Not supported (counted in
`unsupportedElements()`): text, `use`, `image`, clip paths, masks, filters, CSS sheets. Malformed XML or path data is
an error, never a crash.

**Icons.** `Icon::{ChevronLeft, ChevronRight, ChevronUp, ChevronDown, Close, Check, Search, Folder, File, Save, Undo,
Redo, Plus, Minus, Settings, Warning, Info, Error, Menu}`; `iconImage(icon)`, `iconSvg(icon)`, `iconName` /
`findIcon`, `drawIcon(painter, icon, box, color)`.

**Widgets** (`widgets.hpp`): `svgCreate(xml)` -> `SvgHandle`, `svgImage(ui, parent, handle, w, h)` (drawn in the
node's `color`), `setIcon(ui, button, Icon::Save)` / `setIcon(ui, button, handle, size)` on buttons and tool buttons,
`iconButton(ui, parent, Icon::Undo)`.

**GPU.** The compute kernel has no path op. `packDrawList` pre-rasterises every vector op (and every clip-path /
layer group as a whole) on the CPU into an image and draws it with the kernel's Image op; the reference rasteriser
composites these ops as 8-bit texels with the same blend, so both paths agree by construction (gate
`fuse_ui_gpu_pack_vector` checks this with a CPU model of the kernel; a real GPU run is `fuse_ui_vk_parity`).
`GpuDrawData::bakedOps / bakedPixels / unsupportedOps` report it; ops beyond 16 M baked pixels per frame are dropped
and counted.

## Styling (CSS subset, D4)

Selectors: type, `.class`, `#id`, `:hover :pressed :focus :focus-visible :checked :disabled`, descendant and `>`
combinators. Tokens are `--name` in `:root` and used with `var(--name[, fallback])`; they are global (scoped tokens
are deferred). Inline (`setCss`) beats everything; later sheets win ties. Layout properties: `layout`
(box/grid/stack/anchor/absolute/flow), `flex-direction`, `flex-wrap`, `gap`, `padding*`, `margin*`,
`width/height/min-*/max-*`, `flex-grow/shrink/basis`, `align-items/self`, `justify-content`,
`grid-template-columns`, `grid-row/column(-span)`, `left/top/right/bottom/center-x/center-y`, `overflow`
(visible/clip/scroll), `display`, `pointer-events`. Paint: `background` (colour or `linear-gradient`),
`background-end` (gradient end; widgets use it as the accent), `border`, `border-width/color`, `border-*-radius`,
`box-shadow`, `shadow-x/y/blur/color`, `opacity`, `color`, `font-size`, `text-align`. Selectors also accept
`:active`, `:selected`, `:drag-over`. Motion and direction properties (`transition`, `enter-animation`,
`exit-animation`, `enter-duration`, `exit-duration`, `direction`, `padding-start`, `padding-end`) are described under
"Interaction" below. `cursor` (see Platform services) is the mouse cursor a node asks for.

### Themes: dark, light, high contrast

The widget theme (`Widgets/src/theme.cpp`) is one rule body over `:root` colour tokens with three token sets:

```cpp
widgets::install(ui);                                   // the dark theme, in the context's theme slot
widgets::setTheme(ui, widgets::ThemeVariant::Light);    // replaces the slot in place (no sheet is stacked)
widgets::setTheme(ui, {.variant = widgets::ThemeVariant::Dark, .hasAccent = true, .accent = Color::rgb(0xCC3366)});
widgets::setTheme(ui, {.variant = widgets::ThemeVariant::HighContrast, .contrast = palette}); // HighContrastPalette
std::string_view css = widgets::themeCss(widgets::ThemeVariant::Light);   // the sheet text; lightThemeCss(), highContrastThemeCss(p)
std::string tokens = widgets::themeTokensCss({.variant = widgets::ThemeVariant::Light}); // only :root: an override sheet
```

- **Tokens only.** Every colour of the theme rules and of the widget paint code is a token (`--bg`, `--surface`,
  `--text`, `--accent`, ... and paint tokens such as `--track`, `--knob`, `--canvas`, `--gutter-text`, `--selection`,
  `--code-keyword`; the table with its dark / light / high-contrast values is `kTokens` in `theme.cpp`,
  `widgets::themeTokenCount / themeTokenName / themeTokenColor` read it). Paint functions call
  `themed(pc, tok::name, 0xDARK)` (`widgets_internal.hpp`), which is `UiContext::themeColor(hashId("--name"),
  fallback)`: the last sheet that defines the token wins, cached per sheet version, no allocation. An application
  re-colours any widget part with a later `:root { --track: #...; }` sheet. New widget paint code follows the same rule
  (`fuse_ui_theme_paint_literals` scans the sources): add a token to `kTokens` with its three values and the
  `tok::` hash, or mark a data-colour line `// theme-exempt: why`. Not themed on purpose: data colours (the
  colour picker's gradients and markers, flame / pin / clip palettes and the text over them, images, the items of a
  `GraphicsScene`: `GraphicsItem::fill / stroke` are the application's). The ported widgets (`widgets/extra.hpp`,
  `widgets/edit_options.hpp`, dock splits) follow the rule: `--lcd-text` / `--lcd-bg` (LCD number), `--workspace`
  (MDI area), `--scene-bg` and `--item-focus` (graphics view and its selection outline), `--placeholder`
  (placeholder text of a line edit, "None" of a key sequence edit), `--text-disabled` (a disabled tab),
  `--focus-ring` (rings drawn by paint code: the editable combo box around its field).
- **Sheets by handle.** `ui.addStyleSheetTracked(sheet)` returns a handle for `ui.replaceStyleSheet(handle, sheet)`
  (same cascade position) and `ui.removeStyleSheet(handle)`; `ui.setThemeStyleSheet(css)` is the theme slot
  (`StyleEngine::addSheetTracked / replaceSheet / removeSheet / hasSheet / sheetCount / tokenColor`, in
  `Core/src/style_sheets.cpp`). `AppWindow::applyTheme` keeps one override sheet and replaces it, and takes its tokens
  from `widgets::themeTokensCss` (`SystemTheme::highContrast` selects the high-contrast set; X11 sets it from the
  desktop's theme name - XSETTINGS `Net/ThemeName` live, `$GTK_THEME` at start, e.g. "HighContrast" - the other
  backends do not query it). The client-side title bar (`fuse::app::titleBar`, also the Wayland client-side
  decoration) is styled from tokens only (`--surface`, `--surface-hi`, `--window-close`, `--on-accent`), so a theme
  switch restyles it; `fuse_ui_theme_paint_literals` also scans `App/src` for colour literals in code and style
  strings.
- **Focus rings.** Every focusable class shows keyboard focus (`:focus-visible`): a 1px accent border (painted over
  the box, no layout change) for the classes that had none, the heading colour on an accent (primary) button
  (`--primary-ring`), 2px in the hot-light colour in high contrast. `fuse_ui_theme_focus_ring` renders every
  focusable class of the registry with and without focus, and separately the composites and option-driven paint
  paths the class loop cannot reach (editable combo box, line edit with options / a completer, spin box and tab bar
  with options, key sequence edit, graphics view, tool box header, dialog button box, the handle of a dock split).
- **Contrast.** Light and dark: text against its background is at least 4.5:1, high contrast at least 7:1
  (`fuse_ui_theme_contrast`, from computed styles and the paint-code token pairs). The dark theme has five known
  pairs below 4.5 that are frozen by its goldens: white on `--accent` 3.33 (primary button, check marks, a latched
  on-screen keyboard key), on `--danger` 3.91 and `--warning` 2.27 (message box badges), code editor line numbers
  3.00, and placeholder text (the text colour at half alpha: 4.47 in a field, 3.71 in an editable combo box).

### Fonts, text styles and i18n

Text properties: `font-weight` (`normal` / `bold` / 1..1000), `font-style` (`normal` / `italic`), `font-family` (comma
list), `letter-spacing`, `line-height` (`normal`, a multiplier, px or %) are inherited; `text-decoration` (`underline`,
`line-through`, both) is not. Labels, buttons and every widget that paints through `paintString` /
`paintTextLayout` / `measureString` follow them.

```cpp
#include <fuse/text/font_family.hpp>
#include <fuse/text/i18n.hpp>

ui.setCss(title, "font-weight", "bold");                  // synthesised from the context's regular face
ui.setCss(note, "font-family", "\"DejaVu Sans\", sans-serif"); // an installed family, else the context's font
fuse::text::FontFamily brand("Brand");                    // an application family with real faces
brand.add(*fuse::text::Font::fromFile("Brand-Regular.ttf"));
brand.add(*fuse::text::Font::fromFile("Brand-Bold.ttf")); // weight / slant read from the file (OS/2, head)
ui.addFontFamily(brand);                                  // font-family: Brand now uses the real bold
for (const auto& f : fuse::text::systemFonts()) { /* family, style, path, faceIndex, weight, italic, monospace */ }
auto mono = fuse::text::Font::fromSystem("DejaVu Sans Mono", 700, false);

fuse::text::formatPlural("one {# file} other {# files}", "ru", 5); // CLDR category of the locale, '#' = the number
fuse::text::formatNumber("de", 1234567.891, 2);            // "1.234.567,89"
fuse::text::pseudoLocalize("Save {name}");                 // "[Šávé {name} ··]"
```

- A family answers a (weight, italic) request with its nearest real face (CSS weight matching). Without one it
  synthesises: bold smears the rasterised coverage sideways by a fraction of the em (and widens advances), italic shears
  the outline before rasterisation. Both happen where glyphs enter the atlas, so the CPU rasteriser and the GPU glyph
  path draw the same texels, at the render scale. Fallback-chain faces (other scripts, emoji) are not restyled.
- `font-family` names are matched case-insensitively: families registered with `addFontFamily`, then installed
  fonts, then the context's font. `sans-serif` / `system-ui` are the context's font; `serif` / `monospace` pick a
  well-known installed family. `setSystemFontLookup(false)` keeps a context to its own and registered fonts.
- `systemFonts()` scans the platform font directories once (Linux / BSD: XDG data dirs, `~/.fonts`,
  `/usr/share/fonts`, `/usr/local/share/fonts`; macOS / iOS: `/System/Library/Fonts`, `/Library/Fonts`,
  `~/Library/Fonts`; Android: `/system/fonts`; Windows: `%WINDIR%\Fonts`) and reads each file's name / OS/2 tables
  itself: `.ttf`, `.otf` (glyf or CFF) and `.ttc` collections. It is empty, not an error, on a machine without fonts.
  `setSystemFontDirectories` points it elsewhere (a font folder shipped with the app, tests).
- `fontPicker` lists the bundled and generic families followed by the installed ones and previews the chosen face
  with its Bold / Italic boxes. `richText` draws `<b>` / `<i>` in the family's bold / italic face; `markdownView` does
  the same for `**bold**` / `*italic*`, and its headings are bold.
- `letter-spacing` and `line-height` apply to a node's own text (labels, text blocks); `letter-spacing` also to
  strings painted with `paintString`. Text edits ignore both.
- Plural rules cover the common CLDR families (one/other, French, East and South Slavic, Polish, Czech, Slovenian,
  Baltic, Romanian, Arabic, Hebrew, Welsh, Irish, and the no-plural languages); number formatting has a built-in
  separator table and always writes Western digits. `StringTable::trPlural(key, n)` applies both to an entry.
- Not there: variable-font axes (a variable font is its default instance), CFF2 and bitmap-only fonts, dates and
  currencies, a string-extraction tool, cooked `.fusefont` files (`FUSE_UI_TEXT=COOKED` still builds MINIMAL).

## Interaction: drag and drop, gestures, actions, motion, RTL

All of this is in `fuse_ui_core` (`ui.hpp`, `actions.hpp`, `style.hpp`); the widget glue is
`fuse/ui/widgets/interaction.hpp` (included by `widgets.hpp`).

**Drag and drop.** A drag carries a `DragData` (`type` = `dragType("application/x-thing")`, two inline `u64`, an
optional pointer and text). Sources: `ui.setDragSource(node, fn)` (or `WidgetClass::dragStart`) is asked when a press
moves past `GestureConfig::dragThreshold`, or call `ui.beginDrag(source, data, image)` yourself. Targets:
`ui.setDropTypes(node, {types...})`, `ui.setDropFilter(node, fn)` or `WidgetClass::acceptsDrop`. The target gets
`DragEnter / DragMove / DragLeave / Drop` events, has the `:drag-over` style state, and may call
`ui.setDropAction(DropAction::None)` to refuse or `ui.setDropIndicator(rect)` to show where the data lands; the
source gets `DragEnd` with the result (`Move`, `Copy` with Ctrl, `None`). Escape cancels. The ghost (a chip with the
text, a translucent copy of a node, or your draw callback) is painted over everything in the draw list. Touch: a
swipe on a drag source inside something scrollable still pans; a long press picks it up. Platform layers inject OS
file drops with `ui.dropExternal(pos, data)` (`kDragFiles`, paths in `text`), `dragExternalMove` / `dragExternalLeave`
for hover feedback; no backend calls them yet.

```cpp
w::setListDragDrop(ui, list, w::kViewDragDrop);       // reorder rows, drag them to other views, accept theirs
w::setTableDragDrop(ui, table, w::kViewDragReorder);  // model views report: RowMoved {value = from, value2 = to}
ui.connect(table, SignalKind::RowMoved, [](UiContext&, NodeHandle, const SignalArgs& a) { /* move the model row */ });
```

A string `listView` edits its own items; `tableView` / `modelListView` / `gridView` emit `RowMoved`, `Dropped` and
`DragEnded` and leave the model to the application.

**Gestures.** `Event::clicks` counts presses; the second click of a double click sends `DoubleClick` after its
`Click` (unconsumed: `SignalKind::DoubleClicked`), and list / table / grid views treat it as Enter (`Submitted`).
Touch: `GestureTap`, `GestureDoubleTap`, `GestureLongPress` (0.5 s within the slop; timer in the context's time, so
call `update(dt)`); with pointer ids (`touchDown(id, p)` ...) two fingers send `GesturePinch` (`scale`, centre in
`pos`) and `GestureRotate` (`rotation` in radians) with `gesturePhase` Begin / Update / End. The single-finger pan /
fling is unchanged. Listen with a `SignalKind::Event` slot or in a widget class's `onEvent`. No backend feeds pointer
ids yet (Android and iOS pass the primary touch).

**Actions and shortcuts.** `ActionRegistry reg(ui);` then
`reg.add({.id = "file.save", .text = "Save", .shortcut = primaryShortcut(Key::S)}, callback)`. Shortcuts fire on a
KeyDown the focused widget did not consume: Subtree-scoped actions while the focus is inside their node, Window
actions otherwise, none of the window's actions while a modal scope is open, and plain typing keys never while a
text field has the focus. `reg.bind(action, button)` keeps text / enabled / checked in sync and triggers on click;
`w::actionMenuBarMenu(ui, reg, bar, "File", reg.addList({...}))` and `w::actionPopupMenu` build menus that show the
shortcut right-aligned. `parseShortcut("Ctrl+Shift+S")` / `shortcutToString`, `reg.rebind(a, s)` (returns the
conflicting action unless told to steal), `saveShortcuts()` / `loadShortcuts()`. `primaryModifier()` is Cmd on Apple
platforms and Ctrl elsewhere. The registry does not use fuse_core's `ActionMap`; ids and shortcut strings are what an
adapter needs.

**Motion.** `transition: background 0.15s ease-out, border-radius 0.1s` (or `all`) fades animatable properties when
a node's style changes; `enter-animation` / `exit-animation: fade | slide-up | slide-down` with `enter-duration` /
`exit-duration` play when a node first shows / is hidden (popups stay alive for their exit time).
`ui.animateProperty(node, Prop::Width, PropValue::ofLength(Length::px(240)), AnimSpec::springy())` tweens or springs
a style property and leaves the end value inline; `ui.animateKeyframes(...)` runs up to 6 keys. Animated nodes use a
scratch slot of the style table, so nothing is interned or allocated per frame and an idle context sleeps.
`ui.setReducedMotion(true)` turns all of it (and `animate()`) into jumps. The default theme declares no motion; opt
in per node with the classes `.fx` (hover / focus colours fade), `.fx-fade`, `.fx-slide`, or for every popup with a
rule such as `popup { enter-animation: fade; exit-animation: fade; }`. Descendants inherit the cascade's final value
of `color`, not the in-flight one: give them their own `transition` if they should fade too.

**RTL.** `direction: rtl` (inherited; `ltr` switches a subtree back) mirrors Box layouts along x, swaps `left` /
`right` of Anchor children, makes `padding-start` / `padding-end` the right / left padding and flips `text-align:
start / end` (`left` / `right` are aliases of start / end and flip too). Overlay popups keep their screen position.
Widgets that paint their own content (lists, sliders, check marks) are not mirrored inside.

**Hot reload.** `doc::DocumentFileWatcher watch(path);` then `watch.poll(document)` each frame (one `stat`) reloads a
`.fuseui` file when it changes; nodes with an id keep their handle, focus, scroll, value and edit text
(`Document::reload`). Style sheets reload the same way: `doc::StyleSheetFileWatcher theme(path); theme.poll(ui);` adds
the sheet at the first poll and replaces it in place when the file changes (same cascade position, the sheet list does
not grow); a sheet that does not parse keeps the last good one and reports `file:line: message`.

## Immediate mode

`fuse::ui::immediate::ImmediateUi` has the same API and layout rules as Relight's `overlay::Ui` (`begin`, `text`,
`button`, `textField`, `sameLine`, …) but draws real glyphs into a FUSE UI draw list. `fuse_ui_relight_equivalence`
checks it against the overlay under one scripted event stream.

## Gates

| Gate | Checks |
|---|---|
| `fuse_ui_layout_conformance` | every layout model, scrolling, `display:none`, measure cache and layout boundaries |
| `fuse_ui_style_cascade` | specificity, order, inline, inheritance, pseudo-classes, combinators, tokens, diagnostics |
| `fuse_ui_event_routing` | capture/target/bubble, stop, click semantics, pointer capture, hover, disabled, modal, `post()` |
| `fuse_ui_focus_order` | tab order, tab index, skipping, wrap, focus-visible, keyboard activation, spatial nav, modal trap |
| `fuse_ui_cpu_raster_golden` | rasteriser primitives and the sample tree against `tests/golden/sample.png` (`sample_full.png` in FULL) |
| `fuse_ui_zero_alloc_steady` | zero heap allocations over 50 interactive frames after warm-up |
| `fuse_ui_bench_10k` | 10k nodes, one dirty label: ≤ 0.5 ms median (Release; 1.0 ms in FULL) |
| `fuse_ui_single_thread_parity` | direct vs posted-from-threads mutations give identical layout, draw list and pixels |
| `fuse_ui_a11y_tree_dump` | roles, names, states, values |
| `fuse_text_font_styles` | face description, synthetic bold / oblique at pixel level, atlas keys, `FontFamily` matching (real face preferred), the font scanner over a temp directory (collection, corrupt files, no fonts), CSS font / decoration / spacing properties, render scale 2, `tests/golden/text_styles.png` (`text_styles_full.png` in FULL) |
| `fuse_text_i18n` | CLDR plural categories per rule family, plural patterns, number formatting per locale, pseudo-localisation, `StringTable::trPlural` |
| `fuse_ui_l10n` | `fuse_ui_l10n_lib`: CLDR plural samples (cardinal / ordinal), ICU-style messages, numbers, dates, calendar names, times, currencies, the calendar widget under `setUiLocale`, `StringTable` fallback and message formatting, `fuse_ui_extract_strings` (extraction, merge, obsolete keys), load-time document translation and `retranslate()` |
| `fuse_ui_script_bind` | the whole Lua `ui` table: every function, errors, signals, handler lifetime (released with the node), steady frames |
| `fuse_ui_drag_drop` | drag threshold, target by type / filter / modal scope, event order, `:drag-over`, Move / Copy / refuse / Escape, ghost, external drops, touch long-press drag vs pan, list reorder and list / table / grid row drags |
| `fuse_ui_gestures` | double click (time, distance, third click), list activation, tap / double tap / long press, multi-touch ids, pinch and rotate phases |
| `fuse_ui_actions` | shortcut text round trip, scope rules (window, subtree, modal, text fields, disabled), bound widgets, action menus, rebinding conflicts, save / load |
| `fuse_ui_style_motion` | `transition` parsing, fades in context time, idle afterwards, zero allocation / no style-table growth, tween / spring / keyframes, popup enter / exit, reduced motion, static default theme |
| `fuse_ui_rtl_layout` | layout dumps of one tree in `ltr` and `rtl`, logical padding, text alignment, popups unmoved |
| `fuse_ui_hot_reload_file` | file-time poll reloads a `.fuseui` file keeping handles, focus, edit text, value and scroll; broken / missing file |
| `fuse_ui_hot_reload_style` | a style sheet file reloads in place (no growth, same cascade position, theme tokens follow), a broken edit keeps the last good sheet, deleted file |
| `fuse_ui_theme_sheet_handles` | sheets by handle: replace keeps the cascade position, remove, dead handles, the theme slot never stacks sheets, token colours follow the newest sheet, accent / custom high-contrast palette |
| `fuse_ui_theme_token_only` | every variant defines every token as a colour, no literal colour in the theme rules, dark and light share the rule text, paint code follows a token override, fallback literals without the theme |
| `fuse_ui_theme_paint_literals` | scans `Widgets/src/*.cpp`: every `Color::rgb(0x...)` is the fallback of a theme token (`themed` / `themedA`) or on a line marked `theme-exempt` (data colours); a new widget with a hard-coded colour fails here |
| `fuse_ui_theme_contrast` | WCAG contrast of text vs background per widget (computed styles of a gallery + paint-code token pairs): ≥ 4.5 light / dark, ≥ 7 high contrast (also with a white custom palette); the known dark exceptions are pinned |
| `fuse_ui_theme_focus_ring` | each of the 32 focusable classes renders differently with `:focus-visible`, in all three variants |
| `fuse_ui_theme_golden_{light,contrast}_<scene>` | the widget golden scenes (containers, text, dialogs, wizard, mainwindow, spinmenu, controls, editors; not the game HUD, which keeps its own colours) in the light and high-contrast themes; MINIMAL text profile only |
| `fuse_ui_widget_golden_{controls,editors}` | Tier A controls in their states with one keyboard-focused, and the token-painted editors (code edit, node graph, timeline, curve editor, calendar, markdown); MINIMAL only |
| `fuse_app_theme_sheets` | `applyTheme` keeps one override sheet over 25 system theme changes, uses the widget library's tokens, high contrast outlines controls and gives a 2px hot-light focus ring |
| `fuse_ui_bench_hud` (`perf`) | a 293-node game HUD animated as in play: UI CPU per frame median ≤ 0.5 ms (measures ~0.10 ms) |
| `fuse_app_binary_size` (`perf`) | stripped `hello_app` within baseline + 10 % (Linux x86-64 GCC Release MINIMAL standalone: 2,130,000 B; other toolchains report and skip until `-DFUSE_APP_BINARY_BUDGET` is set) |
| `fuse_project_schema2`, `fuse_project_schema2_native` (in tree) | schema 1 files unchanged, schema 2 round trip and validation, the Application template, an app project through `ProjectHost` (only `AppPresent` comes up, first frame ≤ 150 ms headless), `ui.theme`; with the engine game side also the deferred hybrid game side |
| `fuse_ui_interaction_golden` | action menu with shortcut texts and a list row drag (ghost, insertion line) against `tests/golden/interaction.png` (MINIMAL profile only) |
| `fuse_ui_mv_notify`, `_selection`, `_tree`, `_editing`, `_view`, `_proxy`, `_async`, `_dir_model` | model / view: notifications, selection modes, tree view, in-place editing, column features, proxies, async model, a lazy `std::filesystem` model |
| `fuse_ui_mv_selection_1m`, `fuse_ui_mv_tree_1m` | range selection over 1M rows allocates nothing per row; a lazy tree with > 1M nodes scrolls with a constant node count and bounded model reads |
| `fuse_ui_mv_golden_tree`, `fuse_ui_mv_golden_table_edit` | tree table and multi-selection + open editor against `tests/golden/modelview_*.png` |
| `fuse_ui_paths` | path fill / stroke coverage (areas of known shapes, fill rules, caps, joins, miter limit, dashes), N-stop gradients, clip path, layers, 9-slice, `translate` / `scale` of every vector op, coverage cache, bad geometry |
| `fuse_ui_painter` | Painter state stack, transforms, pens under non-uniform scale, clips, layers, text and image limits, a widget painting at render scale 1 and 2 |
| `fuse_ui_svg` | SVG subset parse + render, malformed and fuzzed documents, the built-in icons, `svgimage` and button icons |
| `fuse_ui_vector_steady` | zero allocations with vector ops (update + CPU raster), no vector ops in a classic tree, icon raster budget (cached < 0.02 ms, uncached < 0.5 ms at 24 px), large-path cost |
| `fuse_ui_vector_golden_{paths,icons}` | `tests/golden/vector_paths.png`, `vector_icons.png` |
| `fuse_ui_gpu_pack_vector` | vector ops packed as kernel Image commands reproduce the CPU frame exactly (CPU model of the kernel's Image op), statistics, over-budget drop |

| `fuse_app_idle_zero_frames`, `fuse_app_multiwindow`, `fuse_app_hello_startup`, `fuse_app_settings` | UI5 loop, windows, startup ≤ 150 ms, settings / command line / single instance |
| `fuse_app_native_window` | real X11 / Win32 window, Vulkan GPU-draw and CPU-upload presenters vs the CPU raster (skips 77 without a display or device) |
| `fuse_app_wayland` | Wayland backend: protocol tables against libwayland-client's, xkb keymap vs the fallback table, backend selection, wheel / uri-list maths, clean failure without a compositor (always); then under a headless weston it starts itself (output scale 1 and 2): `wl_shm` buffer == CPU raster, injected click / key, frame throttle, resize, title, size limits, maximise / fullscreen / restore, hide / show, close guard, fractional 1.5 through a viewport, `xdg_popup` menus, a child tool toplevel, the client-side chrome fallback (weston has no xdg-decoration: title bar in the frame shown, caption / edge presses taken from the UI, maximise and close from its buttons), the Vulkan presenter when the build has one (skips 77 without libwayland-client or weston) |
| `fuse_app_wayland_seat` | the seat against an in-process compositor on libwayland-server: real `wl_pointer` / `wl_keyboard` (keymap, Shift, repeat) / `wl_touch` (tap, pan, cancel) events, wheel steps and finger scrolling, cursor shapes, clipboard both ways (text; `setClipboard` with text + a lazy custom type + an image, pasted by "another client" as PNG; another client's formats read), the primary selection, synthetic pointer input, a `text/uri-list` drop, text-input-v3 pre-edit / commit, fractional scale, server-side decoration, compositor-sent maximise / restore, client-side chrome (a compositor that answers "client side": the fallback title bar, real presses on an edge / corner / the caption arriving as `xdg_toplevel.resize` / `move` with the press serial, double click = maximise, close button; a `Borderless` window asks for no decoration), `postClose`, compositor exit (skips 77 without libwayland-server / -client / libxkbcommon) |
| `fuse_app_cursor_resolution`, `fuse_app_menu_description`, `fuse_app_dpi_scale`, `fuse_app_theme_restyle`, `fuse_app_window_state`, `fuse_app_file_dialog_fallback` | platform services, headless: cursor from styles / hooks / hover / drag, `MenuDesc` -> in-window menu bar, scale detection maths and pixel mapping, theme change -> restyle, window state API + events + close guard, toolkit file-dialog fallback and helper command lines |
| `fuse_app_native_services` | the same on real X11 windows under Xvfb: scale-2 window and popup vs the CPU raster, clicks in window pixels, applied cursors, CLIPBOARD between two connections, position / show / hide / focus / size limits, displays, `zenity` / `xdg-open` helpers (skips 77 without a display) |
| `fuse_app_saved_state`, `fuse_app_window_chrome`, `fuse_app_dialog_helpers`, `fuse_app_clipboard_png`, `fuse_app_frame_pacing`, `fuse_app_tool_surfaces` | the platform layer, headless (`tests/test_app_platform.cpp`): UI state + window geometry through `saveState` / `restoreState` and a state file; `WindowFrame` / `titleBar` / `chromeHitTest` regions and title-bar commands; zenity / kdialog command lines, colour / path / uri-list / XSETTINGS parsers; the `image/png` clipboard codec; `setMaxFrameRate` on a virtual clock; `setOsWindows` surfaces of floating docks and `dragFinishExternal` |
| `fuse_app_native_services` (sections added with the platform-layer port) | on X11 under Xvfb: window-less dialogs through fake helpers; clipboard image (lazy, PNG), a 600 kB custom MIME type (INCR) and PRIMARY between two connections; XDND text and file drags from a UI drag source in one connection to a drop target in another (accept, refuse, leave, in-app-only types); a floating dock in its own OS window (read-back equals the CPU raster, click, title drag, follows the main window); caption drag, edge / corner resize within the size limits and close on a `WindowFrame::Custom` window; a window restored from a state file at its saved position and size; XSETTINGS DPI / theme / cursor size read and followed live, with the gate as settings manager |
| `fuse_ui_extras_notify` | `desktop::Notifier` and `TrayIcon::showMessage` against a private `dbus-daemon` with an in-process `org.freedesktop.Notifications` service: arguments, actions, urgency hint, replace, `ActionInvoked` / `NotificationClosed` callbacks, close, "unsupported" without a service |
| `fuse_ui_game_hud_golden` | HUD demo scene against `tests/golden/hud.png` plus trail / marker / pool state |
| `fuse_ui_gamepad_nav` | navigation with delay + repeat, A / B, radial menu by stick, prompt glyph swap, focus sounds |
| `fuse_ui_world_panel_raycast` | ray → panel pixel mapping, misses and back faces, ray clicks, idle surface does not re-render |
| `fuse_ui_input_arbitration` | pointer / keyboard / gamepad ownership between UI and game |
| `fuse_ui_pause_time_domain` | pause and slow motion affect game-time HUD only; locale switching |

Finishing gates: `fuse_ui_dock_drag_*` (dock drag and drop), `fuse_ui_gap_*` (public widget functions no other gate
called), `fuse_ui_widget_golden_spinmenu`, `fuse_ui_a11y_family_*`, `fuse_ui_doc_*` (document classes, cooked
registration for them), `fuse_app_widget_gallery_page*`, `fuse_app_game_menu_sample`, `fuse_ui_install_consumer`,
`fuse_app_stage_assets`. Their CMake wiring is in `tests/finishing.cmake` and at the end of the UI `CMakeLists.txt`.

From the `gui-framework` branch (label `gate`): `fuse_ui_os_seam` (the std OS backend: files, watcher, lock, dynamic
library, helper process, pipes), `fuse_ui_extras_{undo,desktop,pdf,tray,declarative,dataviz3d,dataviz3d_bsp}`,
`fuse_ui_extras_webview` (Xvfb; skips inside the test when no WebKitGTK is installed), `fuse_ui_charts_{series,axes,
decimation,ring,hit,widget,model,export,bench,demo,3d,3d_large,demo3d,mesh3d,mesh3d_large,beyond,transition,
demo_beyond}`, `fuse_app_cocoa_map` (key table of the parked Cocoa backend), `fuse_ui_extras_richtext_{model,undo,
widget,view,golden}` (rich text editor on main's text stack), `fuse_ui_extras_pdf_paths`, `fuse_ui_extras_audio_meter`,
`fuse_ui_extras_gesture`, `fuse_gui_quickstart` (`<fuse/gui.hpp>`), `fuse_ui_l10n_po` (gettext `.po` catalogues). `fuse_ui_extras_tray` starts its own
`dbus-daemon` and needs `busctl` for one of its checks.

Extra: `fuse_ui_text_basics`, `fuse_ui_text_full`, `fuse_ui_widget_interaction`, `fuse_ui_tier_b`, `fuse_ui_tier_b2`,
`fuse_ui_relight_equivalence`, `fuse_ui_platform_keys_v2`, `fuse_ui_text_input`, `fuse_ui_doc_load`,
`fuse_ui_lua_bindings`, `fuse_ui_vk_parity`, the sample runs `fuse_app_hello_sample`, `fuse_app_notes_sample`,
`fuse_ui_game_hud_demo`, `fuse_ui_game_world_panel_demo`; in tree also `fuse_core_ui1_text_input`,
`fuse_scene_renderer_vk_fuse_ui_t0` and `rl_overlay_glyphs`.

## Editor shell (UI9)

Configure with `-DFUSE_EDITOR_SHELL=fuse` to build `fuse_editor_fuse`, which is the editor chrome in FUSE UI on the shared Qt-free `EditorController`. Run `ctest -L fuse_editor_ui`. All ten editor panels and the New Project wizard are ported (45 gates, including the Top / Front / Side orthographic views with View > Viewport Layout single / two / quad on Ctrl+Alt+1..3 (`multi_viewport`), the play-mode frame / badge and Maximize Viewport on Play (`play_frame`), the asset browser grid / type filter / breadcrumb / context menu (`asset_browser_polish`), timeline zoom / pan / loop / snap / key jumps (`timeline_nav`), Preferences shortcut rebinding (`shortcut_rebind`), File > Recent Projects (`recent_projects`), Group / Ungroup / Duplicate subtree / Frame Selected (`entity_group`), collider wireframes and drag handles (`colliders`), Entity > Open Script (`open_script`), the Terrain panel (`terrain`), the Navigation panel (`navigation`), Edit > Preferences (`preferences`) and view modes Lit / Unlit / Wireframe / Lit Wireframe and mesh bounds: `view_modes`). `--gpu-viewport` shows the live 3D scene in the viewport, zero-copy through a `ui_vk` external-texture slot (gates `scene_viewport`, `gpu_viewport`), with a Move/Rotate/Scale gizmo drawn as a viewport overlay (`viewport_gizmo`). Status is in [FUSE_EDITOR_QT_FIRST_PLAN.md §11](../plans/FUSE_EDITOR_QT_FIRST_PLAN.md).
