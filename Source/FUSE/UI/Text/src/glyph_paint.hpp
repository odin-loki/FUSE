// Internal: colour glyph and MSDF rasterisers of fuse_text (exact COLRv1, CBDT / sbix bitmap strikes, multi-channel
// signed distance fields). Each lives in its own source file and is reached through paintHooks(), so a binary only
// links the rasterisers it enables (fuse::text::enableColorGlyphs / enableMsdf, paint_enable.cpp).
//
// Conventions shared by every rasteriser here:
// - Outlines are in font units, y up (as stored in glyf / CFF).
// - Bitmaps are in device pixels, y down. A bitmap's (x0, y0) is the offset of its top-left texel from the pen
//   position on the baseline, the same as fuse::text::AtlasGlyph.
// - "scale" converts font units to pixels at the size being rendered (Font::unitScale).
#pragma once

#include "fuse/text/text.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace fuse::text::detail {

/// One outline command (stb_truetype's vertex list as floats). Move starts a contour; Line / Quad / Cubic go from the
/// previous point to (x, y); Quad uses (cx, cy), Cubic (cx, cy) then (cx1, cy1). Contours are closed implicitly.
struct OutlineCmd {
    enum Kind : std::uint8_t { Move, Line, Quad, Cubic };
    Kind kind = Move;
    float x = 0.f, y = 0.f, cx = 0.f, cy = 0.f, cx1 = 0.f, cy1 = 0.f;
};

/// Glyph outline in font units (false when the glyph has none). Not available for cooked fonts.
bool glyphOutline(const Font& font, std::uint32_t glyph, std::vector<OutlineCmd>& out);

/// 2D affine map: x' = xx * x + xy * y + dx, y' = yx * x + yy * y + dy.
struct Affine {
    double xx = 1, yx = 0, xy = 0, yy = 1, dx = 0, dy = 0;
    /// this ∘ o (apply o first).
    Affine operator*(const Affine& o) const {
        return {xx * o.xx + xy * o.yx, yx * o.xx + yy * o.yx, xx * o.xy + xy * o.yy, yx * o.xy + yy * o.yy,
                xx * o.dx + xy * o.dy + dx, yx * o.dx + yy * o.dy + dy};
    }
};

/// A premultiplied RGBA8 bitmap (row stride w * 4), placed like an atlas glyph.
using ColorBitmap = ColorImage;

/// What the COLRv1 renderer reads: the font file, the COLR / CPAL table offsets and the outline source.
struct ColrSource {
    const Font* font = nullptr;
    std::span<const std::uint8_t> data; ///< whole font file
    std::uint32_t colr = 0, cpal = 0;   ///< absolute table offsets (0: absent)
    float scale = 0.f;                  ///< font units -> pixels
    std::span<const std::int16_t> coords; ///< normalised variation coordinates (F2DOT14); empty: Var* paints at defaults
};

/// What the bitmap strike reader reads.
struct BitmapSource {
    std::span<const std::uint8_t> data;           ///< whole font file
    std::uint32_t cblc = 0, cbdt = 0, sbix = 0;   ///< absolute table offsets (0: absent)
    std::uint16_t unitsPerEm = 1000;
    std::uint16_t numGlyphs = 0;
};

/// The pluggable rasterisers (null until enabled).
struct PaintHooks {
    bool colorEnabled = false, msdfEnabled = false; ///< enableColorGlyphs / enableMsdf called (COOKED: only these)
    /// Renders COLRv1 base glyph `glyph` (the whole paint graph: gradients, transforms, clip box, composites).
    /// `foreground` is the text colour (straight RGBA, R in the low byte) for palette index 0xFFFF.
    bool (*colrRender)(const ColrSource& src, std::uint32_t glyph, std::uint32_t foreground, ColorBitmap& out) = nullptr;
    /// Renders the best bitmap strike (CBDT / sbix) of `glyph` scaled to `ppem` pixels per em.
    bool (*bitmapRender)(const BitmapSource& src, std::uint32_t glyph, float ppem, ColorBitmap& out) = nullptr;
    /// Glyph ids with a bitmap (read when a font is loaded).
    void (*bitmapList)(const BitmapSource& src, std::vector<std::uint32_t>& out) = nullptr;
    /// Multi-channel signed distance field of `path` (font units) mapped by `toPixels` into a w x h RGBA8 bitmap with
    /// row stride `stride`: texel (x, y) samples the point whose image is (x + 0.5, y + 0.5). RGB hold the
    /// per-channel signed pseudo-distances, A the true signed distance, all as 255 * (0.5 + d / range) clamped
    /// (inside > 127.5). `range` is in pixels.
    void (*msdf)(std::span<const OutlineCmd> path, const Affine& toPixels, float range, std::uint8_t* rgba, int w, int h,
                 int stride) = nullptr;
};
PaintHooks& paintHooks();

// ---- implementations (each in its own source file) ----
/// colr_paint.cpp
bool colrV1Render(const ColrSource& src, std::uint32_t glyph, std::uint32_t foreground, ColorBitmap& out);
/// Glyph ids of COLRv1 BaseGlyphList records (sorted).
void colrV1Glyphs(const ColrSource& src, std::vector<std::uint32_t>& out);
/// color_bitmap.cpp
bool bitmapGlyphRender(const BitmapSource& src, std::uint32_t glyph, float ppem, ColorBitmap& out);
/// Glyph ids that have a bitmap in some strike (sorted, unique).
void bitmapGlyphs(const BitmapSource& src, std::vector<std::uint32_t>& out);
/// msdf.cpp
void generateMsdf(std::span<const OutlineCmd> path, const Affine& toPixels, float range, std::uint8_t* rgba, int w, int h,
                  int stride);

// ---- font cooker access (text.cpp, MINIMAL / FULL builds) ----
struct CookSource {
    int ascent = 0, descent = 0, lineGap = 0; ///< font units (hhea)
    float unitsPerPixelScale1 = 0.f, emPerPixelSize = 1.f;
    std::uint16_t unitsPerEm = 1000, numGlyphs = 0;
};
CookSource cookSource(const Font& font);
int advanceUnits(const Font& font, std::uint32_t glyph);
int kernUnits(const Font& font, std::uint32_t l, std::uint32_t r);
bool hasKerning(const Font& font);
/// COLRv0 layers + flattened COLRv1 (what colorLayers returns without colour images).
void rawColorTables(const Font& font, std::vector<ColorLayer>& layers, std::vector<std::array<std::uint32_t, 3>>& base);
/// Glyphs drawn as one RGBA image (COLRv1, bitmap strikes), sorted.
void imageGlyphs(const Font& font, std::vector<std::uint32_t>& out);

/// path_raster.cpp: non-zero winding coverage (0..1, exact area) of `path` mapped by `toPixels`, written into a w x h
/// float buffer (pixel (x, y) covers [x, x + 1) x [y, y + 1) in mapped space). Overwrites `coverage`.
void rasterisePath(std::span<const OutlineCmd> path, const Affine& toPixels, float* coverage, int w, int h);

/// Insertion sort of a few values (curve split parameters). std::sort on these short arrays trips GCC 13's
/// -Warray-bounds at -O2 (a false positive in its unguarded insertion step; -Werror builds such as MinGW).
inline void sortSmall(double* v, std::size_t n) {
    for (std::size_t i = 1; i < n; ++i) {
        const double x = v[i];
        std::size_t j = i;
        for (; j > 0 && x < v[j - 1]; --j) v[j] = v[j - 1];
        v[j] = x;
    }
}

} // namespace fuse::text::detail
