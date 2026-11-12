// FUSE UI built-in icon set: common UI icons authored for this module as SVG (24 x 24 grid, 2 px round strokes in
// currentColor, so they take the text colour and stay sharp at any size / render scale).
#pragma once

#include "fuse/ui/svg.hpp"

namespace fuse::ui {

enum class Icon : u8 {
    ChevronLeft, ChevronRight, ChevronUp, ChevronDown, Close, Check, Search, Folder, File, Save, Undo, Redo, Plus, Minus,
    Settings, Warning, Info, Error, Menu,
    Count
};
inline constexpr u32 kIconCount = u32(Icon::Count);

/// "chevron-left", "close", ... (kebab case).
std::string_view iconName(Icon icon);
bool findIcon(std::string_view name, Icon& out);
/// The icon as a complete SVG document.
std::string_view iconSvg(Icon icon);
/// The parsed icon (parsed on first use, kept for the life of the process; safe from any thread).
const SvgImage& iconImage(Icon icon);
/// Draws the icon fitted into `box` in `color`.
void drawIcon(Painter& p, Icon icon, const Rect& box, Color color);

} // namespace fuse::ui
