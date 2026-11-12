// FUSE UI model / view (plan §4.11, the Qt "model/view" row of §5): change notifications, a shareable selection
// model, in-place editing, column management, the virtualised tree view, tree filtering and asynchronous models.
//
// Everything here extends the function-table design of widgets.hpp (ItemModel, CellDelegate, tableView): no
// inheritance, models and delegates are not owned and must outlive the views that show them. tableView,
// modelListView and treeModelView are one view class ("item view" below); the setView* / view* calls work on all
// three, the selection calls also on gridView.
#pragma once

#include "fuse/ui/widgets/widgets.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fuse::ui::widgets {

// ---- change notifications -------------------------------------------------------------------------------------------
/// One change of a model. Rows are child rows of `parent` (kRootItem for a flat model).
struct ModelChange {
    enum class Kind : u8 {
        Reset,        ///< everything may have changed (views keep the scroll position clamped, drop tree expansion)
        RowsInserted, ///< `count` rows now start at `first`
        RowsRemoved,  ///< `count` rows starting at `first` are gone
        RowsMoved,    ///< `count` rows starting at `first` now start at `dest` (an index in the model after the move)
        DataChanged,  ///< rows first .. first + count - 1, columns colFirst .. colLast changed value
    };
    Kind kind = Kind::Reset;
    ItemId parent = kRootItem;
    u32 first = 0, count = 0, dest = 0;
    u32 colFirst = 0, colLast = 0xFFFFFFFFu;
};
/// Subscription list of a model (ItemModel::notifier). The model calls the report functions AFTER it changed its
/// data; views then update incrementally: the selection and the first visible row follow the rows, and a change
/// outside the visible window repaints nothing. Owner-thread only. A view stays subscribed until it is destroyed
/// (the subscription is dropped by the first report after that), so a model must not report changes once the
/// UiContext of its views is gone.
class ModelNotifier {
public:
    /// Returns false to drop the subscription.
    using Fn = bool (*)(void* user, u64 tag, const ModelChange& change);
    u32 subscribe(Fn fn, void* user, u64 tag = 0); ///< returns an id (never 0)
    void unsubscribe(u32 id);
    u32 subscribers() const;

    void rowsInserted(u32 first, u32 count, ItemId parent = kRootItem) { notify({ModelChange::Kind::RowsInserted, parent, first, count}); }
    void rowsRemoved(u32 first, u32 count, ItemId parent = kRootItem) { notify({ModelChange::Kind::RowsRemoved, parent, first, count}); }
    void rowsMoved(u32 first, u32 count, u32 dest, ItemId parent = kRootItem) { notify({ModelChange::Kind::RowsMoved, parent, first, count, dest}); }
    void dataChanged(u32 first, u32 count = 1, u32 colFirst = 0, u32 colLast = 0xFFFFFFFFu, ItemId parent = kRootItem) {
        notify({ModelChange::Kind::DataChanged, parent, first, count, 0, colFirst, colLast});
    }
    void reset() { notify({}); }
    void notify(const ModelChange& change);

private:
    struct Sub { Fn fn = nullptr; void* user = nullptr; u64 tag = 0; u32 id = 0; };
    std::vector<Sub> m_subs;
    u32 m_nextId = 1;
    u32 m_depth = 0;
};

// ---- selection ------------------------------------------------------------------------------------------------------
enum class SelectionMode : u8 {
    Single,   ///< one item; a click or an arrow key selects it
    Multi,    ///< a click or Space toggles an item, arrows only move the current item
    Extended, ///< click selects, Ctrl+click toggles, Shift+click / Shift+arrows select a range, Ctrl+A selects all
    None,     ///< nothing can be selected (the current item still moves)
};
enum class SelectionUnit : u8 { Rows, Cells };
inline constexpr u32 kAllColumns = 0xFFFFFFFFu;
/// An inclusive rectangle of selected cells; a row selection spans c0 = 0 .. c1 = kAllColumns.
struct SelectionRange {
    u32 r0 = 0, r1 = 0, c0 = 0, c1 = kAllColumns;
    friend bool operator==(const SelectionRange&, const SelectionRange&) = default;
};
/// Selection + current item of one or more views (Qt: QItemSelectionModel). The selection is a list of disjoint
/// ranges, so selecting a million rows stores one range. The current item (keyboard focus inside the view) is
/// separate from the selection, as in Qt. Rows are view rows: model rows for a table / list / grid; for a tree view
/// the visible rows, which the tree shifts when nodes expand and collapse (rows that collapse are deselected), so a
/// selection model is shared between views of the same flat model, not between a tree and a table.
class SelectionModel {
public:
    SelectionModel();
    ~SelectionModel();
    SelectionModel(const SelectionModel&) = delete;
    SelectionModel& operator=(const SelectionModel&) = delete;

