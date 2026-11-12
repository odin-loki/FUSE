// Internal glue between the Tier A and Tier B widget sources.
#pragma once

#include "fuse/ui/ui.hpp"

namespace fuse::ui::widgets {
class SelectionModel;
enum class SelectionMode : u8;
} // namespace fuse::ui::widgets

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

} // namespace fuse::ui::widgets::detail
