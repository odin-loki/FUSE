// Internal: OpenType font variations (fvar / avar / gvar / HVAR / MVAR and the item variation store shared with the
// COLRv1 renderer). font_var.cpp owns the variation state of a font and its instances; text.cpp reaches it only
// through varHooks(), so binaries that never ask for a variation do not link the glyf / gvar decoder.
//
// Coordinates: "design" values are the user-facing axis values (wght 100..900); "normalised" ones are F2DOT14 after
// the fvar default / min / max mapping and avar, the space every variation table is evaluated in.
#pragma once

#include "glyph_paint.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace fuse::text::detail {

inline constexpr std::size_t kMaxAxes = 16; // axes beyond this stay at their default

// ---- Item variation store (OpenType "Font Variations Common Table Formats"; absolute offsets into `d`) ----
/// Scalar of every region of the store at `ivs` for normalised coordinates `norm`.
void ivsScalars(std::span<const std::uint8_t> d, std::uint32_t ivs, std::span<const std::int16_t> norm, std::vector<float>& out);
/// Delta of item (outer, inner) given the region scalars; 0 when out of range.
float ivsDelta(std::span<const std::uint8_t> d, std::uint32_t ivs, std::uint32_t outer, std::uint32_t inner,
               std::span<const float> scalars);
/// DeltaSetIndexMap lookup (indices past the end use the last entry). False when the map is empty or malformed.
bool deltaSetIndex(std::span<const std::uint8_t> d, std::uint32_t map, std::uint32_t index, std::uint32_t& outer,
                   std::uint32_t& inner);

struct VarState; // font_var.cpp: coordinates, tables and (on the font that holds the file) its instances

/// Variation entry points (set by enableVariableFonts): the Font variation API, and what text.cpp calls for a font whose
/// coordinates are not the default.
struct VarHooks {
    std::span<const VariationAxis> (*axes)(const Font& f) = nullptr;
    std::span<const NamedInstance> (*named)(const Font& f) = nullptr;
    std::span<const float> (*coords)(const Font& f) = nullptr;
    const Font& (*instance)(const Font& f, std::span<const FontVariation> settings) = nullptr;
    void (*setVariation)(Font& f, std::span<const FontVariation> settings) = nullptr;
    void (*destroy)(VarState* v) = nullptr;
    /// Advance in font units (HVAR, else the gvar phantom points).
    float (*advance)(VarState* v, std::uint32_t glyph) = nullptr;
    /// Varied glyf outline in font units.
    bool (*outline)(VarState* v, std::uint32_t glyph, std::vector<OutlineCmd>& out) = nullptr;
    /// Bitmap box and coverage at `scale` (font units -> pixels), like stbtt_GetGlyphBitmapBox / MakeGlyphBitmap.
    void (*glyphBox)(VarState* v, std::uint32_t glyph, float scale, int& x0, int& y0, int& x1, int& y1) = nullptr;
    void (*rasterise)(VarState* v, std::uint32_t glyph, float scale, std::uint8_t* dst, int w, int h, int stride) = nullptr;
};
VarHooks& varHooks();

/// What font_var.cpp may touch of a Font (text.cpp).
struct FontAccess {
    static const Font::State* state(const Font& f);
    static VarState*& var(const Font& f);
    /// The font file bytes (an instance shares its owner's) and the offset of the face in it.
    static std::span<const std::uint8_t> file(const Font& f);
    static std::uint32_t fontStart(const Font& f);
    /// Gives `f` the normalised coordinates `coords` (kept alive by f's VarState; empty: default) and the MVAR metric
    /// deltas in font units. FULL: also HarfBuzz / FreeType.
    static void setVaried(const Font& f, std::span<const std::int16_t> coords, float dAscent, float dDescent, float dLineGap);
    /// A state over `src`'s file (sharing its owner's bytes, colour tables and fallback chain).
    static Font::State* shareState(const Font::State* src);
    /// A new font over `from`'s file (sharing its owner's bytes, colour tables and fallback chain) that owns `v`.
    static std::unique_ptr<Font> makeInstance(const Font& from, VarState* v, std::span<const std::int16_t> coords,
                                              float dAscent, float dDescent, float dLineGap);
};

} // namespace fuse::text::detail