    // queries
    bool isSelected(u32 row, u32 col = 0) const;
    bool rowSelected(u32 row) const { return isSelected(row, 0); }
    bool empty() const { return m_ranges.empty(); }
    u64 selectedRows() const;  ///< rows with at least one selected cell
    u64 selectedCells(u32 columns) const; ///< cells, counting a row range as `columns` cells per row
    std::span<const SelectionRange> ranges() const { return m_ranges; } ///< disjoint, sorted by (r0, c0)
    i64 currentRow() const { return m_curRow; } ///< -1 = none
    u32 currentColumn() const { return m_curCol; }
    i64 anchorRow() const { return m_anchorRow; }
    u64 version() const { return m_version; } ///< bumps on every change

    // edits (each reports one change)
    void clear();
    void select(const SelectionRange& r);   ///< adds
    void deselect(const SelectionRange& r); ///< removes (splits ranges)
    void selectRow(u32 row) { select({row, row}); }
    void selectRows(u32 first, u32 last) { select({std::min(first, last), std::max(first, last)}); }
    void selectAll(u32 rows) { if (rows) setSelection({0u, rows - 1u}); }
    void toggle(u32 row, u32 col = kAllColumns); ///< kAllColumns = the whole row
    void setSelection(const SelectionRange& r); ///< clear + select as one change
    void setCurrent(i64 row, u32 col = 0);       ///< does not change the selection
    void setAnchor(i64 row, u32 col = 0) { m_anchorRow = row; m_anchorCol = col; }

    /// The interaction rules of SelectionMode, shared by every view: a pointer press on (row, col) with modifier
    /// keys `mods` (kMod*), and keyboard navigation to (row, col). Both make it the current item.
    void pointerSelect(u32 row, u32 col, u8 mods, SelectionMode mode, SelectionUnit unit = SelectionUnit::Rows);
    void keySelect(u32 row, u32 col, u8 mods, SelectionMode mode, SelectionUnit unit = SelectionUnit::Rows);

    /// Row bookkeeping (what a view does on model notifications): ranges and the current / anchor rows follow.
    void rowsInserted(u32 first, u32 count);
    void rowsRemoved(u32 first, u32 count);
    void rowsMoved(u32 first, u32 count, u32 dest);
    /// Follows a flat model's notifier by itself (for a selection shared between views; setViewSelectionModel does
    /// it). The notifier must outlive the selection model, or be untracked first (nullptr).
    void track(ModelNotifier* notifier);
    bool tracking() const { return m_tracked != nullptr; }

    /// Change signal; `fn` returns false to drop the subscription.
    using Fn = bool (*)(void* user, u64 tag, const SelectionModel& sel);
    u32 subscribe(Fn fn, void* user, u64 tag = 0);
    void unsubscribe(u32 id);

private:
    struct Sub { Fn fn = nullptr; void* user = nullptr; u64 tag = 0; u32 id = 0; };
    void changed();
    void addNoNotify(SelectionRange r);
    void removeNoNotify(const SelectionRange& r);
    std::vector<SelectionRange> m_ranges, m_scratch;
    std::vector<Sub> m_subs;
    u32 m_partial = 0; ///< ranges that do not span every column (0: binary search by row)
    i64 m_curRow = -1, m_anchorRow = -1;
    u32 m_curCol = 0, m_anchorCol = 0;
    u64 m_version = 0;
    u32 m_nextId = 1, m_batch = 0, m_depth = 0;
    bool m_dirty = false;
    ModelNotifier* m_tracked = nullptr;
    u32 m_trackId = 0;
};

