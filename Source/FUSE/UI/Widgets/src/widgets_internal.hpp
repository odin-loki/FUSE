// Internal glue between the Tier A and Tier B widget sources.
#pragma once

#include "fuse/ui/ui.hpp"

namespace fuse::ui::widgets::detail {

/// Opens the combo box's popup list (Tier B). False when the Tier B classes are not registered (Tier A cycles).
bool openComboPopup(UiContext& ui, NodeHandle combo);
void registerTierB2(UiContext& ui);
/// Popup menu whose item values start at `base` (submenus report the line index in the root list).
NodeHandle popupMenuAt(UiContext& ui, NodeHandle owner, std::string_view items, Vec2 at, u32 base, std::string_view id = {});
/// A popup scope (outside click / Escape closes it) holding an empty card at `at`; returns the card.
NodeHandle popupPanel(UiContext& ui, NodeHandle owner, Vec2 at);

/// Completer hooks from the line edit: a KeyDown while focused (true = consumed), a user edit, focus loss.
bool completerKey(UiContext& ui, NodeHandle edit, const Event& e);
void completerEdited(UiContext& ui, NodeHandle edit, bool deletion);
void completerFocusOut(UiContext& ui, NodeHandle edit);
/// Sets a line edit's text from the completer (TextChanged, no refilter); selects [selFrom, end) unless npos.
void lineEditComplete(UiContext& ui, NodeHandle edit, std::string_view text, std::size_t selFrom);

struct ItemSelection;
/// Tree view selection state (item_selection.hpp) for the shared selection API in widgets_tier_b2.cpp.
ItemSelection* treeSelection(const UiContext& ui, NodeHandle tree);
u32 treeLineCount(const UiContext& ui, NodeHandle tree);
void treeSelectionChanged(UiContext& ui, NodeHandle tree);

} // namespace fuse::ui::widgets::detail
