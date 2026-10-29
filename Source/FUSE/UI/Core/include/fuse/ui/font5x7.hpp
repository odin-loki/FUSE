// FUSE UI: built-in 5x7 bitmap font (printable ASCII 32..126; AGPL-3.0). A placeholder until fuse_text (UI3)
// provides real fonts: every glyph is a 5x7 cell, advance 6, line height 10, scaled by an integer factor.
#pragma once

#include <cstdint>

namespace fuse::ui {

inline constexpr int kGlyphWidth = 5;
inline constexpr int kGlyphHeight = 7;
inline constexpr int kCellWidth = 6;
inline constexpr int kLineHeight = 10;

/// The 7 rows of `c`, top first; bit 4 is the leftmost column. Characters outside 32..126 draw as '?'.
const std::uint8_t* glyph5x7(char c);

} // namespace fuse::ui
