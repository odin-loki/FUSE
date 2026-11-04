// Internal: the COOKED text profile backend (text_cooked.cpp). Only compiled when FUSE_UI_TEXT=COOKED
// (FUSE_TEXT_COOKED=1); text.cpp calls through these so the public API stays profile-neutral.
#pragma once

#include "fuse/text/text.hpp"

#include <array>
#include <span>
#include <vector>

namespace fuse::text::detail {

struct CookedFace; // a parsed view over a .fusefont pack (the Font's byte copy)

struct CookedInfo {
    int ascent = 0, descent = 0, lineGap = 0; ///< font units
    float unitsPerPixelScale1 = 0.f, emPerPixelSize = 1.f;
    std::uint16_t unitsPerEm = 1000, numGlyphs = 0;
};

/// Validates the pack's header and tables (nullptr when malformed); `bytes` must outlive the result.
CookedFace* cookedCreate(std::span<const std::uint8_t> bytes);
void cookedDestroy(CookedFace* f);
const CookedInfo& cookedInfo(const CookedFace* f);
std::uint32_t cookedGlyphIndex(const CookedFace* f, char32_t cp);
int cookedAdvance(const CookedFace* f, std::uint32_t glyph);          ///< font units
int cookedKern(const CookedFace* f, std::uint32_t l, std::uint32_t r); ///< font units
/// Cooked sizes: the stored strike; others: generated from the MSDF or the nearest strike.
void cookedGlyphBox(const CookedFace* f, std::uint32_t glyph, float px, int& x0, int& y0, int& x1, int& y1);
void cookedRasterise(const CookedFace* f, std::uint32_t glyph, float px, std::uint8_t* dst, int w, int h, int stride);
/// The glyph's colour image depends on the text colour (COLRv1 foreground).
bool cookedForeground(const CookedFace* f, std::uint32_t glyph);
bool cookedColor(const CookedFace* f, std::uint32_t glyph, float px, std::uint32_t foreground, ColorImage& out);
bool cookedMsdf(const CookedFace* f, std::uint32_t glyph, float px, float range, ColorImage& out);
/// The COLR layer tables (as Font::State keeps them) and the image glyphs.
void cookedColorTables(const CookedFace* f, std::vector<ColorLayer>& layers, std::vector<std::array<std::uint32_t, 3>>& base,
                       std::vector<ColorLayer>& images);

} // namespace fuse::text::detail
