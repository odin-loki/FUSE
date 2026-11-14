// COOKED text profile runtime: fonts are .fusefont packs (fusefont.hpp) cooked by fuse_text_cook. Lookups are binary
// searches over the pack; glyphs come from the cooked coverage strikes (byte-identical to MINIMAL at those sizes),
// other sizes from the pack's MSDF (or the nearest strike resampled). No font file is ever parsed.
#include "text_cooked.hpp"

#include "fusefont.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace fuse::text::detail {

using namespace fusefont;

namespace {
struct Strike {
    std::uint32_t q = 0;
    float range = 0.f;
    std::uint32_t n = 0;
    const std::uint8_t* records = nullptr;
    const std::uint8_t* data = nullptr;
    std::size_t dataSize = 0;
};

struct Section {
    const std::uint8_t* p = nullptr;
    std::uint32_t size = 0;
};
} // namespace

struct CookedFace {
    std::span<const std::uint8_t> bytes;
    CookedInfo info;
    Section cmap, hmtx, kern, colr;
    std::vector<Strike> coverage, color, msdf;
};

namespace {
bool readStrikes(const Section& s, std::vector<Strike>& out) {
    if (!s.p) return true;
    if (s.size < 4) return false;
    const std::uint32_t count = rd<std::uint32_t>(s.p);
    if (std::uint64_t(count) * kStrikeHeader + 4 > s.size) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* h = s.p + 4 + i * kStrikeHeader;
        Strike st;
        st.q = rd<std::uint32_t>(h);
        st.range = rd<float>(h + 4);
        st.n = rd<std::uint32_t>(h + 8);
        const std::uint32_t rec = rd<std::uint32_t>(h + 12), data = rd<std::uint32_t>(h + 16);
        if (rec > s.size || std::uint64_t(st.n) * sizeof(GlyphRecord) > s.size - rec || data > s.size) return false;
        st.records = s.p + rec;
        st.data = s.p + data;
        st.dataSize = s.size - data;
        out.push_back(st);
    }
    return true;
}

bool findRecord(const Strike& s, std::uint32_t glyph, GlyphRecord& r) {
    std::uint32_t lo = 0, hi = s.n;
    while (lo < hi) {
        const std::uint32_t m = (lo + hi) / 2;
        std::memcpy(&r, s.records + std::size_t(m) * sizeof(GlyphRecord), sizeof r);
        const std::uint32_t g = r.glyph & ~kForegroundBit;
        if (g == glyph) return true;
        if (g < glyph) lo = m + 1;
        else hi = m;
    }
    return false;
}

std::size_t texels(const GlyphRecord& r, std::size_t bpp, bool two) { return std::size_t(r.w) * r.h * bpp * (two ? 2 : 1); }

const Strike* exact(const std::vector<Strike>& v, std::uint32_t q) {
    for (const Strike& s : v)
        if (s.q == q) return &s;
    return nullptr;
}

// Strike to resample from for size q: the nearest, preferring larger.
const Strike* nearest(const std::vector<Strike>& v, std::uint32_t q, std::uint32_t glyph) {
    const Strike* best = nullptr;
    GlyphRecord r;
    for (const Strike& s : v) {
        if (!findRecord(s, glyph, r)) continue;
        auto cost = [&](const Strike& x) { return x.q >= q ? double(x.q) / double(q) : 4.0 * double(q) / double(x.q); };
        if (!best || cost(s) < cost(*best)) best = &s;
    }
    return best;
}

// A glyph bitmap generated for a size that is not cooked: the last one is kept for glyphBox -> rasterise.
struct Generated {
    const CookedFace* face = nullptr;
    std::uint32_t glyph = 0, q = 0;
    int x0 = 0, y0 = 0, w = 0, h = 0;
    std::vector<std::uint8_t> cov;
};
thread_local Generated g_gen;

float texel(const std::uint8_t* d, int w, int h, int x, int y, int ch, int bpp) {
    x = std::clamp(x, 0, w - 1);
    y = std::clamp(y, 0, h - 1);
    return float(d[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * std::size_t(bpp) + std::size_t(ch)]) / 255.f;
}

