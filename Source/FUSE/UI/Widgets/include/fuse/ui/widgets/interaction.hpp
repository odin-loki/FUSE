// FUSE UI widgets: drag and drop for the item views and menus / buttons driven by actions (plan §4.7, §5).
// Included by widgets.hpp.
#pragma once

#include "fuse/ui/actions.hpp"
#include "fuse/ui/ui.hpp"

#include <string>
#include <string_view>

namespace fuse::ui::widgets {

struct ItemModel;

// ---- Row geometry of the item views (after update) -----------------------------------------------------------------
u32 listCount(const UiContext& ui, NodeHandle list);
Rect listRowRect(UiContext& ui, NodeHandle list, u32 row); ///< may lie outside the view (scrolled out)
i32 listRowAt(UiContext& ui, NodeHandle list, Vec2 p);     ///< -1: no row there
const ItemModel* tableModel(const UiContext& ui, NodeHandle table);
Rect tableRowRect(const UiContext& ui, NodeHandle table, u32 row);
i32 tableRowAt(const UiContext& ui, NodeHandle table, Vec2 p);
const ItemModel* gridModel(const UiContext& ui, NodeHandle grid);
Rect gridItemRect(const UiContext& ui, NodeHandle grid, u32 index);
i32 gridItemAt(const UiContext& ui, NodeHandle grid, Vec2 p);

// ---- Drag and drop in item views (QAbstractItemView::dragDropMode) ---------------------------------------------------
enum ViewDragFlags : u32 {
    kViewDragReorder = 1u << 0, ///< rows can be dragged to another position in the same view (InternalMove)
    kViewDragOut = 1u << 1,     ///< rows can be dragged to other views (DragOnly)
    kViewAcceptDrops = 1u << 2, ///< rows dragged out of other views can be dropped here (DropOnly)
    kViewDragDrop = kViewDragReorder | kViewDragOut | kViewAcceptDrops,
};
/// Enables row dragging on a view; call once per view (flags 0 turns it off again). The drag carries DragData
/// {type = kDragRow, payload[0] = source row, text = the row's text (column 0)}; an insertion line shows where it
/// would land, Escape cancels, Ctrl copies instead of moving. On touch a row drag starts with a long press when the
/// view scrolls (a swipe still scrolls it), otherwise when the finger leaves the slop.
///
/// listView (string items) is self-contained: a reorder moves the line, a drop from another view inserts the dragged
/// text, and a Move that left the view removes the source line. tableView / modelListView / gridView do not own their
/// model: they only report, and the application edits the model (then tableModelChanged / invalidate).
/// Signals (all three kinds of view):
///   RowMoved  {value = from, value2 = to}  on the view: reorder; `to` is the row's index after the move
///   Dropped   {value = insert index, text, event = the Drop event}  on the target view: a row arrived from another view
///   DragEnded {value = source row, flag = accepted, value2 = DropAction}  on the source view, after the target's Dropped
void setListDragDrop(UiContext& ui, NodeHandle list, u32 flags);
void setTableDragDrop(UiContext& ui, NodeHandle table, u32 flags); ///< tableView and modelListView
void setGridDragDrop(UiContext& ui, NodeHandle grid, u32 flags);
/// Where a drop at `p` would insert in the view (0..count); what the insertion line shows.
u32 listDropIndex(UiContext& ui, NodeHandle list, Vec2 p);

// ---- Actions in widgets (QAction) -----------------------------------------------------------------------------------
/// popupMenu item text for an action list: one line per action, "Text\tShortcut", "-" for a separator (an invalid
/// handle); disabled / checked actions carry the state prefixes popupMenu understands (\x01 disabled, \x02 checked).
std::string actionMenuItems(const ActionRegistry& reg, u32 listId);
/// A menu-bar menu whose items are the actions of `listId`: text, shortcut text (right-aligned), enabled and checked
/// state follow the actions; picking an item triggers its action.
NodeHandle actionMenuBarMenu(UiContext& ui, ActionRegistry& reg, NodeHandle bar, std::string_view title, u32 listId, std::string_view id = {});
/// A context / popup menu from an action list at `at`; picking triggers the action. Returns the popup scope.
NodeHandle actionPopupMenu(UiContext& ui, ActionRegistry& reg, NodeHandle owner, u32 listId, Vec2 at, std::string_view id = {});

} // namespace fuse::ui::widgets