/// Selection of an item view or a grid view. Views start in SelectionMode::Single with no selection model: value()
/// is the selected row, exactly as before. Any other mode (or a shared model) gives the view a SelectionModel; then
/// value() is the CURRENT row and the model says what is selected. ValueChanged is emitted on every selection or
/// current change.
void setViewSelectionMode(UiContext& ui, NodeHandle view, SelectionMode mode, SelectionUnit unit = SelectionUnit::Rows);
/// Shares `sel` (not owned; nullptr = back to a private one). Cell selection is for tables.
void setViewSelectionModel(UiContext& ui, NodeHandle view, SelectionModel* sel);
SelectionModel* viewSelectionModel(UiContext& ui, NodeHandle view); ///< null while the view is in plain single mode
/// Current item (keyboard cursor). Row -1 = none.
i64 viewCurrentRow(const UiContext& ui, NodeHandle view);
u32 viewCurrentColumn(const UiContext& ui, NodeHandle view);
/// Makes (row, col) current, selects it as a plain click would and scrolls it into view.
void setViewCurrent(UiContext& ui, NodeHandle view, u32 row, u32 col = 0);

// ---- item view: columns, rows, hooks --------------------------------------------------------------------------------
/// Header drag reorders columns (off by default; while on, a header click sorts on release instead of on press).
void setViewColumnsMovable(UiContext& ui, NodeHandle view, bool on);
/// Moves the column at visual position `from` to visual position `to`.
void moveViewColumn(UiContext& ui, NodeHandle view, u32 from, u32 to);
u32 viewColumnAt(const UiContext& ui, NodeHandle view, u32 visual);  ///< model column shown at a visual position
u32 viewColumnVisual(const UiContext& ui, NodeHandle view, u32 col); ///< visual position of a model column
void setViewColumnHidden(UiContext& ui, NodeHandle view, u32 col, bool hidden);
bool viewColumnHidden(const UiContext& ui, NodeHandle view, u32 col);
/// Sizes a column to its header and the cells of the rows shown now (double-clicking a header edge does the same).
void resizeViewColumnToContents(UiContext& ui, NodeHandle view, u32 col);
/// The first `n` visual columns stay in place while the others scroll horizontally. Frozen cells are painted over an
/// opaque backdrop: the view's `background` when it has one, else the default theme's window colour.
void setViewFrozenColumns(UiContext& ui, NodeHandle view, u32 n);
void setViewAlternatingRows(UiContext& ui, NodeHandle view, bool on); ///< default on
/// Row height in px (0 = from the font, the default). Cell text is one line, never wrapped: it is clipped, and a
/// clipped cell shows its full text in a tooltip on hover.
void setViewRowHeight(UiContext& ui, NodeHandle view, f32 px);
void setViewHeaderVisible(UiContext& ui, NodeHandle view, bool on);
f32 viewScrollX(const UiContext& ui, NodeHandle view);
void setViewScrollX(UiContext& ui, NodeHandle view, f32 px);
/// The sort arrow in the header (a header click sets it and emits Toggled; this sets it without emitting).
void setViewSortIndicator(UiContext& ui, NodeHandle view, i32 col, bool ascending);
/// Scrolls so that `row` is the first visible row (clamped).
void setViewFirstVisible(UiContext& ui, NodeHandle view, u32 row);
void viewScrollToRow(UiContext& ui, NodeHandle view, u32 row); ///< minimal scroll that shows `row`
u32 viewVisibleRows(const UiContext& ui, NodeHandle view);      ///< whole rows that fit
u32 viewRowCount(const UiContext& ui, NodeHandle view);
/// Cell rect in context coordinates, clipped to nothing (empty) when the row is not in the visible window or the
/// column is hidden. After update().
Rect viewCellRect(const UiContext& ui, NodeHandle view, u32 row, u32 col);
/// Elided-cell tooltips (on by default): hovering a cell whose text does not fit, or one with a ToolTip role, shows
/// the text after `delay` seconds. The view creates one tooltip node on first use.
void setViewToolTips(UiContext& ui, NodeHandle view, bool on, f32 delay = 0.5f);
NodeHandle viewToolTip(const UiContext& ui, NodeHandle view); ///< the tooltip node (invalid until first shown)
/// Context-menu hook: a right press on a cell makes it current and calls `fn` (row = view row, item = row or tree
/// node id; `at` in context coordinates). Open a menu with popupMenu / contextMenu from it. While a hook is set it
/// answers every right press on a cell; a menu given with setContextMenu opens for presses off the cells (header,
/// empty area), or everywhere when no hook is set.
using ViewContextMenuFn = void (*)(void* user, UiContext& ui, NodeHandle view, u32 row, ItemId item, u32 col, Vec2 at);
void setViewContextMenu(UiContext& ui, NodeHandle view, ViewContextMenuFn fn, void* user = nullptr);
/// Paints Decoration-role icons: `box` is a square at the start of the cell.
using ViewIconPainter = void (*)(void* user, const PaintContext& pc, DrawList& dl, Rect box, i32 icon, Color color);
void setViewIconPainter(UiContext& ui, NodeHandle view, ViewIconPainter fn, void* user = nullptr);
/// Type-ahead search: typing in a focused view moves the current row to the next row whose `col` text starts with
/// the typed prefix (case-insensitive; the prefix restarts after a second without typing). At most
/// kTypeAheadScanRows rows are scanned per key. Cells that open an editor on typing take the key instead.
void setViewTypeAheadColumn(UiContext& ui, NodeHandle view, i32 col); ///< default 0; -1 disables
inline constexpr u32 kTypeAheadScanRows = 65536;

