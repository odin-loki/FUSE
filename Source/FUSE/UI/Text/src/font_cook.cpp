// The .fusefont cooker (fusefont.hpp): everything the COOKED runtime needs, rendered by the MINIMAL code paths so
// cooked glyphs are byte-identical to MINIMAL at the cooked sizes.
#include "fuse/text/font_cook.hpp"

#include "fusefont.hpp"
#include "glyph_paint.hpp"

#include <algorithm>
#include <cmath>

namespace fuse::text {

namespace {
using namespace detail::fusefont;

struct Writer {
    std::vector<std::uint8_t> b;
    template <class T> void put(T v) {
        const std::size_t at = b.size();
        b.resize(at + sizeof v);
        std::memcpy(b.data() + at, &v, sizeof v);
    }
    void bytes(const std::uint8_t* p, std::size_t n) { b.insert(b.end(), p, p + n); }
    void align4() {
        while (b.size() & 3) b.push_back(0);
    }
};

struct Bitmap {
    std::uint32_t glyph;
    int x0, y0, w, h;
    std::vector<std::uint8_t> texels;
};

// One strike section: count, headers, then per strike records + data.
struct StrikeData {
    std::uint32_t q;
    float range;
    std::vector<Bitmap> glyphs; // sorted by glyph
};

std::vector<std::uint8_t> strikeSection(const std::vector<StrikeData>& strikes) {
    Writer w;
    w.put(std::uint32_t(strikes.size()));
    const std::size_t headers = w.b.size();
    w.b.resize(headers + strikes.size() * kStrikeHeader);
    for (std::size_t i = 0; i < strikes.size(); ++i) {
        const StrikeData& s = strikes[i];
        w.align4();
        const std::uint32_t rec = std::uint32_t(w.b.size());
        std::uint32_t off = 0;
        for (const Bitmap& g : s.glyphs) {
            w.put(g.glyph);
            w.put(std::int16_t(g.x0));
            w.put(std::int16_t(g.y0));
            w.put(std::uint16_t(g.w));
            w.put(std::uint16_t(g.h));
            w.put(off);
            off += std::uint32_t(g.texels.size());
        }
        const std::uint32_t data = std::uint32_t(w.b.size());
        for (const Bitmap& g : s.glyphs) w.bytes(g.texels.data(), g.texels.size());
        std::uint8_t* h = w.b.data() + headers + i * kStrikeHeader;
        const std::uint32_t n = std::uint32_t(s.glyphs.size());
        std::memcpy(h, &s.q, 4);
        std::memcpy(h + 4, &s.range, 4);
        std::memcpy(h + 8, &n, 4);
        std::memcpy(h + 12, &rec, 4);
        std::memcpy(h + 16, &data, 4);
    }
    return std::move(w.b);
}

std::uint32_t quant(float px) { return std::uint32_t(std::lround(px * 4.f)); }
} // namespace

std::expected<std::vector<std::uint8_t>, TextError> cookFont(const Font& font, const CookOptions& opt) {
    if (!font.valid() || textProfile() == TextProfile::Cooked) return std::unexpected(TextError::InvalidFont);
    const detail::CookSource src = detail::cookSource(font);

    // cmap subset
    std::vector<std::pair<std::uint32_t, std::uint32_t>> cmap;
    auto scan = [&](char32_t a, char32_t b) {
        for (char32_t cp = a; cp <= b && cp <= 0x10FFFF; ++cp)
            if (const std::uint32_t g = font.glyphIndex(cp)) cmap.push_back({std::uint32_t(cp), g});
    };
    if (opt.ranges.empty()) scan(0, 0x10FFFF);
    else
        for (const auto& r : opt.ranges) scan(r.first, r.second);
    std::sort(cmap.begin(), cmap.end());
    cmap.erase(std::unique(cmap.begin(), cmap.end(), [](auto& x, auto& y) { return x.first == y.first; }), cmap.end());
    if (cmap.empty()) return std::unexpected(TextError::NoGlyphs);

    // glyph sets: mapped glyphs (+ .notdef), and for coverage also the COLR layer glyphs they paint with
    std::vector<ColorLayer> layers;
    std::vector<std::array<std::uint32_t, 3>> base;
    detail::rawColorTables(font, layers, base);
    std::vector<std::uint32_t> mapped{0};
    for (const auto& e : cmap) mapped.push_back(e.second);
    std::sort(mapped.begin(), mapped.end());
    mapped.erase(std::unique(mapped.begin(), mapped.end()), mapped.end());
    std::vector<std::uint32_t> coverage = mapped;
    std::vector<std::array<std::uint32_t, 3>> keptBase;
    std::vector<ColorLayer> keptLayers;
    for (const auto& b : base) {
        if (!std::binary_search(mapped.begin(), mapped.end(), b[0])) continue;
        keptBase.push_back({b[0], std::uint32_t(keptLayers.size()), b[2]});
        for (std::uint32_t k = 0; k < b[2]; ++k) {
            keptLayers.push_back(layers[b[1] + k]);
            coverage.push_back(layers[b[1] + k].glyph);
        }
    }
    std::sort(coverage.begin(), coverage.end());
    coverage.erase(std::unique(coverage.begin(), coverage.end()), coverage.end());
    std::vector<std::uint32_t> images;
    detail::imageGlyphs(font, images);
    std::erase_if(images, [&](std::uint32_t g) { return !std::binary_search(mapped.begin(), mapped.end(), g); });

    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> sections; // tag, payload
    {
        Writer w;
        w.put(std::uint32_t(cmap.size()));
        for (const auto& e : cmap) {
            w.put(e.first);
            w.put(e.second);
        }
        sections.push_back({tag("CMAP"), std::move(w.b)});
    }
    {
        Writer w;
        w.put(std::uint32_t(src.numGlyphs));
        for (std::uint32_t g = 0; g < src.numGlyphs; ++g) w.put(std::uint16_t(std::clamp(detail::advanceUnits(font, g), 0, 0xFFFF)));
        sections.push_back({tag("HMTX"), std::move(w.b)});
    }
    if (opt.kerning && detail::hasKerning(font)) {
        Writer w;
        w.put(std::uint32_t(0));
        std::uint32_t n = 0;
        for (const std::uint32_t l : mapped)
            for (const std::uint32_t r : mapped)
                if (const int k = detail::kernUnits(font, l, r)) {
                    w.put(std::uint16_t(l));
                    w.put(std::uint16_t(r));
                    w.put(std::int16_t(std::clamp(k, -32768, 32767)));
                    w.put(std::uint16_t(0));
                    ++n;
                }
        std::memcpy(w.b.data(), &n, 4);
        sections.push_back({tag("KERN"), std::move(w.b)});
    }
    if (!keptBase.empty()) {
        Writer w;
        w.put(std::uint32_t(keptBase.size()));
        for (const auto& b : keptBase) {
            w.put(b[0]);
            w.put(b[1]);
            w.put(b[2]);
        }
        w.put(std::uint32_t(keptLayers.size()));
        for (const ColorLayer& l : keptLayers) {
            w.put(l.glyph);
            const std::uint8_t c[8] = {l.r, l.g, l.b, l.a, std::uint8_t(l.foreground ? 1 : 0), 0, 0, 0};
            w.bytes(c, 8);
        }
        sections.push_back({tag("COLR"), std::move(w.b)});
    }
    std::vector<float> sizes = opt.sizes;
    std::sort(sizes.begin(), sizes.end(), [](float a, float b) { return quant(a) < quant(b); });
    sizes.erase(std::unique(sizes.begin(), sizes.end(), [](float a, float b) { return quant(a) == quant(b); }), sizes.end());
    if (!sizes.empty()) {
        std::vector<StrikeData> strikes;
        for (const float px : sizes) {
            StrikeData sd{quant(px), 0.f, {}};
            const float pxq = float(sd.q) / 4.f;
            for (const std::uint32_t g : coverage) {
                int x0, y0, x1, y1;
                font.glyphBox(g, pxq, x0, y0, x1, y1);
                Bitmap bm{g, x0, y0, std::max(x1 - x0, 0), std::max(y1 - y0, 0), {}};
                if (bm.w > 0 && bm.h > 0) {
                    bm.texels.assign(std::size_t(bm.w) * std::size_t(bm.h), 0);
                    font.rasterise(g, pxq, bm.texels.data(), bm.w, bm.h, bm.w);
                } else {
                    bm.w = bm.h = 0;
                }
                sd.glyphs.push_back(std::move(bm));
            }
            strikes.push_back(std::move(sd));
        }
        sections.push_back({tag("COVR"), strikeSection(strikes)});
    }
    std::vector<float> colorSizes = opt.colorSizes.empty() ? sizes : opt.colorSizes;
    std::sort(colorSizes.begin(), colorSizes.end(), [](float a, float b) { return quant(a) < quant(b); });
    colorSizes.erase(std::unique(colorSizes.begin(), colorSizes.end(), [](float a, float b) { return quant(a) == quant(b); }),
                     colorSizes.end());
    if (!images.empty() && colorGlyphsEnabled() && !colorSizes.empty()) {
        std::vector<StrikeData> strikes;
        ColorImage black, white;
        for (const float px : colorSizes) {
            StrikeData sd{quant(px), 0.f, {}};
            const float pxq = float(sd.q) / 4.f;
            for (const std::uint32_t g : images) {
                if (!font.renderColor(g, pxq, 0xFF000000u, black) || !font.renderColor(g, pxq, 0xFFFFFFFFu, white)) continue;
                Bitmap bm{g, black.x0, black.y0, black.w, black.h, std::move(black.rgba)};
                // text-colour dependent (COLRv1 foreground): keep both renders, the runtime mixes them
                if (white.rgba != bm.texels) {
                    if (white.w != bm.w || white.h != bm.h) continue;
                    bm.glyph |= kForegroundBit;
                    bm.texels.insert(bm.texels.end(), white.rgba.begin(), white.rgba.end());
                }
                sd.glyphs.push_back(std::move(bm));
            }
            strikes.push_back(std::move(sd));
        }
        sections.push_back({tag("RGBA"), strikeSection(strikes)});
    }
    if (opt.msdf && msdfEnabled()) {
        StrikeData sd{quant(GlyphAtlas::kMsdfPixelSize), GlyphAtlas::kMsdfRange, {}};
        ColorImage img;
        for (const std::uint32_t g : coverage)
            if (font.renderMsdf(g, GlyphAtlas::kMsdfPixelSize, GlyphAtlas::kMsdfRange, img))
                sd.glyphs.push_back({g, img.x0, img.y0, img.w, img.h, std::move(img.rgba)});
        sections.push_back({tag("MSDF"), strikeSection({sd})});
    }

    // header, section table, sections (4-byte aligned)
    Writer w;
    w.bytes(reinterpret_cast<const std::uint8_t*>(kMagic), 8);
    w.put(kVersion);
    w.put(std::uint32_t(sections.size()));
    w.put(std::int32_t(src.ascent));
    w.put(std::int32_t(src.descent));
    w.put(std::int32_t(src.lineGap));
    w.put(src.unitsPerPixelScale1);
    w.put(src.emPerPixelSize);
    w.put(src.unitsPerEm);
    w.put(src.numGlyphs);
    w.b.resize(kHeaderSize, 0);
    const std::size_t table = w.b.size();
    w.b.resize(table + sections.size() * 12, 0);
    for (std::size_t i = 0; i < sections.size(); ++i) {
        w.align4();
        const std::uint32_t off = std::uint32_t(w.b.size()), size = std::uint32_t(sections[i].second.size());
        std::uint8_t* e = w.b.data() + table + i * 12;
        std::memcpy(e, &sections[i].first, 4);
        std::memcpy(e + 4, &off, 4);
        std::memcpy(e + 8, &size, 4);
        w.bytes(sections[i].second.data(), sections[i].second.size());
    }
    return std::move(w.b);
}

} // namespace fuse::text
