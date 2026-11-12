// Internal glue between the Tier A and Tier B widget sources.
#pragma once

#include "fuse/ui/ui.hpp"

namespace fuse::ui::widgets {
class SelectionModel;
enum class SelectionMode : u8;
} // namespace fuse::ui::widgets
#include <initializer_list>
#include <string_view>
#include <vector>

namespace fuse::ui::widgets::detail {

/// Opens the combo box's popup list (Tier B). False when the Tier B classes are not registered (Tier A cycles).
bool openComboPopup(UiContext& ui, NodeHandle combo);
void registerTierB2(UiContext& ui);
/// Popup menu whose item values start at `base` (submenus report the line index in the root list).
NodeHandle popupMenuAt(UiContext& ui, NodeHandle owner, std::string_view items, Vec2 at, u32 base, std::string_view id = {});
/// A popup scope (outside click / Escape closes it) holding an empty card at `at`; returns the card.
NodeHandle popupPanel(UiContext& ui, NodeHandle owner, Vec2 at);

/// Registers the item view classes "tableview" and "treemodelview" (widgets_modelview.cpp).
void registerModelView(UiContext& ui);
/// Grid view selection (widgets_tier_cd.cpp), behind setViewSelectionMode / setViewSelectionModel: `shared` replaces
/// the grid's selection model (null = a private one); `setMode` also sets the mode. False when `grid` is no grid view.
bool gridSetSelection(UiContext& ui, NodeHandle grid, SelectionModel* shared, bool setMode, SelectionMode mode);
SelectionModel* gridSelection(UiContext& ui, NodeHandle grid);

// widgets_svg.cpp: icons on buttons (setIcon). Both are no-ops (one empty-table test) while no icon is set anywhere.
/// The content size of a button with text size `text`: adds the icon and the gap.
Size iconContentSize(const UiContext& ui, NodeHandle n, Size text);
/// Paints the node's icon (centred with the text as one group); `textBox` receives where the text goes. False when
/// the node has no icon.
bool paintIcon(const PaintContext& pc, DrawList& dl, Rect& textBox);

// ---- accessibility helpers for the class hooks (WidgetClass::a11y / a11yAction) --------------------------------------
constexpr u32 a11yBits(std::initializer_list<A11yAction> actions) {
    u32 m = 0;
    for (A11yAction a : actions) m |= a11yActionBit(a);
    return m;
}
/// What a selectable item (row, tile, tab) accepts.
constexpr u32 kA11yItemActions = a11yBits({A11yAction::Select, A11yAction::Click, A11yAction::ScrollIntoView});
/// A virtual child: `child` is its stable id inside the widget (never 0), `bounds` in context coordinates.
inline A11ySnapshotNode a11yItem(u32 child, A11yRole role, std::string_view name, Rect bounds, u32 actions = 0) {
    A11ySnapshotNode it;
    it.child = child;
    it.role = role;
    it.name.assign(name);
    it.bounds = bounds;
    it.actions = actions;
    return it;
}
/// Drops the core's fallback name (the node text) for a class whose text is its item list or markup.
inline void a11yDropTextName(const UiContext& ui, NodeHandle n, A11ySnapshotNode& self) {
    if (!self.name.empty() && self.name == ui.text(n)) self.name.clear();
}
/// Sets the role and hooks of a registered class (no-op when the class is not registered).
inline void a11yHook(WidgetRegistry& r, std::string_view cls, A11yRole role,
                     void (*a11y)(const UiContext&, NodeHandle, A11ySnapshotNode&, std::vector<A11ySnapshotNode>&) = nullptr,
                     bool (*action)(UiContext&, NodeHandle, u32, A11yAction, f32, std::string_view) = nullptr) {
    const u32 id = r.find(cls);
    if (id == 0xFFFFFFFFu) return;
    WidgetClass& c = r.edit(id);
    c.role = role;
    c.a11y = a11y;
    c.a11yAction = action;
}
/// Per source file: installs the accessibility roles / hooks of the classes it registers (defined at its end).
void a11yTierA(WidgetRegistry& r);
void a11yTierB(WidgetRegistry& r);
void a11yTierB2(WidgetRegistry& r);
void a11yModelView(WidgetRegistry& r); // widgets_modelview.cpp: tableview (table / model list), treemodelview
void a11yTierB3(WidgetRegistry& r);
void a11yTierCD(WidgetRegistry& r);
void a11yTierD2(WidgetRegistry& r);

} // namespace fuse::ui::widgets::detail