// ---- editing --------------------------------------------------------------------------------------------------------
// A cell is editable when the model has setData and flags() reports kItemEditable for it. The editor is one child
// node of the view placed over the cell (virtualisation is unchanged: it is the only extra node). Enter commits
// (delegate commit -> model setData with ItemRole::Edit), Escape cancels, Tab / Shift+Tab commit and move to the
// next / previous editable cell, a click elsewhere commits. Scrolling the cell out of the window commits too.
// Cells with kItemCheckable and a CheckState role show a check box that a click (or Space) toggles in place through
// setData(ItemRole::CheckState), with no editor.
enum EditTrigger : u32 { kEditDoubleClick = 1, kEditKeyF2 = 2, kEditAnyKey = 4, kEditAll = 7 };
void setViewEditTriggers(UiContext& ui, NodeHandle view, u32 triggers); ///< default kEditAll
/// Opens the editor on a cell; false when it is not editable or not in the visible window.
bool viewEdit(UiContext& ui, NodeHandle view, u32 row, u32 col);
NodeHandle viewEditor(const UiContext& ui, NodeHandle view); ///< the open editor (invalid when none)
bool viewCommitEdit(UiContext& ui, NodeHandle view);         ///< false when the delegate or the model rejected
void viewCancelEdit(UiContext& ui, NodeHandle view);
/// Ready-made editor delegates (paint stays the view's): a line edit (the default), a spin box, a combo box whose
/// committed value is the picked item's text (`number` = its index).
struct SpinEditorSpec { f32 lo = 0.f, hi = 100.f, step = 1.f; };
CellDelegate lineEditDelegate();
CellDelegate spinBoxDelegate(const SpinEditorSpec* spec); ///< `spec` must outlive the view
CellDelegate comboBoxDelegate(const std::string* items);  ///< '\n'-separated; must outlive the view

