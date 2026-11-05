// Internal: the FULL text profile backend (HarfBuzz shaping + FreeType rasterisation). Only compiled when
// FUSE_UI_TEXT=FULL (FUSE_TEXT_FULL=1); text.cpp calls through these hooks so the public API stays profile-neutral.
#pragma once

#include "fuse/text/text.hpp"

namespace fuse::text::detail {

struct FullFace; // hb_face_t + hb_font_t + FT_Face over the Font's byte copy

/// Creates the HarfBuzz / FreeType objects over `bytes` (which must outlive the result). nullptr on failure.
FullFace* fullCreate(std::span<const std::uint8_t> bytes, std::uint32_t faceIndex);
void fullDestroy(FullFace* f);
/// A variation instance of `owner`: its own hb_font, the owner's hb_face / FT_Face / run cache (owner outlives it).
FullFace* fullCreateInstance(FullFace* owner);
/// Normalised (F2DOT14, after avar) coordinates for shaping (HarfBuzz) and rendering (FreeType); empty: default.
void fullSetCoords(FullFace* f, std::span<const std::int16_t> norm);
/// HarfBuzz features applied to every shape call of `f` (part of its run cache key).
void fullSetFeatures(FullFace* f, std::span<const FontFeature> features);
/// Shapes `utf8`; `unitScale` converts font units to pixels. Clears and fills `out`; returns the run width.
float fullShape(FullFace* f, float unitScale, std::string_view utf8, std::vector<ShapedGlyph>& out);
/// FreeType bitmap box / coverage at an em size of `emPx` pixels (unhinted, grayscale).
void fullGlyphBox(FullFace* f, std::uint32_t glyph, float emPx, int& x0, int& y0, int& x1, int& y1);
/// A face of the layout font's fallback chain as fullLayout shapes it: its HarfBuzz face, font units to pixels at its
/// pixel size, and the synthetic bold widening of its spacing glyphs (pixels).
struct FullRunFace {
    FullFace* face = nullptr;
    float unitScale = 0.f, bold = 0.f;
};
/// Bytes [b, e) of the text drawn with fallback face `face` (sorted, disjoint; face 0 everywhere else).
struct FullFaceSpan {
    std::uint32_t b = 0, e = 0;
    std::uint16_t face = 0;
};
/// FULL layout: bidi runs (SheenBidi) split at face spans and each piece shaped once, with its face, through that
/// face's run cache; UAX #14 line breaking (libunibreak) over the real advances, rule L1 trailing whitespace and L2
/// reordering per line. Fills `glyphs` (visual order per line, x from the line start, `face` set) and `lines`
/// (baseline, width, rtl). `faces[0]` is the layout font. `maxWidth` <= 0: no wrapping.
void fullLayout(std::span<const FullRunFace> faces, std::span<const FullFaceSpan> spans, std::string_view utf8,
                float maxWidth, float lineHeight, float ascent, std::vector<ShapedGlyph>& glyphs,
                std::vector<TextLine>& lines, int base = 2); ///< base: 0 LTR, 1 RTL, 2 auto (first strong)
/// libunibreak breaks per byte (LINEBREAK_* values).
void fullLineBreaks(std::string_view utf8, std::vector<LineBreak>& out, bool strict);
/// SheenBidi levels per byte + rule L1 per paragraph; `base`: 0 LTR, 1 RTL, 2 auto.
void fullBidiLevels(std::string_view utf8, int base, std::vector<std::uint8_t>& levels, std::uint8_t& paraLevel);
void fullCacheStats(const FullFace* f, std::uint64_t& hits, std::uint64_t& misses);
void fullRasterise(FullFace* f, std::uint32_t glyph, float emPx, std::uint8_t* dst, int w, int h, int stride);

} // namespace fuse::text::detail