// Coverage at q / 4 px from the MSDF (sampled like the draw list's MSDF glyphs) or from the nearest strike (bilinear,
// transparent outside), then trimmed to its ink.
void generate(const CookedFace& f, std::uint32_t glyph, std::uint32_t q) {
    Generated& g = g_gen;
    if (g.face == &f && g.glyph == glyph && g.q == q) return;
    g.face = &f;
    g.glyph = glyph;
    g.q = q;
    g.x0 = g.y0 = g.w = g.h = 0;
    g.cov.clear();
    const float px = float(q) / 4.f;
    GlyphRecord r{};
    const Strike* src = nullptr;
    bool msdf = false;
    for (const Strike& s : f.msdf)
        if (findRecord(s, glyph, r)) {
            src = &s;
            msdf = true;
            break;
        }
    if (!src) {
        src = nearest(f.coverage, q, glyph);
        if (!src || !findRecord(*src, glyph, r)) return;
    }
    if (r.w == 0 || r.h == 0 || std::size_t(r.offset) + texels(r, msdf ? 4 : 1, false) > src->dataSize) return;
    const float k = px / (float(src->q) / 4.f);
    const int bx0 = int(std::floor(float(r.x0) * k)), by0 = int(std::floor(float(r.y0) * k));
    const int bx1 = int(std::ceil(float(r.x0 + r.w) * k)), by1 = int(std::ceil(float(r.y0 + r.h) * k));
    const int w = bx1 - bx0, h = by1 - by0;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    std::vector<std::uint8_t> buf(std::size_t(w) * std::size_t(h));
    const std::uint8_t* d = src->data + r.offset;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float sx = (float(bx0 + x) + 0.5f) / k - float(r.x0) - 0.5f;
            const float sy = (float(by0 + y) + 0.5f) / k - float(r.y0) - 0.5f;
            const float fx0 = std::floor(sx), fy0 = std::floor(sy), fx = sx - fx0, fy = sy - fy0;
            const int ix = int(fx0), iy = int(fy0);
            float cov = 0.f;
            if (msdf) {
                float ch[3];
                for (int c = 0; c < 3; ++c) {
                    const float t = texel(d, r.w, r.h, ix, iy, c, 4) + (texel(d, r.w, r.h, ix + 1, iy, c, 4) - texel(d, r.w, r.h, ix, iy, c, 4)) * fx;
                    const float b = texel(d, r.w, r.h, ix, iy + 1, c, 4) + (texel(d, r.w, r.h, ix + 1, iy + 1, c, 4) - texel(d, r.w, r.h, ix, iy + 1, c, 4)) * fx;
                    ch[c] = t + (b - t) * fy;
                }
                const float med = std::max(std::min(ch[0], ch[1]), std::min(std::max(ch[0], ch[1]), ch[2]));
                cov = std::clamp(src->range * k * (med - 0.5f) + 0.5f, 0.f, 1.f);
            } else { // transparent outside the source bitmap
                auto at = [&](int x1, int y1) { return (x1 < 0 || y1 < 0 || x1 >= r.w || y1 >= r.h) ? 0.f : texel(d, r.w, r.h, x1, y1, 0, 1); };
                const float t = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * fx;
                const float b = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * fx;
                cov = t + (b - t) * fy;
            }
            buf[std::size_t(y) * std::size_t(w) + std::size_t(x)] = std::uint8_t(std::lround(std::clamp(cov, 0.f, 1.f) * 255.f));
        }
    int tx0 = w, ty0 = h, tx1 = -1, ty1 = -1; // trim to the ink
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (buf[std::size_t(y) * std::size_t(w) + std::size_t(x)]) {
                tx0 = std::min(tx0, x);
                ty0 = std::min(ty0, y);
                tx1 = std::max(tx1, x);
                ty1 = std::max(ty1, y);
            }
    if (tx1 < 0) return;
    g.x0 = bx0 + tx0;
    g.y0 = by0 + ty0;
    g.w = tx1 - tx0 + 1;
    g.h = ty1 - ty0 + 1;
    g.cov.resize(std::size_t(g.w) * std::size_t(g.h));
    for (int y = 0; y < g.h; ++y)
        std::memcpy(g.cov.data() + std::size_t(y) * std::size_t(g.w), buf.data() + std::size_t(ty0 + y) * std::size_t(w) + std::size_t(tx0),
                    std::size_t(g.w));
}
} // namespace