// ---- tree view ------------------------------------------------------------------------------------------------------
/// Virtualised tree over a hierarchical model (ItemModel::childCount / childAt / itemData). Column 0 carries the
/// indentation and the expander; with a header and more columns it is a tree table. Only expanded nodes are
/// tracked (one record each), so the cost is independent of the number of rows: a node with a million children is
/// one record. Children of lazy models are fetched as their end scrolls into view (canFetchMore / fetchMore).
/// value() = current visible row. Keys: Up / Down / PageUp / PageDown / Home / End, Right expands or steps to the
/// first child, Left collapses or steps to the parent, Enter -> Submitted, typing searches (type-ahead). Expanding
/// or collapsing emits Toggled {value = visible row, flag = open, text = "expand"}; a double click on a row toggles
/// it too. (A header sort click emits Toggled {value = column, flag = ascending, text = "sort"}, in every item view:
/// a tree table tells the two apart by the text.) The header is shown when the model has more than one column and a
/// header callback. A model without a notifier: tableModelChanged(tree) starts over from the collapsed tree.
NodeHandle treeModelView(UiContext& ui, NodeHandle parent, const ItemModel* model, std::string_view id = {});
/// Expansion by visible row, or by item id (the item must be shown, i.e. its ancestors expanded). False when the
/// row / item is not shown or has no children.
bool setTreeRowExpanded(UiContext& ui, NodeHandle tree, u32 row, bool open);
bool setTreeItemExpanded(UiContext& ui, NodeHandle tree, ItemId item, bool open);
bool treeRowExpanded(const UiContext& ui, NodeHandle tree, u32 row);
bool treeItemExpanded(const UiContext& ui, NodeHandle tree, ItemId item);
void treeCollapseAll(UiContext& ui, NodeHandle tree);
ItemId treeItemAt(const UiContext& ui, NodeHandle tree, u32 row); ///< kRootItem when out of range
u32 treeRowDepth(const UiContext& ui, NodeHandle tree, u32 row);
/// Visible row of an item whose parent is expanded (-1 otherwise). Costs one childAt scan of the parent's rows.
i64 treeRowOf(const UiContext& ui, NodeHandle tree, ItemId item, ItemId parent);
u32 treeExpandedCount(const UiContext& ui, NodeHandle tree); ///< expanded nodes tracked (the view's whole state)

/// A ready-made tree model that owns its cells (the tree counterpart of StringTableModel). Reports every change
/// through its notifier, so views update incrementally. Ids are stable handles, never reused.
class StringTreeModel {
public:
    explicit StringTreeModel(std::string_view headersTabSeparated);
    StringTreeModel(const StringTreeModel&) = delete;
    StringTreeModel& operator=(const StringTreeModel&) = delete;
    /// Adds a node under `parent` at `row` (default: last). Returns its id.
    ItemId add(ItemId parent, std::string_view cellsTabSeparated, u32 row = 0xFFFFFFFFu);
    void remove(ItemId item); ///< with its subtree
    void setCell(ItemId item, u32 col, std::string_view text);
    std::string_view cell(ItemId item, u32 col) const;
    u32 childCount(ItemId parent) const;
    ItemId childAt(ItemId parent, u32 row) const;
    ItemId parentOf(ItemId item) const;
    u32 size() const { return m_live; }
    void setEditable(bool on) { m_editable = on; }
    const ItemModel& model() const { return m_model; }
    ModelNotifier& notifier() { return m_notifier; }

private:
    struct Node { ItemId parent = kRootItem; std::vector<ItemId> kids; std::vector<std::string> cells; bool live = false; };
    const Node* get(ItemId id) const { return id < m_nodes.size() && (id == kRootItem || m_nodes[id].live) ? &m_nodes[id] : nullptr; }
    std::vector<Node> m_nodes; ///< index = id; [0] = root
    std::vector<std::string> m_headers;
    u32 m_cols = 1, m_live = 0;
    bool m_editable = false;
    ItemModel m_model;
    ModelNotifier m_notifier;
};

/// Tree-aware filter: keeps the nodes that match and every ancestor of a match (Qt: recursive filtering). Ids are
/// the source ids. Matching is a case-insensitive substring over `col` (col < 0 = any column). Building the filter
/// walks the whole source tree once (it reads what the source has fetched; it never calls fetchMore), so it is for
/// trees that fit in memory, not for unbounded lazy ones. With an empty needle it is the source, at no cost.
class TreeFilterProxy {
public:
    explicit TreeFilterProxy(const ItemModel* source);
    ~TreeFilterProxy();
    TreeFilterProxy(const TreeFilterProxy&) = delete;
    TreeFilterProxy& operator=(const TreeFilterProxy&) = delete;
    void setFilter(i32 col, std::string_view needle);
    void invalidate(); ///< after the source changed (automatic when the source has a notifier)
    bool matches(ItemId item) const; ///< the node itself matches (as opposed to being kept for a descendant)
    u32 keptCount() const { return m_kept; }
    const ItemModel& model() const { return m_model; }
    ModelNotifier& notifier() { return m_notifier; }

private:
    bool build(ItemId parent, std::string& scratch, std::string& low, u32 depth);
    const ItemModel* m_src;
    ItemModel m_model;
    ModelNotifier m_notifier;
    std::unordered_map<ItemId, std::vector<ItemId>> m_children; ///< kept children of each kept parent
    std::unordered_map<ItemId, bool> m_match;
    std::string m_needle;
    i32 m_col = -1;
    u32 m_kept = 0, m_srcSub = 0;
};

