// fuse_text_cook --reference and the fuse_text_cooked gate: a profile-neutral dump of what a font produces (layout of
// sample strings: glyph ids, pen positions, widths; and the strings rendered from GlyphAtlas coverage). The cooker
// writes it with the MINIMAL font, the gate recomputes it with the cooked font and compares.
#pragma once

#include "fuse/text/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fuse::text::cookref {

inline const std::vector<std::string>& samples() {
    static const std::vector<std::string> s = {
        "The quick brown fox jumps over the lazy dog 0123456789",
        "AVATAR Wave Typography: To, Ty, Yo, LT, \"quoted\" (x) [y] {z} @#%&*!?",
        "\xC3\x89t\xC3\xA9 na\xC3\xAFve \xC3\xA5ngstr\xC3\xB6m \xC3\x9F \xE2\x80\x94 \xE2\x80\xA6",
        "\xD0\xA1\xD1\x8A\xD0\xB5\xD1\x88\xD1\x8C \xD0\xB6\xD0\xB5 \xD0\xB5\xD1\x89\xD1\x91 \xCE\x9E\xCE\xB5\xCF\x83\xCE\xBA\xCE\xB5\xCF\x80\xCE\xAC\xCE\xB6\xCF\x89",
    };
    return s;
}

/// Appends one record per (size, sample): u32 q, f32 width, u32 glyphs, per glyph {u32 id, f32 x}, u32 w, u32 h, then
/// w * h coverage bytes (glyphs blitted with max at pen positions rounded like the UI).
inline void dump(const Font& font, const std::vector<float>& sizes, std::vector<std::uint8_t>& out) {
    auto put = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };
    GlyphAtlas atlas(2048);
    TextLayout tl;
    for (const float px : sizes)
        for (const std::string& s : samples()) {
            TextLayoutDesc d;
            d.pixelSize = px;
            layoutText(font, defaultShaper(), s, d, tl);
            const std::uint32_t q = std::uint32_t(std::lround(px * 4.f)), n = std::uint32_t(tl.glyphs.size());
            put(&q, 4);
            put(&tl.width, 4);
            put(&n, 4);
            for (const ShapedGlyph& g : tl.glyphs) {
                put(&g.glyph, 4);
                put(&g.x, 4);
            }
            const std::uint32_t w = std::uint32_t(std::ceil(tl.width)) + std::uint32_t(px), h = std::uint32_t(std::ceil(tl.height)) + 2;
            std::vector<std::uint8_t> img(std::size_t(w) * h, 0);
            for (const TextLine& ln : tl.lines)
                for (std::uint32_t k = ln.firstGlyph; k < ln.firstGlyph + ln.glyphCount; ++k) {
                    const ShapedGlyph& g = tl.glyphs[k];
                    if (g.glyph == kInvisibleGlyph) continue;
                    const AtlasGlyph a = atlas.get(font, g.glyph, px);
                    const int ox = int(std::lround(g.x)) + a.x0 + int(px / 2), oy = int(std::lround(ln.baseline)) + a.y0 + 1;
                    for (int y = 0; y < a.h; ++y)
                        for (int x = 0; x < a.w; ++x) {
                            const int X = ox + x, Y = oy + y;
                            if (X < 0 || Y < 0 || X >= int(w) || Y >= int(h)) continue;
                            std::uint8_t& dst = img[std::size_t(Y) * w + std::size_t(X)];
                            dst = std::max(dst, atlas.pixels()[std::size_t(a.v + y) * std::size_t(atlas.size()) + a.u + std::size_t(x)]);
                        }
                }
            put(&w, 4);
            put(&h, 4);
            put(img.data(), img.size());
        }
}

} // namespace fuse::text::cookref