CookedFace* cookedCreate(std::span<const std::uint8_t> b) {
    const std::uint8_t* d = b.data();
    const std::size_t n = b.size();
    if (!isFuseFont(d, n) || rd<std::uint32_t>(d + 8) != kVersion) return nullptr;
    const std::uint32_t count = rd<std::uint32_t>(d + 12);
    if (std::uint64_t(count) * 12 + kHeaderSize > n) return nullptr;
    auto* f = new CookedFace;
    f->bytes = b;
    f->info.ascent = rd<std::int32_t>(d + 16);
    f->info.descent = rd<std::int32_t>(d + 20);
    f->info.lineGap = rd<std::int32_t>(d + 24);
    f->info.unitsPerPixelScale1 = rd<float>(d + 28);
    f->info.emPerPixelSize = rd<float>(d + 32);
    f->info.unitsPerEm = rd<std::uint16_t>(d + 36);
    f->info.numGlyphs = rd<std::uint16_t>(d + 38);
    Section cov, rgba, msdf;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* e = d + kHeaderSize + i * 12;
        const std::uint32_t t = rd<std::uint32_t>(e), off = rd<std::uint32_t>(e + 4), size = rd<std::uint32_t>(e + 8);
        if (off > n || size > n - off) {
            delete f;
            return nullptr;
        }
        const Section s{d + off, size};
        if (t == tag("CMAP")) f->cmap = s;
        else if (t == tag("HMTX")) f->hmtx = s;
        else if (t == tag("KERN")) f->kern = s;
        else if (t == tag("COLR")) f->colr = s;
        else if (t == tag("COVR")) cov = s;
        else if (t == tag("RGBA")) rgba = s;
        else if (t == tag("MSDF")) msdf = s;
    }
    auto tableOk = [](const Section& s, std::size_t entry) {
        return !s.p || (s.size >= 4 && std::uint64_t(rd<std::uint32_t>(s.p)) * entry + 4 <= s.size);
    };
    if (!f->cmap.p || !f->hmtx.p || !tableOk(f->cmap, 8) || !tableOk(f->hmtx, 2) || !tableOk(f->kern, 8) ||
        !(f->info.unitsPerPixelScale1 > 0.f) || !readStrikes(cov, f->coverage) || !readStrikes(rgba, f->color) ||
        !readStrikes(msdf, f->msdf)) {
        delete f;
        return nullptr;
    }
    return f;
}

void cookedDestroy(CookedFace* f) { delete f; }
const CookedInfo& cookedInfo(const CookedFace* f) { return f->info; }

std::uint32_t cookedGlyphIndex(const CookedFace* f, char32_t cp) {
    const std::uint32_t n = rd<std::uint32_t>(f->cmap.p);
    std::uint32_t lo = 0, hi = n;
    while (lo < hi) {
        const std::uint32_t m = (lo + hi) / 2;
        const std::uint32_t c = rd<std::uint32_t>(f->cmap.p + 4 + m * 8);
        if (c == std::uint32_t(cp)) return rd<std::uint32_t>(f->cmap.p + 8 + m * 8);
        if (c < std::uint32_t(cp)) lo = m + 1;
        else hi = m;
    }
    return 0;
}

int cookedAdvance(const CookedFace* f, std::uint32_t glyph) {
    const std::uint32_t n = rd<std::uint32_t>(f->hmtx.p);
    if (n == 0) return 0;
    return rd<std::uint16_t>(f->hmtx.p + 4 + std::min(glyph, n - 1) * 2);
}

int cookedKern(const CookedFace* f, std::uint32_t l, std::uint32_t r) {
    if (!f->kern.p) return 0;
    const std::uint32_t key = (l << 16) | r;
    std::uint32_t lo = 0, hi = rd<std::uint32_t>(f->kern.p);
    while (lo < hi) {
        const std::uint32_t m = (lo + hi) / 2;
        const std::uint8_t* e = f->kern.p + 4 + m * 8;
        const std::uint32_t k = (std::uint32_t(rd<std::uint16_t>(e)) << 16) | rd<std::uint16_t>(e + 2);
        if (k == key) return rd<std::int16_t>(e + 4);
        if (k < key) lo = m + 1;
        else hi = m;
    }
    return 0;
}