// ---- asynchronous model ---------------------------------------------------------------------------------------------
/// A flat model whose rows arrive later (plan §4.11). The view reads a row; when its page is not cached the model
/// serves placeholder cells and asks the source for the page ONCE through `fetch`. The source answers with
/// deliver(), from any thread, now or later: the page is queued under a lock, a command is posted to the owning
/// context (UiContext::post) and the page is committed on the owner thread at the start of its next update, where
/// dataChanged is reported for exactly the delivered rows (a view whose window does not show them repaints nothing).
/// Pages are cached up to `maxPages` (least recently used are dropped and re-fetched on demand).
class AsyncModel {
public:
    /// Asked on the owner thread for rows first .. first + count - 1. Answer with deliver(ticket, ...) at any time
    /// from any thread; an answer for a ticket that was invalidated meanwhile is dropped.
    using FetchFn = void (*)(void* user, AsyncModel& model, u32 first, u32 count, u64 ticket);
    AsyncModel(UiContext& ui, u32 columns, u32 pageRows = 64, u32 maxPages = 64);
    ~AsyncModel();
    AsyncModel(const AsyncModel&) = delete;
    AsyncModel& operator=(const AsyncModel&) = delete;

    void setSource(FetchFn fetch, void* user);
    void setHeaders(std::string_view headersTabSeparated);
    void setPlaceholder(std::string_view text) { m_placeholder.assign(text); } ///< default "..."
    /// Sets the row count and drops every cached and pending page (reports a reset).
    void setRowCount(u32 rows);
    void invalidate() { setRowCount(m_rows); }

    /// Any thread. `cells` holds count x columns strings, row-major (moved from). Returns false when the ticket is
    /// stale or the model is gone.
    bool deliver(u64 ticket, std::vector<std::string>&& cells);
    /// Owner thread: commits delivered pages now (update() does it through the posted command; call this when the
    /// context's command queue was full). Returns the number of pages committed.
    u32 poll();

    u32 rows() const { return m_rows; }
    bool loaded(u32 row) const;
    u32 pendingRequests() const { return m_pending; }
    u32 cachedPages() const { return u32(m_pages.size()); }
    u64 fetchCount() const { return m_fetches; }
    const ItemModel& model() const { return m_model; }
    ModelNotifier& notifier() { return m_notifier; }

private:
    struct Page { u32 index = 0; u64 stamp = 0; bool loaded = false; std::vector<std::string> cells; };
    struct Inbox { u64 ticket = 0; std::vector<std::string> cells; };
    Page* find(u32 page);
    const Page* find(u32 page) const;
    std::string_view cell(u32 row, u32 col);
    static void commitCmd(UiContext& ui, NodeHandle, u64 id, u64);
    UiContext& m_ui;
    ItemModel m_model;
    ModelNotifier m_notifier;
    FetchFn m_fetch = nullptr;
    void* m_fetchUser = nullptr;
    std::vector<std::string> m_headers;
    std::string m_placeholder = "...";
    std::vector<Page> m_pages; ///< at most m_maxPages
    u32 m_cols, m_pageRows, m_maxPages, m_rows = 0, m_pending = 0;
    u32 m_generation = 1;
    u64 m_stamp = 0, m_fetches = 0, m_id = 0;
    std::mutex m_mutex; ///< guards m_inbox
    std::vector<Inbox> m_inbox;
    std::atomic<u32> m_liveGeneration{1};
};

} // namespace fuse::ui::widgets
