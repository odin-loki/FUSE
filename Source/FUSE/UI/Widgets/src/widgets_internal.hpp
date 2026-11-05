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
/// `dir`: -1 follows the owner's direction, 0 / 1 force left-to-right / right-to-left. A right-to-left popup inherits
/// that direction and takes `at` as the card's top-right corner (aligned to the owner's right edge).
NodeHandle popupPanel(UiContext& ui, NodeHandle owner, Vec2 at, int dir = -1);

// ---- right-to-left helpers (widgets lay out left-to-right, then mirror; events mirror back first) -----------------
inline bool isRtl(const UiContext& ui, NodeHandle n) { return ui.alive(n) && ui.computed(n).rtl; }
/// Left / Right swapped for a right-to-left node (arrow keys move visually).
inline Key visualArrow(const UiContext& ui, NodeHandle n, Key k) {
    if (!isRtl(ui, n)) return k;
    return k == Key::Left ? Key::Right : k == Key::Right ? Key::Left : k;
}
/// A context point in the node's left-to-right space (mirrored in `box` for a right-to-left node).
inline Vec2 ltrPoint(const UiContext& ui, NodeHandle n, Vec2 p, const Rect& box) {
    return isRtl(ui, n) ? Vec2{mirrorX(p.x, box), p.y} : p;
}
/// Per-row font of a list view (the font picker draws each family in its own face): fn(ui, list, row) returns the
/// row's font or nullptr (the list's font). One function per list node; nullptr removes it.
using RowFontFn = const fuse::text::Font* (*)(UiContext& ui, NodeHandle list, u32 row);
void setListRowFont(UiContext& ui, NodeHandle list, RowFontFn fn);

} // namespace fuse::ui::widgets::detail