void cookedGlyphBox(const CookedFace* f, std::uint32_t glyph, float px, int& x0, int& y0, int& x1, int& y1) {
    const std::uint32_t q = std::uint32_t(std::lround(px * 4.f));
    GlyphRecord r;
    if (const Strike* s = exact(f->coverage, q); s && findRecord(*s, glyph, r)) {
        x0 = r.x0;
        y0 = r.y0;
        x1 = r.x0 + r.w;
        y1 = r.y0 + r.h;
        return;
    }
    generate(*f, glyph, q);
    x0 = g_gen.x0;
    y0 = g_gen.y0;
    x1 = g_gen.x0 + g_gen.w;
    y1 = g_gen.y0 + g_gen.h;
}

void cookedRasterise(const CookedFace* f, std::uint32_t glyph, float px, std::uint8_t* dst, int w, int h, int stride) {
    const std::uint32_t q = std::uint32_t(std::lround(px * 4.f));
    GlyphRecord r;
    const std::uint8_t* src = nullptr;
    int sw = 0, sh = 0;
    if (const Strike* s = exact(f->coverage, q); s && findRecord(*s, glyph, r) && r.offset + texels(r, 1, false) <= s->dataSize) {
        src = s->data + r.offset;
        sw = r.w;
        sh = r.h;
    } else {
        generate(*f, glyph, q);
        src = g_gen.cov.data();
        sw = g_gen.w;
        sh = g_gen.h;
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            dst[std::size_t(y) * std::size_t(stride) + std::size_t(x)] =
                (x < sw && y < sh) ? src[std::size_t(y) * std::size_t(sw) + std::size_t(x)] : std::uint8_t(0);
}

bool cookedForeground(const CookedFace* f, std::uint32_t glyph) {
    GlyphRecord r;
    for (const Strike& s : f->color)
        if (findRecord(s, glyph, r)) return (r.glyph & kForegroundBit) != 0;
    return false;
}

bool cookedColor(const CookedFace* f, std::uint32_t glyph, float px, std::uint32_t fg, ColorImage& out) {
    const std::uint32_t q = std::uint32_t(std::lround(px * 4.f));
    GlyphRecord r{};
    const Strike* s = exact(f->color, q);
    if (!s || !findRecord(*s, glyph, r)) s = nearest(f->color, q, glyph);
    if (!s || !findRecord(*s, glyph, r)) return false;
    const bool two = (r.glyph & kForegroundBit) != 0;
    if (std::size_t(r.offset) + texels(r, 4, two) > s->dataSize) return false;
    const std::uint8_t* black = s->data + r.offset;
    const std::uint8_t* white = black + std::size_t(r.w) * r.h * 4;
    // the text colour: black + (white - black) * colour, per channel
    const float fc[3] = {float(fg & 0xFF) / 255.f, float((fg >> 8) & 0xFF) / 255.f, float((fg >> 16) & 0xFF) / 255.f};
    std::vector<std::uint8_t> img(std::size_t(r.w) * r.h * 4);
    for (std::size_t i = 0; i < img.size(); ++i) {
        const int c = int(i & 3);
        img[i] = (two && c < 3) ? std::uint8_t(std::lround(float(black[i]) + (float(white[i]) - float(black[i])) * fc[c])) : black[i];
    }
    if (s->q == q) {
        out.w = r.w;
        out.h = r.h;
        out.x0 = r.x0;
        out.y0 = r.y0;
        out.rgba = std::move(img);
        return true;
    }
    // another size: bilinear resample of the nearest image (premultiplied, transparent outside)
    const float k = float(q) / float(s->q);
    out.x0 = int(std::floor(float(r.x0) * k));
    out.y0 = int(std::floor(float(r.y0) * k));
    out.w = int(std::ceil(float(r.x0 + r.w) * k)) - out.x0;
    out.h = int(std::ceil(float(r.y0 + r.h) * k)) - out.y0;
    if (out.w <= 0 || out.h <= 0 || out.w > 4096 || out.h > 4096) return false;
    out.rgba.assign(std::size_t(out.w) * std::size_t(out.h) * 4, 0);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            const float sx = (float(out.x0 + x) + 0.5f) / k - float(r.x0) - 0.5f;
            const float sy = (float(out.y0 + y) + 0.5f) / k - float(r.y0) - 0.5f;
            const float fx0 = std::floor(sx), fy0 = std::floor(sy), fx = sx - fx0, fy = sy - fy0;
            const int ix = int(fx0), iy = int(fy0);
            for (int c = 0; c < 4; ++c) {
                auto at = [&](int x1, int y1) {
                    return (x1 < 0 || y1 < 0 || x1 >= r.w || y1 >= r.h) ? 0.f : float(img[(std::size_t(y1) * r.w + std::size_t(x1)) * 4 + std::size_t(c)]);
                };
                const float t = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * fx;
                const float b = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * fx;
                out.rgba[(std::size_t(y) * std::size_t(out.w) + std::size_t(x)) * 4 + std::size_t(c)] = std::uint8_t(std::lround(std::clamp(t + (b - t) * fy, 0.f, 255.f)));
            }
        }
    return true;
}

