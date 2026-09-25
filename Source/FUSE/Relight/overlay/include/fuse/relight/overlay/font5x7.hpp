// FUSE Relight RL-6.1: the developer overlay's bitmap font (FUSE's own 5x7 glyphs, printable ASCII 32..126; MIT).
#pragma once

#include <cstdint>

namespace fuse::relight::overlay {

inline constexpr int kGlyphWidth = 5;
inline constexpr int kGlyphHeight = 7;
inline constexpr int kCellWidth = 6;   ///< advance per character (unscaled)
inline constexpr int kLineHeight = 10; ///< advance per line (unscaled): glyph + 3 rows of padding

/// The 7 rows of `c`, top first; bit 4 is the leftmost column. Characters outside 32..126 draw as '?'.
const std::uint8_t* glyph5x7(char c);

} // namespace fuse::relight::overlay
