// FUSE UI widgets (plan §5). Tier A: Label, Button, ToolButton, CheckBox, RadioButton, Switch, Slider, ScrollBar,
// SpinBox, ComboBox, LineEdit, ProgressBar, Dial, Image, Separator, Link.
//
// Every widget is a registered WidgetClass (plain function table) over core nodes; state lives in the node (value,
// range, checked, text, edit buffer). Styling comes from the theme sheet: `color` = text, `background` = face,
// `background2` = accent / indicator fill (when no gradient is set), `border-color` = outline.
#pragma once

#include <memory>
#include <span>

#include <string>
#include <vector>

#include "fuse/ui/icons.hpp"
#include "fuse/ui/ui.hpp"

#include <expected>
#include <string_view>

namespace fuse::ui::widgets {

/// Registers every Tier A class with the context's registry.
void registerTierA(UiContext& ui);
/// The default dark theme (theme tokens in :root). Style sheet text, lives for the program.
std::string_view defaultThemeCss();
// ---- theme variants (theme.cpp) --------------------------------------------------------------------------------------
/// The three token sets of the widget theme. The rules are the same; only the `:root` tokens differ (high contrast
/// adds outline rules). Every colour of the rules and of the widget paint code is a token.
enum class ThemeVariant : u8 { Dark, Light, HighContrast };
/// High-contrast system colours (Win32 GetSysColor of the active scheme; CSS system colours).
struct HighContrastPalette {
    Color window = Color::rgb(0x000000), windowText = Color::rgb(0xFFFFFF);
    Color highlight = Color::rgb(0x1AEBFF), highlightText = Color::rgb(0x000000);
    Color buttonFace = Color::rgb(0x000000), buttonText = Color::rgb(0xFFFFFF);
    Color grayText = Color::rgb(0x3FF23F), hotlight = Color::rgb(0xFFFF00);
    friend bool operator==(const HighContrastPalette&, const HighContrastPalette&) = default;
};
struct ThemeDesc {
    ThemeVariant variant = ThemeVariant::Dark;
    bool hasAccent = false;       ///< Dark / Light: `accent` replaces --accent (and a shade of it --accent-hi)
    Color accent{};
    HighContrastPalette contrast{}; ///< HighContrast only
};
/// The whole theme sheet of a variant (tokens + rules; HighContrast with the default palette). Lives for the program.
std::string_view themeCss(ThemeVariant v);
std::string_view lightThemeCss();
std::string highContrastThemeCss(const HighContrastPalette& p = {});
std::string themeCss(const ThemeDesc& d);
/// Only the `:root` tokens of `d` (plus the outline rules for HighContrast): a sheet that re-colours the widget theme
/// when added after it (fuse::app::AppWindow::applyTheme keeps one such sheet and replaces it in place).
std::string themeTokensCss(const ThemeDesc& d);
/// Puts the theme of `d` into the context's theme slot (UiContext::setThemeStyleSheet) and restyles.
std::expected<void, UiError> setTheme(UiContext& ui, const ThemeDesc& d);
inline std::expected<void, UiError> setTheme(UiContext& ui, ThemeVariant v) { return setTheme(ui, ThemeDesc{.variant = v}); }
/// The token table: names ("--accent") by index, and the colour a token has under `d` (false: unknown / not a colour).
u32 themeTokenCount();
std::string_view themeTokenName(u32 index);
bool themeTokenColor(const ThemeDesc& d, std::string_view name, Color& out);
/// Registers the Tier B classes (popup, menu, menuitem, menubar, menubutton, listview, treeview, tabbar, tooltip).
void registerTierB(UiContext& ui);
/// registerTierA + registerTierB + default theme. (With Tier B registered, combo boxes open a popup list on click.)
void install(UiContext& ui);
/// Registers only the named widget classes (a cooked registration list, see fuse/ui/doc/cook.hpp) plus the default
/// theme. Names the context already has (core panel / label) are skipped. Returns how many names are unknown.
u32 installClasses(UiContext& ui, std::span<const std::string_view> classes, bool theme = true);

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

/// Tree view: one line per node, leading tabs give the depth. value() = selected line. Any line with children can
/// collapse (one bit per line, no row limit). Toggling emits Toggled {value = line, flag = open}. Setting new text
/// keeps the collapsed lines by index. For a model-backed, virtualised tree see treeModelView (modelview.hpp).
NodeHandle treeView(UiContext& ui, NodeHandle parent, std::string_view lines, std::string_view id = {});
bool treeExpanded(const UiContext& ui, NodeHandle tree, u32 line);
void setTreeExpanded(UiContext& ui, NodeHandle tree, u32 line, bool open);
/// Number of rows currently shown (collapsed subtrees skipped).
u32 treeVisibleRows(const UiContext& ui, NodeHandle tree);
/// The line under `at` (context coordinates; -1 outside the rows) and a shown line's row rectangle (empty when it is
/// inside a collapsed subtree): what drag-and-drop targets and drop marks need.
i32 treeLineAt(UiContext& ui, NodeHandle tree, Vec2 at);
Rect treeLineRect(UiContext& ui, NodeHandle tree, u32 line);

/// Tab bar (titles '\n'-separated; value() = current tab) and a tab widget (tab bar + one page per title; only the
/// current page is displayed).
NodeHandle tabBar(UiContext& ui, NodeHandle parent, std::string_view titles, u32 current = 0, std::string_view id = {});
NodeHandle tabWidget(UiContext& ui, NodeHandle parent, std::string_view titles, std::string_view id = {});
NodeHandle tabPage(const UiContext& ui, NodeHandle tabs, u32 index);
NodeHandle tabWidgetBar(const UiContext& ui, NodeHandle tabs);

/// Scroll area: a column panel with overflow: scroll (wheel scrolls; role ScrollArea).
NodeHandle scrollArea(UiContext& ui, NodeHandle parent, std::string_view id = {});

// ---- Tier B: splitters, docks, item models -----------------------------------------------------------------------
/// Splitter: panes separated by draggable handles (Row = side by side, Column = stacked). A pane with size > 0 keeps
/// that many px (dragging resizes it); size 0 panes share the rest. Dragging emits ValueChanged {px} on the splitter.
NodeHandle splitter(UiContext& ui, NodeHandle parent, bool stacked = false, std::string_view id = {});
NodeHandle splitterPane(UiContext& ui, NodeHandle split, f32 size = 0.f, std::string_view id = {});

/// Dock area: left / right / bottom regions around a centre, each resizable by splitter handles and hidden while
/// empty. Dock widgets are titled panels with a close button; several in one region stack (or share it as tabs, see
/// dockTabify). A dock's user data is reserved for the dock system.
enum class DockSide : u8 { Left, Right, Bottom, Center };
NodeHandle dockArea(UiContext& ui, NodeHandle parent, std::string_view id = {});
NodeHandle dockRegion(const UiContext& ui, NodeHandle area, DockSide side);
NodeHandle dockWidget(UiContext& ui, NodeHandle area, std::string_view title, DockSide side, std::string_view id = {});
NodeHandle dockBody(const UiContext& ui, NodeHandle dock);
void dockTo(UiContext& ui, NodeHandle dock, DockSide side);
void dockClose(UiContext& ui, NodeHandle dock); ///< hides (dockTo re-shows); emits Toggled {flag = false}
/// Re-shows a closed dock where it is: in its split pane, as a tab of its group (the current tab stays) or floating
/// (dockTo also moves it to the top level of a side); emits Toggled {flag = true}.
void dockOpen(UiContext& ui, NodeHandle dock);

/// Dock splits (Qt QMainWindow::splitDockWidget): a side region (and the centre) holds a tree of nested splitters
/// ("docksplit": rows and columns of panes separated by draggable handles) whose leaves are docks. Left / Right split
/// `target`'s place into a row, Top / Bottom into a column; a split of the same direction gains a pane next to
/// target's instead of nesting. A tabbed target is split as a whole group (strip and members stay together). Center
/// makes `dock` a tab of target's group and raises it (dockTabify + dockRaise). `size` > 0: px of the new pane along
/// the split axis (0: the panes share). Splits left with one pane collapse; panes and handles hide while their docks
/// are all closed. `dock` leaves its tab group / floating state first. False when the docks are not in the same
/// area, dock == target or target floats.
enum class DockEdge : u8 { Left, Right, Top, Bottom, Center };
bool dockSplit(UiContext& ui, NodeHandle dock, NodeHandle target, DockEdge edge, f32 size = 0.f);
/// Takes a dock out of the area (collapsing the split it leaves, dissolving a tab group of two) and destroys it.
void dockRemove(UiContext& ui, NodeHandle dock);
/// The side region a dock sits in, however deep in splits (Center when it floats or is not in a dock area).
DockSide dockSideOf(const UiContext& ui, NodeHandle dock);
/// The innermost split holding a dock (invalid at the top level of its side).
NodeHandle dockSplitOf(const UiContext& ui, NodeHandle dock);
/// Tabbed docks (QMainWindow::tabifyDockWidget): `dock` joins `withDock` in its region as the last tab of its group.
/// A group shows a tab strip in place of the members' title bars and only the current member's body; clicking a tab
/// (or Left / Right on the focused strip) raises it, the strip's close box closes the current dock. A closed member
/// keeps its place in the group without a tab; when the current one closes the first open member takes over. dockTo
/// and dockFloat take a dock out of its group; a group left with one dock dissolves. Docks of different areas, and a
/// floating `withDock`, are refused.
void dockTabify(UiContext& ui, NodeHandle dock, NodeHandle withDock);
/// Makes a tabbed dock the current tab of its group (re-opening it when closed); emits ValueChanged {value = tab
/// index} on the tab strip. A floating dock comes to the front of the floating docks.
void dockRaise(UiContext& ui, NodeHandle dock);
/// The dock whose body shows in `dock`'s tab group (`dock` itself when it is not tabbed; invalid when every member
/// of the group is closed).
NodeHandle dockCurrent(const UiContext& ui, NodeHandle dock);
NodeHandle dockTabBar(const UiContext& ui, NodeHandle dock); ///< the group's tab strip (invalid when not tabbed)
Rect dockTabRect(UiContext& ui, NodeHandle dock);            ///< `dock`'s tab in the strip (empty: none; after update)
/// Floating docks: the dock leaves its region and becomes a panel of `rect` (context coordinates, at least 80 x 60)
/// in the overlay layer, above the window content and below popups. Dragging its title bar moves it (the title stays
/// inside the viewport), pressing the title brings it to the front; dockTo docks it again, dockClose hides it.
void dockFloat(UiContext& ui, NodeHandle dock, Rect rect);
bool dockFloating(const UiContext& ui, NodeHandle dock);
Rect dockFloatRect(const UiContext& ui, NodeHandle dock); ///< current floating rect (empty when docked)
/// Dock drag and drop (QDockWidget). Dragging a docked dock's title bar, or its tab in a tab strip, more than 8 px
/// tears it off into a floating dock that follows the pointer; a floating dock is dragged by its title bar as before.
/// While a dragged dock's pointer is over its dock area, drop-zone indicators show: Left / Right / Bottom at the area
/// edges, Center in the centre region, and Tab in the middle of the docked dock under the pointer (an area indicator
/// the Tab indicator would cover is not offered). The zone whose indicator is under the pointer is hot and a
/// translucent preview shows where the dock will land. Releasing on a hot zone docks it there (dockTo) or makes it
/// the current tab of that dock's group (dockTabify + dockRaise); releasing anywhere else leaves it floating.
/// Escape (the dragged dock's title bar holds the focus during the drag), dockCancelDrag or a cancelled touch puts
/// the dock back where the drag started: region and order, tab group / index / current tab, or floating rect. Mouse
/// and touch both work (title bars and tab strips capture drags). A floating dock still holds no tabs: the Tab zone
/// is only offered over docked docks.
enum class DockDropZone : u8 { None, Left, Right, Bottom, Center, Tab };
NodeHandle dockDragged(const UiContext& ui);    ///< the dock being dragged (past the threshold), else invalid
DockDropZone dockDropZone(const UiContext& ui); ///< the hot zone of the drag in progress
NodeHandle dockDropTarget(const UiContext& ui); ///< Tab zone: the dock to tab with
Rect dockDropPreview(const UiContext& ui);      ///< where the dock lands on release (empty: stays floating)
/// Where the indicator of `zone` is drawn in `area` (`tabWith`: the dock of the Tab zone). Valid after update().
Rect dockDropIndicator(const UiContext& ui, NodeHandle area, DockDropZone zone, NodeHandle tabWith = {});
bool dockCancelDrag(UiContext& ui);             ///< as Escape; false when no drag is in progress

/// An item of a model. Flat models: the row. Tree models: an opaque id chosen by the model (kRootItem is the invisible
/// root; ids must be unique and stable while the item exists).
using ItemId = u64;
inline constexpr ItemId kRootItem = 0;
/// Typed roles beyond the display text (ItemModel::role). What ItemValue carries per role:
/// Display / Edit / ToolTip: `text` (Edit may also set `number`); Decoration: `integer` = icon index (handed to the
/// view's icon painter, setViewIconPainter) and / or `color` (a swatch); CheckState: `integer` 0 unchecked, 1 checked,
/// 2 partial; Foreground / Background: `color`; TextAlign: `integer` = TextAlignment; User: anything.
enum class ItemRole : u8 { Display, Edit, Decoration, CheckState, ToolTip, Foreground, Background, TextAlign, User };
struct ItemValue {
    std::string_view text;
    double number = 0.0;
    Color color{};
    i32 integer = 0;
};
enum ItemFlags : u32 { kItemSelectable = 1, kItemEditable = 2, kItemCheckable = 4, kItemEnabled = 8 };
class ModelNotifier; // model -> view change notifications (fuse/ui/widgets/modelview.hpp)

/// Item model: a function table over caller data (no inheritance). `data` may return a view into `scratch`.
/// Only `rows` + `data` are needed for a flat model; every member after `header` is optional and a flat view never
/// touches the ones that are null (the flat fast path is unchanged).
struct ItemModel {
    const void* user = nullptr;
    u32 columns = 1;
    u32 (*rows)(const void* user) = nullptr;
    std::string_view (*data)(const void* user, u32 row, u32 col, std::string& scratch) = nullptr;
    std::string_view (*header)(const void* user, u32 col) = nullptr; ///< optional
    // ---- roles, flags, editing (optional). `item` is the row of a flat model, the node id of a tree model.
    /// Fills `out` for `role` and returns true, or returns false when the item has no value for it (the view then
    /// uses its default). `out.text` may point into `scratch`.
    bool (*role)(const void* user, ItemId item, u32 col, ItemRole role, ItemValue& out, std::string& scratch) = nullptr;
    u32 (*flags)(const void* user, ItemId item, u32 col) = nullptr; ///< ItemFlags; null = selectable + enabled
    /// Stores an edited value (roles Edit and CheckState). Returns false to reject. A model with a notifier reports
    /// the change through it (dataChanged).
    bool (*setData)(const void* user, ItemId item, u32 col, ItemRole role, const ItemValue& value) = nullptr;
    // ---- hierarchy (optional; treeModelView needs childCount, childAt and itemData).
    u32 (*childCount)(const void* user, ItemId parent) = nullptr; ///< children fetched so far
    ItemId (*childAt)(const void* user, ItemId parent, u32 row) = nullptr;
    bool (*hasChildren)(const void* user, ItemId item) = nullptr; ///< expandable without counting; null: childCount > 0
    std::string_view (*itemData)(const void* user, ItemId item, u32 col, std::string& scratch) = nullptr;
    /// Lazy children: a view that shows the last fetched child of `parent` calls fetchMore while canFetchMore holds,
    /// then re-reads childCount. fetchMore must not also report the new rows through the notifier.
    bool (*canFetchMore)(const void* user, ItemId parent) = nullptr;
    void (*fetchMore)(const void* user, ItemId parent) = nullptr;
    /// Change notifications (not owned, optional): views subscribe when they are created.
    ModelNotifier* notifier = nullptr;
};
/// A ready-made row/column string model (owns its cells). sortBy reorders rows (stable, numeric when both parse).
class StringTableModel {
public:
    explicit StringTableModel(std::string_view headersTabSeparated);
    StringTableModel(const StringTableModel&) = delete; ///< the model table points at this object
    StringTableModel& operator=(const StringTableModel&) = delete;
    void addRow(std::string_view cellsTabSeparated);
    void clear() { m_cells.clear(); m_rows = 0; }
    void sortBy(u32 col, bool ascending);
    u32 rows() const { return m_rows; }
    std::string_view cell(u32 row, u32 col) const { return m_cells[std::size_t(row) * m_cols + col]; }
    const ItemModel& model() const { return m_model; }

private:
    std::vector<std::string> m_headers, m_cells;
    u32 m_cols = 1, m_rows = 0;
    ItemModel m_model;
};
/// Virtualised table view over a model (not owned; must outlive the view): header row (click sorts: Toggled
/// {value = column, flag = ascending}; drag a header edge to resize), single row selection (value() = row, -1 none;
/// ValueChanged; Enter -> Submitted), wheel / keys / scrollbar drag scroll. Only visible rows are read and painted.
/// Columns wider than the view scroll horizontally (Shift + wheel, Left / Right, the bar at the bottom). Multi /
/// extended / cell selection, in-place editing, column reorder / hide / freeze, model notifications and the tree
/// view over the same machinery: fuse/ui/widgets/modelview.hpp.
NodeHandle tableView(UiContext& ui, NodeHandle parent, const ItemModel* model, std::string_view id = {});
/// Call after the model's rows change (re-clamps the scroll, repaints).
void tableModelChanged(UiContext& ui, NodeHandle table);
u32 tableFirstVisible(const UiContext& ui, NodeHandle table);
f32 tableColumnWidth(const UiContext& ui, NodeHandle table, u32 col);
void setTableColumnWidth(UiContext& ui, NodeHandle table, u32 col, f32 px);
/// List view over column 0 of a model (shares the table machinery, no header).
NodeHandle modelListView(UiContext& ui, NodeHandle parent, const ItemModel* model, std::string_view id = {});

/// Dock layout persistence (plan §5 "saved layouts"): a small text blob with the side region sizes and, per dock
/// widget with an id, its side, order in the region and open state. Restoring moves / opens / closes the named docks
/// and resizes the regions; docks the blob does not name keep their place, names with no dock are ignored. A malformed
/// blob changes nothing and returns false. Tab groups ("tab <id> <group> <index> current|back") and floating docks
/// ("float <id> <x> <y> <w> <h> open|closed") are extra lines written only when present, so a layout without them is
/// byte-identical to what earlier versions wrote and those blobs restore unchanged. A layout with dock splits is
/// written as version 2 ("fusedock 2"): it adds one "tree <side> <node>..." line per side with the split structure
/// (node: "d:<id>", or "h[" / "v[" then "p <px> <node>..." per pane, then "]"; px 0 = shared). Restoring rebuilds the
/// splits and places the named docks in them; restoring any blob first flattens the splits the area has. Version 1
/// blobs still restore.
std::string dockSaveLayout(const UiContext& ui, NodeHandle area);
bool dockRestoreLayout(UiContext& ui, NodeHandle area, std::string_view layout);

// ---- Tier C: delegates, proxies, grid view (plan §5) ------------------------------------------------------------------
/// Cell delegate: custom painting for one table column (the view still owns selection, scrolling and the row stripe).
struct CellDelegate {
    const void* user = nullptr;
    void (*paint)(const void* user, const PaintContext& pc, DrawList& dl, Rect cell, u32 row, u32 col,
                  std::string_view text, bool selected) = nullptr;
    // ---- in-place editing (optional; see "Editing" in fuse/ui/widgets/modelview.hpp). Null members use the default
    // editor, a line edit. `item` is the row of a flat model, the node id of a tree model.
    /// Creates the editor as a child of `view` (the view places it over the cell and focuses it).
    NodeHandle (*createEditor)(const void* user, UiContext& ui, NodeHandle view, ItemId item, u32 col) = nullptr;
    /// Loads the cell's Edit value (its text, and the number it parses to) into the editor.
    void (*setEditorData)(const void* user, UiContext& ui, NodeHandle editor, const ItemValue& value) = nullptr;
    /// Reads the editor back into `out` (`out.text` may point into `scratch`); false rejects the edit.
    bool (*commit)(const void* user, UiContext& ui, NodeHandle editor, ItemValue& out, std::string& scratch) = nullptr;
};
/// Sets (or with nullptr clears) the delegate for `col`. The delegate must outlive the view.
void setTableDelegate(UiContext& ui, NodeHandle table, u32 col, const CellDelegate* delegate);

/// Sort / filter proxy over a source model (not owned). Identity (no row map, no memory) until a filter or sort is
/// set; filtering is a case-insensitive substring match on one column. Call invalidate() after the source changes
/// (automatic when the source has a notifier: the proxy then rebuilds and reports a reset through its own notifier).
/// Proxies stack: a proxy's model() is a valid source for another proxy. Roles, flags and setData are forwarded with
/// the row mapped. Tree callbacks are not (see TreeFilterProxy).
class SortFilterProxy {
public:
    explicit SortFilterProxy(const ItemModel* source);
    ~SortFilterProxy();
    SortFilterProxy(const SortFilterProxy&) = delete;
    SortFilterProxy& operator=(const SortFilterProxy&) = delete;
    void setFilter(u32 col, std::string_view needle); ///< empty needle = no filter
    /// Filter over every column (a row passes when any cell matches). Replaces setFilter's column filter.
    void setFilterAllColumns(std::string_view needle);
    /// Wildcard filter, case-insensitive: '*' = any run, '?' = one character; the pattern must match the whole cell
    /// (a pattern with no wildcard is a substring match). `col` < 0 = any column.
    void setFilterWildcard(i32 col, std::string_view pattern);
    void sortBy(i32 col, bool ascending);              ///< col < 0 = source order (clears every sort key)
    /// Adds a lower-priority sort key (ties of the earlier keys are ordered by it; the sort is stable, so remaining
    /// ties keep source order). Up to kMaxSortKeys keys including the sortBy one.
    void addSortKey(u32 col, bool ascending);
    static constexpr u32 kMaxSortKeys = 4;
    u32 sortKeyCount() const { return m_sortKeys; }
    void invalidate();
    u32 rows() const;
    u32 sourceRow(u32 row) const { return m_identity ? row : m_map[row]; }
    const ItemModel& model() const { return m_model; }
    ModelNotifier& notifier() { return *m_notifier; }

private:
    bool accepts(u32 srcRow, std::string& scratch, std::string& low) const;
    const ItemModel* m_src;
    ItemModel m_model;
    std::vector<u32> m_map;
    std::string m_needle;
    u32 m_filterCol = 0;
    i32 m_sortCol = -1;
    bool m_asc = true, m_identity = true;
    // appended (multi-key sort, all-column / wildcard filter, notifications)
    struct SortKey { u32 col = 0; bool asc = true; };
    SortKey m_extraKeys[kMaxSortKeys - 1]{};
    u32 m_sortKeys = 0; ///< total keys (m_sortCol is key 0)
    bool m_allCols = false, m_wildcard = false;
    std::unique_ptr<ModelNotifier> m_notifier;
    u32 m_srcSub = 0;
};

/// Virtualised icon / tile grid over column 0 of a model: tiles of tileW x tileH flow left to right, rows scroll.
/// value() = selected item (-1 none); arrows move in 2D, Enter -> Submitted. Only visible tiles are read.
NodeHandle gridView(UiContext& ui, NodeHandle parent, const ItemModel* model, f32 tileW = 96.f, f32 tileH = 72.f,
                    std::string_view id = {});
u32 gridColumns(const UiContext& ui, NodeHandle grid);
u32 gridFirstVisibleRow(const UiContext& ui, NodeHandle grid);

// ---- Tier D: rich and pro widgets (plan §5) ----------------------------------------------------------------------------
/// Property grid (reflection-driven inspector): name / editor rows built from descriptors. Group rows fold the rows
/// after them (until the next group). Any editor change emits Submitted {value = row index, text = name} on the grid.
enum class PropKind : u8 { Group, Bool, Number, Text, Enum, Color };
struct PropertyDesc {
    std::string_view name;
    PropKind kind = PropKind::Text;
    f32 lo = 0.f, hi = 100.f, step = 1.f; ///< Number
    std::string_view options{};           ///< Enum: '\n'-separated
};
NodeHandle propertyGrid(UiContext& ui, NodeHandle parent, std::string_view id = {});
/// Adds a row; `value` is "true"/"false", a number, text, an enum index or "#RRGGBB[AA]". Returns the editor node.
NodeHandle propertyRow(UiContext& ui, NodeHandle grid, const PropertyDesc& desc, std::string_view value = {});
/// Current value of row `index` in the same text form propertyRow takes.
std::string propertyValue(const UiContext& ui, NodeHandle grid, u32 index);
u32 propertyCount(const UiContext& ui, NodeHandle grid);
void setPropertyGroupOpen(UiContext& ui, NodeHandle grid, u32 groupIndex, bool open);

/// HSV colour picker: saturation / value square + hue bar + alpha bar. text() = "#RRGGBBAA"; drags and arrow keys
/// emit ValueChanged {value = hue} and TextChanged {text}.
NodeHandle colorPicker(UiContext& ui, NodeHandle parent, Color initial, std::string_view id = {});
Color colorPickerColor(const UiContext& ui, NodeHandle picker);
void setColorPickerColor(UiContext& ui, NodeHandle picker, Color c);

/// Month calendar: value() = days since 1970-01-01. Arrows move by a day / week, PageUp / PageDown by a month, the
/// header arrows page months, clicking a day selects it (ValueChanged), Enter -> Submitted.
NodeHandle calendar(UiContext& ui, NodeHandle parent, int year, int month, int day, std::string_view id = {});
void calendarDate(const UiContext& ui, NodeHandle cal, int& year, int& month, int& day);
i32 daysFromCivil(int y, int m, int d);
void civilFromDays(i32 z, int& y, int& m, int& d);

/// Chart: line, bar, area or scatter series over a shared x index, auto-scaled y axis with grid lines. Hovering
/// sets value() to the nearest x index (-1 outside). ChartKind::Flame shows spans instead of series (chartSetFlame).
enum class ChartKind : u8 { Line, Bar, Area, Scatter, Flame };
NodeHandle chart(UiContext& ui, NodeHandle parent, ChartKind kind, std::string_view id = {});
void chartSetSeries(UiContext& ui, NodeHandle chart, u32 index, std::string_view name, std::span<const f32> values, Color color);
u32 chartSeriesCount(const UiContext& ui, NodeHandle chart);
/// Flame graph (profiler icicle): hierarchical spans painted as labelled bars stacked by depth, depth 0 at the top,
/// x scaled to the total time range [min start, max start + duration]; a label is clipped to its bar. A span with a
/// transparent colour gets a warm colour picked from its name. Hovering sets value() to the hovered span index (-1
/// outside; ValueChanged), a click on a span emits Submitted {value = span index, text = name}.
struct FlameSpan {
    std::string_view name;
    f32 start = 0.f, duration = 0.f; ///< any time unit
    u32 depth = 0;
    Color color{};
};
/// Replaces the chart's spans (copied) and makes it a ChartKind::Flame chart.
void chartSetFlame(UiContext& ui, NodeHandle chart, std::span<const FlameSpan> spans);
u32 chartFlameSpanCount(const UiContext& ui, NodeHandle chart);
Rect chartFlameSpanRect(const UiContext& ui, NodeHandle chart, u32 index); ///< context coordinates (after update)
i32 chartFlameSpanAt(const UiContext& ui, NodeHandle chart, Vec2 pos);     ///< span under `pos`, -1 = none

/// Markdown view: headings (#, ##, ###), paragraphs, '-' / '*' bullets, numbered items, ``` code blocks and '>' quotes
/// become styled children (classes md-h1..md-h3, md-p, md-li, md-code, md-quote): labels, or rich text views for
/// blocks with inline **bold** / __bold__ / *italic* / _italic_ (drawn in the family's bold / italic face, synthesised
/// when the family has none). Headings are bold. Inline ` is stripped; the node text is always the plain text.
NodeHandle markdownView(UiContext& ui, NodeHandle parent, std::string_view markdown, std::string_view id = {});
void setMarkdown(UiContext& ui, NodeHandle view, std::string_view markdown);

/// Registers the Tier C / D classes (gridview, colorpicker, calendar, chart); install() calls it.
void registerTierCD(UiContext& ui);

// ---- Tier D (2): pro widgets (plan §5) ---------------------------------------------------------------------------------
/// Font picker: family list + size spin box + Bold / Italic check boxes + live preview line drawn in the chosen face
/// (font-family / font-weight / font-style on the preview: a real installed face, else synthesised). With `families`
/// empty the list is the bundled family and the generic ones (Open Sans, Sans Serif, Serif, Monospace, System UI)
/// followed by every installed family (fuse::text::systemFontFamilies(); nothing more on a machine without fonts);
/// otherwise exactly the '\n'-separated names given. Any change emits Submitted {text = family, value = size} on the
/// picker.
NodeHandle fontPicker(UiContext& ui, NodeHandle parent, std::string_view families = {}, f32 size = 14.f, std::string_view id = {});
std::string fontPickerFamily(const UiContext& ui, NodeHandle picker);
f32 fontPickerSize(const UiContext& ui, NodeHandle picker);
bool fontPickerBold(const UiContext& ui, NodeHandle picker);
bool fontPickerItalic(const UiContext& ui, NodeHandle picker);
/// Sets the weight / slant boxes (emits Submitted when they change).
void fontPickerSetStyle(UiContext& ui, NodeHandle picker, bool bold, bool italic);

/// File dialog (modal, built on dialog()): path field, "Up" button, directory listing (folders first, then files
/// matching `filter`, e.g. "*.fuseui;*.png", empty = all), file-name field, OK / Cancel. Activating a folder enters
/// it; OK (or Enter / double activation on a file) emits Submitted {text = chosen path} on the dialog and closes it;
/// Cancel emits Submitted {text = ""}. `save` mode accepts names that do not exist yet.
NodeHandle fileDialog(UiContext& ui, std::string_view title, std::string_view directory, std::string_view filter = {},
                      bool save = false, std::string_view id = {});
bool fileDialogNavigate(UiContext& ui, NodeHandle dlg, std::string_view directory);
std::string fileDialogDirectory(const UiContext& ui, NodeHandle dlg);
u32 fileDialogEntryCount(const UiContext& ui, NodeHandle dlg);
std::string fileDialogEntry(const UiContext& ui, NodeHandle dlg, u32 index); ///< names; folders end in '/'
/// Activates entry `index` (folder: enter; file: accept). Returns the accepted path or "".
std::string fileDialogActivate(UiContext& ui, NodeHandle dlg, u32 index);

/// Node graph editor: nodes with typed input / output pins, bezier links, drag nodes, drag from an output pin to an
/// input pin to link (types must match unless either is 0 = any; an input takes one link; cycles are refused),
/// right-drag or middle-drag pans, wheel zooms, Delete removes the selected node. value() = selected node (-1).
/// Link changes emit ValueChanged {value = link count}.
struct GraphPin { std::string_view name; u32 type = 0; };
NodeHandle nodeGraph(UiContext& ui, NodeHandle parent, std::string_view id = {});
u32 nodeGraphAddNode(UiContext& ui, NodeHandle graph, std::string_view title, Vec2 pos, std::span<const GraphPin> inputs,
                     std::span<const GraphPin> outputs);
bool nodeGraphConnect(UiContext& ui, NodeHandle graph, u32 fromNode, u32 outPin, u32 toNode, u32 inPin);
bool nodeGraphDisconnect(UiContext& ui, NodeHandle graph, u32 toNode, u32 inPin);
bool nodeGraphRemoveNode(UiContext& ui, NodeHandle graph, u32 node);
u32 nodeGraphNodeCount(const UiContext& ui, NodeHandle graph);
u32 nodeGraphLinkCount(const UiContext& ui, NodeHandle graph);
Vec2 nodeGraphNodePos(const UiContext& ui, NodeHandle graph, u32 node);
/// Screen position of a pin's dot (tests, tutorials). `output` picks the side.
Vec2 nodeGraphPinPos(const UiContext& ui, NodeHandle graph, u32 node, u32 pin, bool output);
/// Topological order of live nodes (empty when the graph has a cycle, which connect never allows).
std::vector<u32> nodeGraphTopoOrder(const UiContext& ui, NodeHandle graph);

/// Curve editor: one float curve of keys (t, v) with linear / smooth (Catmull-Rom style, monotone in t) / step
/// interpolation. Click empty space adds a key, drag moves keys (t kept between neighbours), Delete removes the
/// selected key. Changes emit ValueChanged {value = key count}.
enum class CurveInterp : u8 { Linear, Smooth, Step };
NodeHandle curveEditor(UiContext& ui, NodeHandle parent, f32 tMin = 0.f, f32 tMax = 1.f, f32 vMin = 0.f, f32 vMax = 1.f,
                       std::string_view id = {});
u32 curveAddKey(UiContext& ui, NodeHandle curve, f32 t, f32 v, CurveInterp interp = CurveInterp::Smooth);
u32 curveKeyCount(const UiContext& ui, NodeHandle curve);
Vec2 curveKey(const UiContext& ui, NodeHandle curve, u32 index); ///< (t, v)
f32 curveEvaluate(const UiContext& ui, NodeHandle curve, f32 t);

/// Timeline: named tracks with clips [start, end), a time ruler and a playhead. Dragging the ruler scrubs
/// (ValueChanged {value = time}); dragging a clip moves it (snapped to `snap` seconds when > 0, free with Shift),
/// selects it and emits Event {value = 5} on press, {value = 6} per move and {value = 7} on release (timelineClipDrag
/// tells which clip and where the pointer is). Ctrl+Down / Up select the next / previous clip, Ctrl+Left / Right move
/// the selected clip by the snap step (a frame without one; Ctrl+Shift: a second) and emit Event {value = 2}
/// (timelineClipDrag names the clip, inactive); Escape clears the selection; wheel zooms. value() = playhead time.
NodeHandle timeline(UiContext& ui, NodeHandle parent, f32 duration, std::string_view id = {});
u32 timelineAddTrack(UiContext& ui, NodeHandle tl, std::string_view name);
u32 timelineAddClip(UiContext& ui, NodeHandle tl, u32 track, f32 start, f32 end, std::string_view label, Color color);
void timelineSetSnap(UiContext& ui, NodeHandle tl, f32 seconds);
/// Visible range [start, start + span) seconds, scaled to the lane width (span 0 = free zoom: wheel / pxPerSec).
/// With a range set, Ctrl+wheel zooms about the pointer and Shift+wheel pans, then emit Event {value = 1}.
void timelineSetView(UiContext& ui, NodeHandle tl, f32 startSec, f32 spanSec);
Vec2 timelineView(const UiContext& ui, NodeHandle tl); ///< (start, span) seconds
Vec2 timelineClip(const UiContext& ui, NodeHandle tl, u32 track, u32 clip); ///< (start, end)
/// The current / last clip drag: track, clip, pointer time (seconds, unsnapped) at press and now, the clip start at
/// press, and whether the drag is still in progress.
struct TimelineClipDrag {
    u32 track = 0, clip = 0;
    f32 downSec = 0.f, pointerSec = 0.f, fromStart = 0.f;
    bool active = false;
};
TimelineClipDrag timelineClipDrag(const UiContext& ui, NodeHandle tl);
/// Selects a clip (the one Ctrl+Left / Right move; -1 clears), as a press on it does. Out-of-range indices clear.
void timelineSelectClip(UiContext& ui, NodeHandle tl, i32 track, i32 clip);
/// The selected clip; false (outputs untouched) when none is selected.
bool timelineSelectedClip(const UiContext& ui, NodeHandle tl, u32& track, u32& clip);
/// Clips under time `t` on every track (sequencer queries).
u32 timelineActiveClips(const UiContext& ui, NodeHandle tl, f32 t);

/// Viewport widget: shows an externally rendered image (a rasteriser / GPU image index, e.g. the engine's scene
/// colour target - zero copy) letterboxed to its aspect ratio, and forwards input in image pixel coordinates.
/// Each forwarded input emits SignalKind::Event {value = EventType}; read it with viewportLastInput().
struct ViewportInput { EventType type = EventType::PointerMove; Vec2 pos; MouseButton button = MouseButton::Left; Vec2 wheel; Key key = Key::None; };
NodeHandle viewportWidget(UiContext& ui, NodeHandle parent, u32 imageIndex, u32 imageW, u32 imageH, std::string_view id = {});
void viewportSetImage(UiContext& ui, NodeHandle vp, u32 imageIndex, u32 imageW, u32 imageH);
Rect viewportImageRect(const UiContext& ui, NodeHandle vp); ///< letterboxed rect in context coordinates
const ViewportInput& viewportLastInput(const UiContext& ui, NodeHandle vp);
u32 viewportInputCount(const UiContext& ui, NodeHandle vp);
/// Vector overlay painted over the viewport image (gizmos, selection markers), in image pixel coordinates; it is part
/// of the UI draw list, so it composes over an external (zero-copy) image as well as a CPU one.
struct ViewportOverlay {
    struct Line { Vec2 a, b; Color color; f32 width = 2.f; };
    struct Dot { Vec2 center; f32 radius = 4.f; Color color; Color border{}; f32 borderWidth = 0.f; };
    std::vector<Line> lines;
    std::vector<Dot> dots;
    /// Frame around the whole viewport (frameWidth > 0) and a centred top badge (badge non-empty), widget space:
    /// the Qt play-mode frame (green PLAYING / amber PAUSED).
    Color frame{};
    f32 frameWidth = 0.f;
    std::string badge;
    Color badgeColor{}, badgeText{};
};
void viewportSetOverlay(UiContext& ui, NodeHandle vp, ViewportOverlay overlay);
const ViewportOverlay& viewportOverlay(const UiContext& ui, NodeHandle vp);

/// Code editor: multi-line text with a line-number gutter, syntax colouring (Lua / C-like keywords, strings,
/// numbers, comments), caret + selection with Shift, Tab inserts spaces, auto-indent on Enter, undo (Ctrl+Z),
/// copy / cut / paste, vertical scrolling. text() = the document. Edits emit TextChanged.
enum class CodeLanguage : u8 { Plain, Lua, Cpp, Css };
NodeHandle codeEdit(UiContext& ui, NodeHandle parent, std::string_view text = {}, CodeLanguage lang = CodeLanguage::Lua,
                    std::string_view id = {});
void codeEditSetText(UiContext& ui, NodeHandle ed, std::string_view text);
std::string codeEditText(const UiContext& ui, NodeHandle ed);
u32 codeEditLineCount(const UiContext& ui, NodeHandle ed);
void codeEditCaret(const UiContext& ui, NodeHandle ed, u32& line, u32& column);
void codeEditSetCaret(UiContext& ui, NodeHandle ed, u32 line, u32 column);
/// Token class of the character at (line, column): 0 text, 1 keyword, 2 string, 3 number, 4 comment.
u8 codeEditTokenAt(const UiContext& ui, NodeHandle ed, u32 line, u32 column);

/// Date-time edit: "YYYY-MM-DD HH:MM" in fields; Left / Right pick a field, Up / Down step it (with carry and
/// month-length clamping), digits type into it, the button (or Space) opens a calendar popup. value() = days since
/// 1970-01-01 (exact in f32); dateTimeEditMinutes() = minutes since 1970-01-01 00:00 (civil time, no zone). Changes
/// emit ValueChanged.
NodeHandle dateTimeEdit(UiContext& ui, NodeHandle parent, int year, int month, int day, int hour = 0, int minute = 0,
                        std::string_view id = {});
void dateTimeEditValue(const UiContext& ui, NodeHandle ed, int& year, int& month, int& day, int& hour, int& minute);
void setDateTimeEditValue(UiContext& ui, NodeHandle ed, int year, int month, int day, int hour, int minute);
i64 dateTimeEditMinutes(const UiContext& ui, NodeHandle ed);
u32 dateTimeEditField(const UiContext& ui, NodeHandle ed); ///< 0 year .. 4 minute

/// Video widget: plays a VideoSource (function table). Theora .ogv files: fuse_ui_video's `video::TheoraVideo::source()`
/// (fuse/ui/video/theora_video.hpp); `testPatternSource()` is a built-in moving pattern. Audio sync is not done. Space toggles play, the scrubber seeks.
/// value() = current frame. Frames are pulled from `frame()` into an image the rasteriser shows.
struct VideoSource {
    void* user = nullptr;
    u32 width = 0, height = 0, frames = 0;
    f32 fps = 30.f;
    /// Fills `rgba` (width * height premultiplied RGBA8, R in the low byte) with frame `index`.
    void (*frame)(void* user, u32 index, u32* rgba) = nullptr;
};
const VideoSource& testPatternSource();
NodeHandle videoWidget(UiContext& ui, NodeHandle parent, const VideoSource* source, std::string_view id = {});
/// The rasteriser image index the widget paints (register it with CpuRasterizer::addImage via videoImage()).
const Image* videoImage(const UiContext& ui, NodeHandle video);
void videoSetImageIndex(UiContext& ui, NodeHandle video, u32 index);
void videoPlay(UiContext& ui, NodeHandle video, bool play);
bool videoPlaying(const UiContext& ui, NodeHandle video);
void videoSeek(UiContext& ui, NodeHandle video, u32 frame);
u32 videoFrame(const UiContext& ui, NodeHandle video);
u32 videoDecodedFrames(const UiContext& ui, NodeHandle video);

/// Registers the Tier D (2) classes; install() calls it.
void registerTierD2(UiContext& ui);

// ---- The rest of the plan §5 catalogue: app chrome, header, text widgets ------------------------------------------------
/// Group box (QGroupBox): a framed, titled group; add children to groupBoxBody. `checkable` puts a check box in the
/// title that enables / disables the body (Toggled {flag} on the group; checked() mirrors it).
NodeHandle groupBox(UiContext& ui, NodeHandle parent, std::string_view title, bool checkable = false, bool checked = true,
                    std::string_view id = {});
NodeHandle groupBoxBody(const UiContext& ui, NodeHandle group);

/// Stack (QStackedWidget): pages laid over each other, one displayed. value() = current page; setStackIndex emits
/// ValueChanged.
NodeHandle stack(UiContext& ui, NodeHandle parent, std::string_view id = {});
NodeHandle stackAddPage(UiContext& ui, NodeHandle stack, std::string_view id = {});
void setStackIndex(UiContext& ui, NodeHandle stack, u32 index);
u32 stackIndex(const UiContext& ui, NodeHandle stack);
u32 stackCount(const UiContext& ui, NodeHandle stack);
NodeHandle stackPage(const UiContext& ui, NodeHandle stack, u32 index);

/// Tool bar (QToolBar): a row of tool buttons (Clicked on each action) and separators; any widget can be added to it.
NodeHandle toolBar(UiContext& ui, NodeHandle parent, std::string_view id = {});
NodeHandle toolBarAction(UiContext& ui, NodeHandle bar, std::string_view text, std::string_view tooltip = {}, std::string_view id = {});
NodeHandle toolBarSeparator(UiContext& ui, NodeHandle bar);

/// Status bar (QStatusBar): a message area (statusMessage; cleared after `timeoutSeconds` when > 0) followed by
/// permanent widgets on the right (statusBarPermanent adds a label; any widget can be added to the bar).
NodeHandle statusBar(UiContext& ui, NodeHandle parent, std::string_view id = {});
void statusMessage(UiContext& ui, NodeHandle bar, std::string_view text, f32 timeoutSeconds = 0.f);
std::string_view statusText(const UiContext& ui, NodeHandle bar);
NodeHandle statusBarPermanent(UiContext& ui, NodeHandle bar, std::string_view text, std::string_view id = {});

/// Main window (QMainWindow): menu bar, tool bar, dock area (central widget in its centre region) and status bar in
/// one column that fills its parent.
NodeHandle mainWindow(UiContext& ui, NodeHandle parent, std::string_view id = {});
NodeHandle mainWindowMenuBar(const UiContext& ui, NodeHandle window);
NodeHandle mainWindowToolBar(const UiContext& ui, NodeHandle window);
NodeHandle mainWindowDockArea(const UiContext& ui, NodeHandle window);
NodeHandle mainWindowCentral(const UiContext& ui, NodeHandle window);
NodeHandle mainWindowStatusBar(const UiContext& ui, NodeHandle window);

/// Context menu: a right press on `node` (or Shift+F10 while it has focus) opens a popup menu with `items`
/// (popupMenu syntax). Picking emits Submitted {value = index, text} on `node`. It works on every widget, also on
/// those that use the press themselves: a list / tree / table / grid view first makes the row under the pointer
/// current (keeping a multi-selection the row is part of), a text field moves its caret; a right press never steps a
/// spin box, moves a slider, switches a tab or folds a tree row. An item view with a per-cell hook
/// (setViewContextMenu) calls that for a press on a cell instead; off the cells this menu opens.
void setContextMenu(UiContext& ui, NodeHandle node, std::string_view items);

/// Message box (QMessageBox): a modal dialog with an icon badge, text and buttons ('\n'-separated, default "OK";
/// the first is the default button). Opens at once; a button closes it and emits Submitted {value = button index,
/// text = button} on the returned dialog, whose value() is then that index (-1 while open / dismissed with Escape).
enum class MessageIcon : u8 { None, Info, Warning, Error, Question };
NodeHandle messageBox(UiContext& ui, std::string_view title, std::string_view text, std::string_view buttons = {},
                      MessageIcon icon = MessageIcon::Info, std::string_view id = {});

/// Wizard (QWizard): a modal dialog of pages with Back / Next / Cancel; Next reads Finish on the last page.
/// value() = current page (ValueChanged on change). Finish emits Submitted {flag = true, text = "finish"}, Cancel
/// Submitted {flag = false, text = "cancel"}; both close it. Add content to the page wizardAddPage returns.
enum class WizardButton : u8 { Back, Next, Cancel };
NodeHandle wizard(UiContext& ui, std::string_view title, std::string_view id = {});
NodeHandle wizardAddPage(UiContext& ui, NodeHandle wizard, std::string_view title, std::string_view id = {});
void wizardSetPage(UiContext& ui, NodeHandle wizard, u32 index);
u32 wizardPage(const UiContext& ui, NodeHandle wizard);
u32 wizardPageCount(const UiContext& ui, NodeHandle wizard);
NodeHandle wizardButton(const UiContext& ui, NodeHandle wizard, WizardButton which);
void openWizard(UiContext& ui, NodeHandle wizard); ///< shows page 0

/// Popover: a card anchored under `anchor` in the overlay layer; an outside click or Escape closes it (focus returns
/// to the anchor). Add content to the returned card.
NodeHandle popover(UiContext& ui, NodeHandle anchor, std::string_view id = {});
void closePopover(UiContext& ui, NodeHandle popover);

/// Toast: a transient message at the bottom centre of the window; stays `seconds`, fades out and is
/// destroyed. Toasts stack (newest lowest) and never take input.
NodeHandle toast(UiContext& ui, std::string_view text, f32 seconds = 2.5f, std::string_view id = {});
u32 toastCount(const UiContext& ui);

/// Header (QHeaderView, standalone): sections with titles ('\n'-separated). Clicking a section sorts by it (Toggled
/// {value = section, flag = ascending}); dragging a section's right edge resizes it (ValueChanged {value = section}).
NodeHandle headerView(UiContext& ui, NodeHandle parent, std::string_view titles, f32 sectionWidth = 120.f, std::string_view id = {});
/// Replaces the titles; sections that already exist keep their width, new ones get the creation width.
void setHeaderTitles(UiContext& ui, NodeHandle header, std::string_view titles);
u32 headerSectionCount(const UiContext& ui, NodeHandle header);
f32 headerSectionWidth(const UiContext& ui, NodeHandle header, u32 section);
void setHeaderSectionWidth(UiContext& ui, NodeHandle header, u32 section, f32 px);
i32 headerSortSection(const UiContext& ui, NodeHandle header, bool* ascending = nullptr); ///< -1 = unsorted

/// Text edit (QTextEdit as plain text): multi-line, word wrapped to the widget width. Caret and selection (Shift,
/// drag), Up / Down keep the column across wrapped lines, Home / End act on the visual line (Ctrl: document),
/// Ctrl+A / C / X / V, undo / redo (Ctrl+Z, Ctrl+Y or Ctrl+Shift+Z), IME pre-edit at the caret, wheel scrolling.
/// text() = the document; edits emit TextChanged. Offsets are UTF-8 bytes.
NodeHandle textEdit(UiContext& ui, NodeHandle parent, std::string_view text = {}, std::string_view id = {});
void textEditSetText(UiContext& ui, NodeHandle ed, std::string_view text);
std::string textEditText(const UiContext& ui, NodeHandle ed);
void textEditSetReadOnly(UiContext& ui, NodeHandle ed, bool readOnly);
u32 textEditCaret(const UiContext& ui, NodeHandle ed);
void textEditSetSelection(UiContext& ui, NodeHandle ed, u32 anchor, u32 caret);
u32 textEditVisualLines(UiContext& ui, NodeHandle ed); ///< wrapped line count at the current width (after update)
/// Plain text edit (QPlainTextEdit): unwrapped lines, no colouring, no line-number gutter. It is a code editor
/// (CodeLanguage::Plain): use the codeEdit* accessors.
NodeHandle plainTextEdit(UiContext& ui, NodeHandle parent, std::string_view text = {}, std::string_view id = {});

/// Rich text view: word-wrapped text with inline markup: <b> <i> <u> <s> <color=#RRGGBB[AA]> <size=px>
/// <a href="target"> <br> (and '\n'), entities &lt; &gt; &amp; &quot;. Clicking a link emits Submitted {value =
/// link index, text = target}. <b> / <i> use the bold / italic face of the node's font family when it has one and a
/// synthesised one otherwise.
NodeHandle richText(UiContext& ui, NodeHandle parent, std::string_view markup, std::string_view id = {});
void setRichText(UiContext& ui, NodeHandle view, std::string_view markup);
u32 richTextLinkCount(const UiContext& ui, NodeHandle view);
std::string richTextLink(const UiContext& ui, NodeHandle view, u32 index);
Rect richTextLinkRect(UiContext& ui, NodeHandle view, u32 index); ///< first piece of the link (after update)
std::string richTextPlain(const UiContext& ui, NodeHandle view);  ///< the text without markup

/// Double spin box (QDoubleSpinBox): a spin box with a fractional step (default 0.1).
NodeHandle doubleSpinBox(UiContext& ui, NodeHandle parent, f32 lo, f32 hi, f32 value, f32 step = 0.1f, std::string_view id = {});

/// Registers the classes of this group (header, textedit, richtext, toast, groupbody, wizardpage); install() calls it.
void registerTierB3(UiContext& ui);

// ---- SVG images and icons (Qt SVG / QIcon) --------------------------------------------------------------------------
/// A parsed SVG in the process-wide store (fuse/ui/svg.hpp for the supported subset). Images are immutable; create and
/// destroy them on the UI thread (a widget that still refers to a destroyed image draws nothing).
struct SvgHandle {
    u32 index = 0xFFFFFFFFu;
    u32 generation = 0;
    bool valid() const { return index != 0xFFFFFFFFu; }
    friend bool operator==(const SvgHandle&, const SvgHandle&) = default;
};
std::expected<SvgHandle, SvgError> svgCreate(std::string_view xml);
SvgHandle svgAdopt(SvgImage image);
void svgDestroy(SvgHandle svg);
const SvgImage* svgGet(SvgHandle svg); ///< null for a stale handle
/// A built-in icon (fuse/ui/icons.hpp) as a handle; never destroyed.
SvgHandle iconHandle(Icon icon);

/// SVG image widget: draws `svg` fitted into its content box (aspect ratio kept, centred) as vector paths, so it is
/// sharp at any size and render scale. `currentColor` in the SVG is the node's `color`. `width` / `height` <= 0 leave
/// that side to the style (the intrinsic SVG size by default).
NodeHandle svgImage(UiContext& ui, NodeHandle parent, SvgHandle svg, f32 width = 0.f, f32 height = 0.f, std::string_view id = {});
void setSvgImage(UiContext& ui, NodeHandle node, SvgHandle svg);
SvgHandle svgImageOf(const UiContext& ui, NodeHandle node);

/// Icon on a button / tool button (QAbstractButton::setIcon): drawn `size` px square left of the text - alone and
/// centred when the button has no text - in the button's text colour. An invalid handle removes it.
void setIcon(UiContext& ui, NodeHandle button, SvgHandle svg, f32 size = 16.f);
void setIcon(UiContext& ui, NodeHandle button, Icon icon, f32 size = 16.f);
void clearIcon(UiContext& ui, NodeHandle button);
SvgHandle iconOf(const UiContext& ui, NodeHandle button);
/// Drops the icon records of every node of `ui` (optional tidy-up before destroying a context that used setIcon;
/// records are keyed by UiContext::uid(), so a stale one is never picked up by another context).
void forgetIcons(const UiContext& ui);
/// A tool button showing a built-in icon (and `text` after it); without text its accessible name is the icon's name.
NodeHandle iconButton(UiContext& ui, NodeHandle parent, Icon icon, std::string_view text = {}, std::string_view id = {});

/// Registers `svgimage`; install() calls it.
void registerSvgWidgets(UiContext& ui);

} // namespace fuse::ui::widgets

// Drag and drop for the item views, action menus (QAction) and the row geometry helpers they use.
#include "fuse/ui/widgets/interaction.hpp"
#include "fuse/ui/widgets/modelview.hpp" // model / view: notifications, selection, editing, tree view, async
#include "fuse/ui/widgets/edit_options.hpp" // Qt-parity options: line edit, completer, editable combo, spin box, tabs
#include "fuse/ui/widgets/extra.hpp" // tool box, input dialog, dialog button box, LCD, key sequence, MDI, graphics view, keyboard