bool cookedMsdf(const CookedFace* f, std::uint32_t glyph, float px, float range, ColorImage& out) {
    const std::uint32_t q = std::uint32_t(std::lround(px * 4.f));
    GlyphRecord r;
    for (const Strike& s : f->msdf) {
        if (s.q != q || s.range != range || !findRecord(s, glyph, r)) continue;
        if (std::size_t(r.offset) + texels(r, 4, false) > s.dataSize) return false;
        out.w = r.w;
        out.h = r.h;
        out.x0 = r.x0;
        out.y0 = r.y0;
        out.rgba.assign(s.data + r.offset, s.data + r.offset + texels(r, 4, false));
        return true;
    }
    return false;
}

void cookedColorTables(const CookedFace* f, std::vector<ColorLayer>& layers, std::vector<std::array<std::uint32_t, 3>>& base,
                       std::vector<ColorLayer>& images) {
    if (f->colr.p && f->colr.size >= 8) {
        const std::uint8_t* p = f->colr.p;
        const std::uint32_t nBase = rd<std::uint32_t>(p);
        if (std::uint64_t(nBase) * 12 + 8 <= f->colr.size) {
            const std::uint32_t nLayers = rd<std::uint32_t>(p + 4 + nBase * 12);
            if (std::uint64_t(nBase) * 12 + 8 + std::uint64_t(nLayers) * 12 <= f->colr.size) {
                for (std::uint32_t i = 0; i < nLayers; ++i) {
                    const std::uint8_t* e = p + 8 + nBase * 12 + i * 12;
                    ColorLayer l;
                    l.glyph = rd<std::uint32_t>(e);
                    l.r = e[4];
                    l.g = e[5];
                    l.b = e[6];
                    l.a = e[7];
                    l.foreground = e[8] != 0;
                    layers.push_back(l);
                }
                for (std::uint32_t i = 0; i < nBase; ++i) {
                    const std::uint8_t* e = p + 4 + i * 12;
                    const std::array<std::uint32_t, 3> b{rd<std::uint32_t>(e), rd<std::uint32_t>(e + 4), rd<std::uint32_t>(e + 8)};
                    if (std::uint64_t(b[1]) + b[2] <= nLayers) base.push_back(b);
                }
            }
        }
    }
    // image glyphs: every glyph of any colour strike
    std::vector<std::uint32_t> ids;
    for (const Strike& s : f->color)
        for (std::uint32_t i = 0; i < s.n; ++i) ids.push_back(rd<std::uint32_t>(s.records + std::size_t(i) * sizeof(GlyphRecord)) & ~kForegroundBit);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (const std::uint32_t g : ids) {
        ColorLayer l;
        l.glyph = g;
        l.image = true;
        images.push_back(l);
    }
}

} // namespace fuse::text::detail
