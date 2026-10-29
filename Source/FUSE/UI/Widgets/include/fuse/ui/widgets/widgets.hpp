// FUSE UI widgets (plan §5). Tier A: Label, Button, ToolButton, CheckBox, RadioButton, Switch, Slider, ScrollBar,
// SpinBox, ComboBox, LineEdit, ProgressBar, Dial, Image, Separator, Link.
//
// Every widget is a registered WidgetClass (plain function table) over core nodes; state lives in the node (value,
// range, checked, text, edit buffer). Styling comes from the theme sheet: `color` = text, `background` = face,
// `background2` = accent / indicator fill (when no gradient is set), `border-color` = outline.
#pragma once

#include "fuse/ui/ui.hpp"

#include <string_view>

namespace fuse::ui::widgets {

/// Registers every Tier A class with the context's registry.
void registerTierA(UiContext& ui);
/// The default dark theme (theme tokens in :root). Style sheet text, lives for the program.
std::string_view defaultThemeCss();
/// Registers the Tier B classes (popup, menu, menuitem, menubar, menubutton, listview, treeview, tabbar, tooltip).
void registerTierB(UiContext& ui);
/// registerTierA + registerTierB + default theme. (With Tier B registered, combo boxes open a popup list on click.)
void install(UiContext& ui);

NodeHandle label(UiContext& ui, NodeHandle parent, std::string_view text, std::string_view id = {});
NodeHandle button(UiContext& ui, NodeHandle parent, std::string_view text, std::string_view id = {});
NodeHandle toolButton(UiContext& ui, NodeHandle parent, std::string_view text, std::string_view id = {});
NodeHandle checkBox(UiContext& ui, NodeHandle parent, std::string_view text, bool checked = false, std::string_view id = {});
NodeHandle radioButton(UiContext& ui, NodeHandle parent, std::string_view text, bool checked = false, std::string_view id = {});
NodeHandle toggleSwitch(UiContext& ui, NodeHandle parent, std::string_view text, bool on = false, std::string_view id = {});
NodeHandle slider(UiContext& ui, NodeHandle parent, f32 lo, f32 hi, f32 value, f32 step = 0.f, std::string_view id = {});
NodeHandle scrollBar(UiContext& ui, NodeHandle parent, bool vertical, std::string_view id = {});
NodeHandle spinBox(UiContext& ui, NodeHandle parent, f32 lo, f32 hi, f32 value, f32 step = 1.f, std::string_view id = {});
/// Items separated by '\n'; value() is the selected index. (Tier A: cycles in place; the popup list is Tier B.)
NodeHandle comboBox(UiContext& ui, NodeHandle parent, std::string_view items, u32 selected = 0, std::string_view id = {});
NodeHandle lineEdit(UiContext& ui, NodeHandle parent, std::string_view text = {}, std::string_view id = {});
NodeHandle progressBar(UiContext& ui, NodeHandle parent, f32 value01, std::string_view id = {});
NodeHandle dial(UiContext& ui, NodeHandle parent, f32 lo, f32 hi, f32 value, std::string_view id = {});
/// `imageIndex` is the CpuRasterizer / renderer image index (stored as user data).
NodeHandle image(UiContext& ui, NodeHandle parent, u32 imageIndex, f32 w, f32 h, std::string_view id = {});
NodeHandle separator(UiContext& ui, NodeHandle parent, std::string_view id = {});
NodeHandle link(UiContext& ui, NodeHandle parent, std::string_view text, std::string_view id = {});
NodeHandle panel(UiContext& ui, NodeHandle parent, std::string_view id = {});

/// Selected item text of a combo box.
std::string_view comboText(const UiContext& ui, NodeHandle combo);

// ---- Tier B (plan §5) ------------------------------------------------------------------------------------------------
// Popups, menus, dialogs and tooltips live in the overlay layer: a core "layer" node kept as the root's last child,
// laid out over the viewport and outside the flow. A popup is a viewport-sized scope node (the scrim) holding the
// panel; outside clicks and Escape close it, focus is trapped in it (UiContext::setModal) and returns to the owner.

/// The overlay layer (created on first use; moved back to the root's last child on every call).
NodeHandle overlayLayer(UiContext& ui);

/// Opens a popup menu at `at` (context coordinates). Items separated by '\n'; "-" is a separator. Picking item i
/// emits Submitted {value = i, text} on the menu scope and on `owner` (when alive), then closes. Returns the scope.
NodeHandle popupMenu(UiContext& ui, NodeHandle owner, std::string_view items, Vec2 at, std::string_view id = {});
/// Closes a popup scope (or the scope containing `n`): releases the modal trap, refocuses the owner, destroys it on
/// the next update (dialogs are hidden instead and can be reopened).
void closePopup(UiContext& ui, NodeHandle n);
/// True while a popup scope is open.
bool popupOpen(const UiContext& ui, NodeHandle scope);

/// A horizontal menu bar; menuBarMenu adds a title button whose popup lists `items` (Submitted re-emitted on it).
NodeHandle menuBar(UiContext& ui, NodeHandle parent, std::string_view id = {});
NodeHandle menuBarMenu(UiContext& ui, NodeHandle bar, std::string_view title, std::string_view items, std::string_view id = {});

/// Modal dialog: a hidden scope in the overlay layer with a centred card (title + body). Add widgets to dialogBody.
NodeHandle dialog(UiContext& ui, std::string_view title, std::string_view id = {});
NodeHandle dialogBody(const UiContext& ui, NodeHandle dlg);
void openDialog(UiContext& ui, NodeHandle dlg);
void closeDialog(UiContext& ui, NodeHandle dlg);

/// Shows `text` under `node` after `delay` seconds of hover; hides on leave. Returns the tooltip node.
NodeHandle setTooltip(UiContext& ui, NodeHandle node, std::string_view text, f32 delay = 0.5f);

/// Virtualised single-selection list (items '\n'-separated; value() = selected index, -1 = none). Only visible rows
/// are painted. Selecting emits ValueChanged and TextChanged {item}; Enter emits Submitted.
NodeHandle listView(UiContext& ui, NodeHandle parent, std::string_view items, i32 selected = -1, std::string_view id = {});
std::string_view listItem(const UiContext& ui, NodeHandle list, u32 index);
u32 listFirstVisible(const UiContext& ui, NodeHandle list);

/// Tree view: one line per node, leading tabs give the depth. value() = selected line. Rows 0..63 can collapse
/// (expansion state is a 64-bit mask; deeper rows stay expanded). Toggling emits Toggled {value = line, flag = open}.
NodeHandle treeView(UiContext& ui, NodeHandle parent, std::string_view lines, std::string_view id = {});
bool treeExpanded(const UiContext& ui, NodeHandle tree, u32 line);
void setTreeExpanded(UiContext& ui, NodeHandle tree, u32 line, bool open);
/// Number of rows currently shown (collapsed subtrees skipped).
u32 treeVisibleRows(const UiContext& ui, NodeHandle tree);

/// Tab bar (titles '\n'-separated; value() = current tab) and a tab widget (tab bar + one page per title; only the
/// current page is displayed).
NodeHandle tabBar(UiContext& ui, NodeHandle parent, std::string_view titles, u32 current = 0, std::string_view id = {});
NodeHandle tabWidget(UiContext& ui, NodeHandle parent, std::string_view titles, std::string_view id = {});
NodeHandle tabPage(const UiContext& ui, NodeHandle tabs, u32 index);
NodeHandle tabWidgetBar(const UiContext& ui, NodeHandle tabs);

/// Scroll area: a column panel with overflow: scroll (wheel scrolls; role ScrollArea).
NodeHandle scrollArea(UiContext& ui, NodeHandle parent, std::string_view id = {});

} // namespace fuse::ui::widgets
