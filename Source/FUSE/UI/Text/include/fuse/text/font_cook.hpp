// fuse_text font cooker (COOKED text profile): turns a TrueType / OpenType font into a .fusefont pack that the COOKED
// runtime loads with zero font parsing. Pre-shapes nothing; pre-rasterises a glyph set (coverage at listed pixel sizes,
// colour glyph images, optionally MSDFs) and stores metrics, advances, kerning pairs, the cmap subset and the COLR
// layer tables. Deterministic: the same font and options give the same bytes. Tool-side only (MINIMAL / FULL builds;
// fuse_text_cook wraps it).
#pragma once

#include "fuse/text/text.hpp"

#include <expected>
#include <utility>
#include <vector>

namespace fuse::text {

struct CookOptions {
    /// Pixel sizes (Font pixel sizes, quantised to 1/4 px like GlyphAtlas) of the coverage strikes.
    std::vector<float> sizes;
    /// Pixel sizes of the colour glyph images (COLRv1, CBDT / sbix); empty: `sizes`.
    std::vector<float> colorSizes;
    /// Also store an MSDF per glyph (GlyphAtlas::kMsdfPixelSize / kMsdfRange): any other size is drawn from it.
    bool msdf = false;
    /// Code point ranges (inclusive) of the glyph set; empty: every code point the font maps.
    std::vector<std::pair<char32_t, char32_t>> ranges;
    /// Store kerning pairs between the glyphs of the set.
    bool kerning = true;
};

/// Cooks `font` (loaded after enableColorGlyphs() / enableMsdf() when colour images / MSDFs are wanted; the cooker
/// tool enables both). InvalidFont for a cooked font (nothing to cook from).
std::expected<std::vector<std::uint8_t>, TextError> cookFont(const Font& font, const CookOptions& options);

} // namespace fuse::text
