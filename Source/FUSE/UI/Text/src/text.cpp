#include "fuse/text/text.hpp"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif
#include "stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "text_internal.hpp"

#if FUSE_TEXT_FULL
#include "text_full.hpp"
#endif

namespace fuse::text {

TextProfile textProfile() {
#if FUSE_TEXT_FULL
    return TextProfile::Full;
#else
    return TextProfile::Minimal;
#endif
}
const char* textProfileName(TextProfile p) {
    switch (p) {
    case TextProfile::Minimal: return "MINIMAL";
    case TextProfile::Full: return "FULL";
    case TextProfile::Cooked: return "COOKED";
    }
    return "?";
}
const char* textErrorName(TextError e) {
    switch (e) {
    case TextError::InvalidFont: return "invalid_font";
    case TextError::NoGlyphs: return "no_glyphs";
    case TextError::AtlasFull: return "atlas_full";
    case TextError::InvalidUtf8: return "invalid_utf8";
    }
    return "?";
}

// ---- UTF-8 ---------------------------------------------------------------------------------------------------------
char32_t decodeUtf8(std::string_view s, std::size_t& i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    auto cont = [&](std::size_t k) -> int {
        if (i + k >= s.size()) return -1;
        const auto b = static_cast<unsigned char>(s[i + k]);
        return (b & 0xC0u) == 0x80u ? int(b & 0x3Fu) : -1;
    };
    if (b0 < 0x80u) { ++i; return b0; }
    int n = 0;
    char32_t cp = 0;
    if ((b0 & 0xE0u) == 0xC0u) { n = 1; cp = b0 & 0x1Fu; }
    else if ((b0 & 0xF0u) == 0xE0u) { n = 2; cp = b0 & 0x0Fu; }
    else if ((b0 & 0xF8u) == 0xF0u) { n = 3; cp = b0 & 0x07u; }
    else { ++i; return 0xFFFD; }
    for (int k = 1; k <= n; ++k) {
        const int c = cont(std::size_t(k));
        if (c < 0) { ++i; return 0xFFFD; }
        cp = (cp << 6) | char32_t(c);
    }
    i += std::size_t(n) + 1;
    return cp;
}

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) out.push_back(char(cp));
    else if (cp < 0x800) { out.push_back(char(0xC0 | (cp >> 6))); out.push_back(char(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12))); out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18))); out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F))); out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

std::size_t prevCodepoint(std::string_view s, std::size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0u) == 0x80u) --i;
    return i;
}
std::size_t nextCodepoint(std::string_view s, std::size_t i) {
    if (i >= s.size()) return s.size();
    decodeUtf8(s, i);
    return i;
}

// ---- UAX #29 grapheme clusters -------------------------------------------------------------------------------------
namespace {
using GB = GraphemeBreak;
enum class Incb : std::uint8_t { None, Linker, Consonant, Extend };
struct GbRange {
    char32_t lo, hi;
    GB v;
};
struct IncbRange {
    char32_t lo, hi;
    Incb v;
};
#include "unicode_tables.inc" // generated from the UCD by Text/tools/gen_unicode_tables.py
template <class R, std::size_t N> auto lookup(const R (&r)[N], char32_t c, decltype(R::v) none) {
    std::size_t a = 0, b = N;
    while (a < b) {
        const std::size_t m = (a + b) / 2;
        if (c < r[m].lo) b = m;
        else if (c > r[m].hi) a = m + 1;
        else return r[m].v;
    }
    return none;
}
Incb incbProperty(char32_t c) { return c < 0x0300 ? Incb::None : lookup(kIncb, c, Incb::None); }

struct ClusterState {
    GB prev = GB::Control;
    bool first = true;
    std::uint32_t riRun = 0; // regional indicators since the last non-RI
    bool pictSeq = false;    // ExtPict Extend* seen (GB11 left side)
    bool pictZwj = false;    // ... followed by ZWJ
    std::uint8_t incb = 0;   // GB9c: 1 after InCB=Consonant [Extend Linker]*, 2 once a Linker was seen
};
// true when there is a boundary before `cur` given the state after the previous code point
bool breakBefore(const ClusterState& st, GB cur, Incb incb) {
    if (st.first) return true;                                         // GB1
    const GB p = st.prev;
    if (p == GB::CR && cur == GB::LF) return false;                     // GB3
    if (p == GB::Control || p == GB::CR || p == GB::LF) return true;    // GB4
    if (cur == GB::Control || cur == GB::CR || cur == GB::LF) return true; // GB5
    if (p == GB::L && (cur == GB::L || cur == GB::V || cur == GB::LV || cur == GB::LVT)) return false; // GB6
    if ((p == GB::LV || p == GB::V) && (cur == GB::V || cur == GB::T)) return false;                   // GB7
    if ((p == GB::LVT || p == GB::T) && cur == GB::T) return false;                                    // GB8
    if (cur == GB::Extend || cur == GB::ZWJ) return false;             // GB9
    if (cur == GB::SpacingMark) return false;                          // GB9a
    if (p == GB::Prepend) return false;                                // GB9b
    if (st.incb == 2 && incb == Incb::Consonant) return false;         // GB9c
    if (p == GB::ZWJ && cur == GB::ExtPict && st.pictZwj) return false; // GB11
    if (p == GB::RegionalIndicator && cur == GB::RegionalIndicator && (st.riRun % 2) == 1) return false; // GB12/13
    return true;                                                       // GB999
}
void advance(ClusterState& st, GB cur, Incb incb) {
    if (incb == Incb::Consonant) st.incb = 1;
    else if (st.incb && incb == Incb::Linker) st.incb = 2;
    else if (!(st.incb && incb == Incb::Extend)) st.incb = 0;
    st.riRun = cur == GB::RegionalIndicator ? st.riRun + 1 : 0;
    if (cur == GB::ExtPict) { st.pictSeq = true; st.pictZwj = false; }
    else if (cur == GB::Extend && st.pictSeq) {}
    else if (cur == GB::ZWJ && st.pictSeq) { st.pictZwj = true; st.pictSeq = false; }
    else { st.pictSeq = false; st.pictZwj = false; }
    st.prev = cur;
    st.first = false;
}
// Grapheme boundaries never cross a line feed, so scanning restarts after the previous '\n'.
std::size_t lineStart(std::string_view s, std::size_t i) {
    while (i > 0 && s[i - 1] != '\n') --i;
    return i;
}
} // namespace

GraphemeBreak graphemeBreakProperty(char32_t c) {
    if (c >= 0xAC00 && c <= 0xD7A3) return (c - 0xAC00) % 28 == 0 ? GB::LV : GB::LVT;
    return lookup(kGraphemeBreak, c, GB::Other);
}
const char* unicodeVersion() { return kUnicodeVersion; }

std::size_t nextBoundary(std::string_view s, std::size_t i) {
    if (i >= s.size()) return s.size();
    // walk from the line start so the RI parity and the emoji ZWJ state are right
    ClusterState st;
    std::size_t at = lineStart(s, i);
    while (at < s.size()) {
        std::size_t nx = at;
        const char32_t cp = decodeUtf8(s, nx);
        const GB g = graphemeBreakProperty(cp);
        const Incb ic = incbProperty(cp);
        if (at > i && breakBefore(st, g, ic)) return at;
        advance(st, g, ic);
        at = nx;
    }
    return s.size();
}

std::size_t prevBoundary(std::string_view s, std::size_t i) {
    if (i == 0) return 0;
    if (i > s.size()) i = s.size();
    // CR LF is one cluster: from after it, step back over both
    if (s[i - 1] == '\n') return (i >= 2 && s[i - 2] == '\r') ? i - 2 : i - 1;
    ClusterState st;
    std::size_t at = lineStart(s, i - 1), last = at;
    while (at < i) {
        std::size_t nx = at;
        const char32_t cp = decodeUtf8(s, nx);
        const GB g = graphemeBreakProperty(cp);
        const Incb ic = incbProperty(cp);
        if (breakBefore(st, g, ic)) last = at;
        advance(st, g, ic);
        at = nx;
    }
    return last;
}

std::size_t graphemeCount(std::string_view s) {
    std::size_t n = 0;
    ClusterState st;
    for (std::size_t at = 0; at < s.size();) {
        const char32_t cp = decodeUtf8(s, at);
        const GB g = graphemeBreakProperty(cp);
        const Incb ic = incbProperty(cp);
        if (breakBefore(st, g, ic)) ++n;
        advance(st, g, ic);
    }
    return n;
}

// ---- Line breaking and bidi ----------------------------------------------------------------------------------------
void lineBreaks(std::string_view s, std::vector<LineBreak>& out, LineBreakStyle style) {
#if FUSE_TEXT_FULL
    detail::fullLineBreaks(s, out, style == LineBreakStyle::Strict);
#else
    (void)style;
    out.assign(s.size(), LineBreak::None);
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t at = i;
        decodeUtf8(s, i);
        for (std::size_t k = at; k + 1 < i; ++k) out[k] = LineBreak::Inside;
        if (s[at] == '\n') out[i - 1] = LineBreak::Must;
        else if (s[at] == ' ' && i < s.size() && s[i] != ' ' && s[i] != '\n') out[i - 1] = LineBreak::Allow;
    }
    if (!s.empty()) out.back() = LineBreak::Must;
#endif
}

bool bidiLevels(std::string_view s, BaseDirection base, std::vector<std::uint8_t>& levels, std::uint8_t& paraLevel) {
#if FUSE_TEXT_FULL
    detail::fullBidiLevels(s, int(base), levels, paraLevel);
    return true;
#else
    paraLevel = base == BaseDirection::Rtl ? 1 : 0;
    levels.assign(s.size(), paraLevel);
    return false;
#endif
}

void reorderVisual(std::span<const std::uint8_t> levels, std::vector<std::uint32_t>& out) {
    const std::size_t n = levels.size();
    out.resize(n);
    for (std::size_t k = 0; k < n; ++k) out[k] = std::uint32_t(k);
    std::uint8_t hi = 0, loOdd = 0xFF;
    for (const std::uint8_t l : levels) {
        hi = std::max(hi, l);
        if (l & 1) loOdd = std::min(loOdd, l);
    }
    // From the highest level down to the lowest odd one, reverse every maximal run at or above that level.
    for (int lv = hi; loOdd != 0xFF && lv >= loOdd; --lv)
        for (std::size_t k = 0; k < n;) {
            if (levels[out[k]] < lv) { ++k; continue; }
            std::size_t m = k;
            while (m < n && levels[out[m]] >= lv) ++m;
            std::reverse(out.begin() + std::ptrdiff_t(k), out.begin() + std::ptrdiff_t(m));
            k = m;
        }
}

// ---- Font ----------------------------------------------------------------------------------------------------------
struct Font::State {
    std::vector<std::uint8_t> bytes;
    stbtt_fontinfo info{};
    float unitsPerPixelScale1 = 0.f; // stbtt_ScaleForPixelHeight(info, 1)
    float emPerPixelSize = 1.f;      // em size in pixels for pixelSize 1 (pixel sizes are ascent-to-descent)
    std::vector<const Font*> fallbacks;
    std::vector<ColorLayer> layers;                       // COLRv0 layer records with CPAL palette 0 resolved
    std::vector<std::array<std::uint32_t, 3>> colorBase; // (glyph, first layer, layer count), sorted by glyph
#if FUSE_TEXT_FULL
    detail::FullFace* full = nullptr; // owned; HarfBuzz + FreeType over `bytes`
    ~State() { detail::fullDestroy(full); }
#endif
};

namespace {
std::atomic<std::uint32_t> g_fontIds{1};

std::uint16_t be16(const std::uint8_t* p) { return std::uint16_t((p[0] << 8) | p[1]); }
std::uint32_t be32(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]; }

std::uint32_t be24(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2]; }

// COLRv1 paint graphs flattened to solid layers (see Font::colorLayers). Every read is bounds-checked; a malformed
// graph just yields fewer layers.
struct ColrV1 {
    const std::uint8_t* d;
    std::size_t n;
    std::uint32_t baseList = 0, layerList = 0; // absolute offsets (0: absent)
    std::uint32_t numBase = 0, numLayers = 0;
    // CPAL palette 0
    std::uint32_t recOff = 0;
    std::uint16_t first = 0, numEntries = 0, numRecords = 0;
    std::vector<ColorLayer>* out = nullptr;
    int budget = 4096; // paint nodes visited per glyph (cycles, huge graphs)

    bool ok(std::uint64_t off, std::uint64_t len) const { return off + len <= n; }
    ColorLayer colour(std::uint16_t pi, float alpha) const {
        ColorLayer cl;
        if (pi == 0xFFFFu || pi >= numEntries || std::uint32_t(first) + pi >= numRecords) {
            cl.foreground = true;
            cl.a = std::uint8_t(std::clamp(alpha, 0.f, 1.f) * 255.f + 0.5f);
            return cl;
        }
        const std::uint8_t* c = d + recOff + (std::uint32_t(first) + pi) * 4; // B G R A
        cl.b = c[0];
        cl.g = c[1];
        cl.r = c[2];
        cl.a = std::uint8_t(float(c[3]) * std::clamp(alpha, 0.f, 1.f) + 0.5f);
        return cl;
    }
    static float f2dot14(const std::uint8_t* p) { return float(std::int16_t(be16(p))) / 16384.f; }
    // Mean colour of a (Var)ColorLine: gradients become one solid fill.
    bool colorLine(std::uint32_t at, bool var, ColorLayer& cl) const {
        if (!ok(at, 3)) return false;
        const std::uint16_t stops = be16(d + at + 1);
        const std::uint32_t size = var ? 10 : 6;
        if (stops == 0 || !ok(at + 3, std::uint64_t(stops) * size)) return false;
        float r = 0, g = 0, b = 0, a = 0;
        bool fg = false;
        for (std::uint32_t k = 0; k < stops; ++k) {
            const std::uint8_t* s = d + at + 3 + k * size;
            const ColorLayer c = colour(be16(s + 2), f2dot14(s + 4));
            fg |= c.foreground;
            r += c.r;
            g += c.g;
            b += c.b;
            a += c.a;
        }
        cl = {};
        cl.foreground = fg;
        cl.r = std::uint8_t(r / stops + 0.5f);
        cl.g = std::uint8_t(g / stops + 0.5f);
        cl.b = std::uint8_t(b / stops + 0.5f);
        cl.a = std::uint8_t(a / stops + 0.5f);
        return true;
    }
    // Fill of the paint under a PaintGlyph: solid, or a gradient's mean colour; transforms and nested glyph clips
    // are looked through.
    bool fill(std::uint32_t p, ColorLayer& cl, int depth) {
        if (depth > 16 || --budget < 0 || !ok(p, 1)) return false;
        const std::uint8_t f = d[p];
        switch (f) {
        case 2:
        case 3: return ok(p, 5) && (cl = colour(be16(d + p + 1), f2dot14(d + p + 3)), true);
        case 4: case 5: case 6: case 7: case 8: case 9:
            return ok(p, 4) && colorLine(p + be24(d + p + 1), (f & 1) != 0, cl);
        case 10:
            return ok(p, 4) && fill(p + be24(d + p + 1), cl, depth + 1);
        case 32: // composite: the source's fill
            return ok(p, 4) && fill(p + be24(d + p + 1), cl, depth + 1);
        default:
            if (f >= 12 && f <= 31) return ok(p, 4) && fill(p + be24(d + p + 1), cl, depth + 1);
            return false;
        }
    }
    // Appends the layers painted by paint `p`.
    void paint(std::uint32_t p, int depth) {
        if (depth > 16 || --budget < 0 || !ok(p, 1)) return;
        const std::uint8_t f = d[p];
        if (f == 1) { // PaintColrLayers
            if (!ok(p, 6) || !layerList) return;
            const std::uint32_t count = d[p + 1], firstLayer = be32(d + p + 2);
            for (std::uint32_t k = 0; k < count; ++k) {
                if (firstLayer + k >= numLayers) break;
                paint(layerList + be32(d + layerList + 4 + (firstLayer + k) * 4), depth + 1);
            }
        } else if (f == 10) { // PaintGlyph: the glyph's outline filled by the child paint
            if (!ok(p, 6)) return;
            ColorLayer cl;
            if (fill(p + be24(d + p + 1), cl, depth + 1)) {
                cl.glyph = be16(d + p + 4);
                out->push_back(cl);
            }
        } else if (f == 11) { // PaintColrGlyph: another base glyph's graph
            if (ok(p, 3))
                if (const std::uint32_t q = basePaint(be16(d + p + 1))) paint(q, depth + 1);
        } else if (f == 32) { // PaintComposite: backdrop, then source (blend modes are not modelled)
            if (!ok(p, 8)) return;
            paint(p + be24(d + p + 5), depth + 1);
            paint(p + be24(d + p + 1), depth + 1);
        } else if (f >= 12 && f <= 31) { // transforms: painted untransformed
            if (ok(p, 4)) paint(p + be24(d + p + 1), depth + 1);
        }
        // gradients / solids outside a PaintGlyph fill the whole clip box: not representable as glyph layers
    }
    std::uint32_t basePaint(std::uint16_t glyph) const {
        std::uint32_t lo = 0, hi = numBase;
        while (lo < hi) { // records are sorted by glyph id
            const std::uint32_t m = (lo + hi) / 2;
            const std::uint8_t* r = d + baseList + 4 + m * 6;
            const std::uint16_t g = be16(r);
            if (g == glyph) return baseList + be32(r + 2);
            if (g < glyph) lo = m + 1;
            else hi = m;
        }
        return 0;
    }
};

// COLR (version 0 layers, plus version 1 paint graphs flattened to solid layers) + CPAL palette 0. Bounds-checked: a
// malformed table leaves the font without colour glyphs.
void loadColr(std::vector<std::uint8_t>& bytes, std::vector<ColorLayer>& layers,
              std::vector<std::array<std::uint32_t, 3>>& colorBase, std::uint32_t fontStart) {
    std::uint8_t* d = bytes.data();
    const std::size_t n = bytes.size();
    const std::uint32_t colr = stbtt__find_table(d, fontStart, "COLR");
    const std::uint32_t cpal = stbtt__find_table(d, fontStart, "CPAL");
    if (!colr || !cpal || colr + 14 > n || cpal + 12 > n) return;
    const std::uint16_t version = be16(d + colr);
    const std::uint16_t numBase = be16(d + colr + 2), numLayers = be16(d + colr + 12);
    const std::uint32_t baseOff = colr + be32(d + colr + 4), layerOff = colr + be32(d + colr + 8);
    const std::uint16_t numEntries = be16(d + cpal + 2), numPalettes = be16(d + cpal + 4), numRecords = be16(d + cpal + 6);
    const std::uint32_t recOff = cpal + be32(d + cpal + 8);
    if (numPalettes == 0 || baseOff + std::size_t(numBase) * 6 > n || layerOff + std::size_t(numLayers) * 4 > n ||
        recOff + std::size_t(numRecords) * 4 > n)
        return;
    const std::uint16_t first = be16(d + cpal + 12); // palette 0's first colour record
    layers.reserve(numLayers);
    for (std::uint32_t i = 0; i < numLayers; ++i) {
        const std::uint8_t* l = d + layerOff + i * 4;
        ColorLayer cl;
        cl.glyph = be16(l);
        const std::uint16_t pi = be16(l + 2);
        if (pi == 0xFFFFu || pi >= numEntries || std::uint32_t(first) + pi >= numRecords) cl.foreground = true;
        else {
            const std::uint8_t* c = d + recOff + (std::uint32_t(first) + pi) * 4; // B G R A
            cl.b = c[0];
            cl.g = c[1];
            cl.r = c[2];
            cl.a = c[3];
        }
        layers.push_back(cl);
    }
    colorBase.reserve(numBase);
    for (std::uint32_t i = 0; i < numBase; ++i) {
        const std::uint8_t* b = d + baseOff + i * 6;
        const std::uint32_t firstLayer = be16(b + 2), count = be16(b + 4);
        if (firstLayer + count <= numLayers && count > 0) colorBase.push_back({be16(b), firstLayer, count});
    }
    std::sort(colorBase.begin(), colorBase.end());

    // COLRv1: glyphs without a v0 record get their paint graph flattened (v0 stays exact where both exist).
    if (version < 1 || colr + 34 > n) return;
    ColrV1 v1{d, n};
    const std::uint32_t bl = be32(d + colr + 14), ll = be32(d + colr + 18);
    if (bl && colr + std::uint64_t(bl) + 4 <= n) {
        v1.baseList = colr + bl;
        v1.numBase = be32(d + v1.baseList);
        if (!v1.ok(v1.baseList + 4, std::uint64_t(v1.numBase) * 6)) return;
    }
    if (ll && colr + std::uint64_t(ll) + 4 <= n) {
        v1.layerList = colr + ll;
        v1.numLayers = be32(d + v1.layerList);
        if (!v1.ok(v1.layerList + 4, std::uint64_t(v1.numLayers) * 4)) v1.layerList = 0;
    }
    v1.recOff = recOff;
    v1.first = first;
    v1.numEntries = numEntries;
    v1.numRecords = numRecords;
    v1.out = &layers;
    const std::size_t v0Count = colorBase.size();
    for (std::uint32_t i = 0; i < v1.numBase; ++i) {
        const std::uint8_t* r = d + v1.baseList + 4 + i * 6;
        const std::uint32_t glyph = be16(r);
        const auto v0End = colorBase.begin() + std::ptrdiff_t(v0Count);
        const auto it = std::lower_bound(colorBase.begin(), v0End, glyph,
                                         [](const std::array<std::uint32_t, 3>& e, std::uint32_t g) { return e[0] < g; });
        if (it != v0End && (*it)[0] == glyph) continue;
        const std::uint32_t start = std::uint32_t(layers.size());
        v1.budget = 4096;
        v1.paint(v1.baseList + be32(r + 2), 0);
        if (layers.size() > start) colorBase.push_back({glyph, start, std::uint32_t(layers.size()) - start});
    }
    std::sort(colorBase.begin(), colorBase.end());
}
} // namespace

std::expected<Font, TextError> Font::fromBytes(std::span<const std::uint8_t> bytes, std::uint32_t faceIndex) {
    if (bytes.size() < 12) return std::unexpected(TextError::InvalidFont);
    auto* st = new State;
    st->bytes.assign(bytes.begin(), bytes.end());
    const int offset = stbtt_GetFontOffsetForIndex(st->bytes.data(), int(faceIndex));
    if (offset < 0 || !stbtt_InitFont(&st->info, st->bytes.data(), offset)) {
        delete st;
        return std::unexpected(TextError::InvalidFont);
    }
    st->unitsPerPixelScale1 = stbtt_ScaleForPixelHeight(&st->info, 1.f);
    st->emPerPixelSize = st->unitsPerPixelScale1 / stbtt_ScaleForMappingEmToPixels(&st->info, 1.f);
    loadColr(st->bytes, st->layers, st->colorBase, std::uint32_t(offset));
#if FUSE_TEXT_FULL
    st->full = detail::fullCreate(st->bytes, faceIndex);
    if (!st->full) {
        delete st;
        return std::unexpected(TextError::InvalidFont);
    }
#endif
    Font f;
    f.m_state = st;
    f.m_id = g_fontIds.fetch_add(1);
    return f;
}

std::expected<Font, TextError> Font::fromFile(const std::string& path, std::uint32_t faceIndex) {
    std::vector<std::uint8_t> data;
    if (!detail::readFile(path, data)) return std::unexpected(TextError::InvalidFont);
    return fromBytes(data, faceIndex);
}

std::expected<Font, TextError> Font::bundled() {
#if defined(FUSE_TEXT_BUNDLED_FONT)
    return fromFile(FUSE_TEXT_BUNDLED_FONT);
#else
    return std::unexpected(TextError::InvalidFont);
#endif
}

std::vector<Font> Font::bundledFallbacks() {
    std::vector<Font> out;
#if defined(FUSE_TEXT_FONT_DIR)
    // Emoji before Symbols 2 so emoji-presentation code points (U+2764 ...) come out in colour.
    static const char* const kFiles[] = {"noto/NotoSansHebrew-Regular.ttf", "noto/NotoSansArabic-Regular.ttf",
                                         "noto/NotoSansDevanagari-Regular.ttf", "twemoji/TwemojiMozilla.ttf",
                                         "noto/NotoSansSymbols2-Regular.ttf", "noto/NotoSansThai-Regular.ttf",
                                         "noto/NotoSansCJKsc-Subset.otf"};
    for (const char* f : kFiles)
        if (auto font = fromFile(std::string(FUSE_TEXT_FONT_DIR) + "/" + f)) out.push_back(std::move(*font));
#endif
    return out;
}

void Font::setFallbacks(std::vector<const Font*> chain) {
    if (!m_state) return;
    std::erase_if(chain, [&](const Font* f) { return !f || !f->valid() || f == this; });
    m_state->fallbacks = std::move(chain);
}
std::size_t Font::faceCount() const { return m_state ? 1 + m_state->fallbacks.size() : 1; }
const Font& Font::face(std::uint16_t i) const {
    return (i == 0 || !m_state || i > m_state->fallbacks.size()) ? *this : *m_state->fallbacks[i - 1];
}
std::uint16_t Font::faceFor(char32_t cp) const {
    if (!m_state) return 0;
    if (glyphIndex(cp)) return 0;
    for (std::size_t k = 0; k < m_state->fallbacks.size(); ++k)
        if (m_state->fallbacks[k]->glyphIndex(cp)) return std::uint16_t(k + 1);
    return 0;
}
float Font::facePixelSize(std::uint16_t i, float px) const {
    const Font& f = face(i);
    if (&f == this) return px;
    return px * m_state->emPerPixelSize / f.m_state->emPerPixelSize;
}
bool Font::hasColorGlyphs() const { return m_state && !m_state->colorBase.empty(); }
std::span<const ColorLayer> Font::colorLayers(std::uint32_t glyph) const {
    if (!m_state || m_state->colorBase.empty()) return {};
    const auto& b = m_state->colorBase;
    auto it = std::lower_bound(b.begin(), b.end(), glyph, [](const auto& e, std::uint32_t g) { return e[0] < g; });
    if (it == b.end() || (*it)[0] != glyph) return {};
    return std::span<const ColorLayer>(m_state->layers.data() + (*it)[1], (*it)[2]);
}

Font::Font(Font&& o) noexcept : m_state(std::exchange(o.m_state, nullptr)), m_id(o.m_id) {}
Font& Font::operator=(Font&& o) noexcept {
    if (this != &o) {
        delete m_state;
        m_state = std::exchange(o.m_state, nullptr);
        m_id = o.m_id;
    }
    return *this;
}
Font::~Font() { delete m_state; }

FontMetrics Font::metrics(float px) const {
    int a = 0, d = 0, g = 0;
    stbtt_GetFontVMetrics(&m_state->info, &a, &d, &g);
    const float s = m_state->unitsPerPixelScale1 * px;
    return {float(a) * s, float(d) * s, float(g) * s};
}
std::uint32_t Font::glyphIndex(char32_t cp) const {
    return std::uint32_t(stbtt_FindGlyphIndex(&m_state->info, int(cp)));
}
float Font::advance(std::uint32_t glyph, float px) const {
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&m_state->info, int(glyph), &adv, &lsb);
    return float(adv) * m_state->unitsPerPixelScale1 * px;
}
float Font::kerning(std::uint32_t l, std::uint32_t r, float px) const {
    return float(stbtt_GetGlyphKernAdvance(&m_state->info, int(l), int(r))) * m_state->unitsPerPixelScale1 * px;
}
void Font::glyphBox(std::uint32_t glyph, float px, int& x0, int& y0, int& x1, int& y1) const {
#if FUSE_TEXT_FULL
    return detail::fullGlyphBox(m_state->full, glyph, m_state->emPerPixelSize * px, x0, y0, x1, y1);
#endif
    const float s = m_state->unitsPerPixelScale1 * px;
    stbtt_GetGlyphBitmapBox(&m_state->info, int(glyph), s, s, &x0, &y0, &x1, &y1);
}
void Font::rasterise(std::uint32_t glyph, float px, std::uint8_t* dst, int w, int h, int stride) const {
#if FUSE_TEXT_FULL
    return detail::fullRasterise(m_state->full, glyph, m_state->emPerPixelSize * px, dst, w, h, stride);
#endif
    const float s = m_state->unitsPerPixelScale1 * px;
    stbtt_MakeGlyphBitmap(&m_state->info, dst, w, h, stride, s, s, int(glyph));
}

// ---- Shaping -------------------------------------------------------------------------------------------------------
float MinimalShaper::shape(const Font& font, float px, std::string_view s, std::vector<ShapedGlyph>& out) const {
    out.clear();
    float pen = 0.f;
    std::uint32_t prev = 0;
    bool hasPrev = false;
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t at = i;
        const char32_t cp = decodeUtf8(s, i);
        // Default-ignorable format characters (ZWSP..RLM, word joiner.., variation selectors, tags) have no ink and
        // no advance; without GSUB the sequences they steer fall back to their visible parts.
        if ((cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
            cp == 0xFEFF || (cp >= 0xE0000 && cp <= 0xE0FFF)) {
            out.push_back({kInvisibleGlyph, std::uint32_t(at), pen, 0.f});
            continue;
        }
        const std::uint32_t g = font.glyphIndex(cp);
        if (hasPrev) pen += font.kerning(prev, g, px);
        const float adv = font.advance(g, px);
        out.push_back({g, std::uint32_t(at), pen, adv});
        pen += adv;
        prev = g;
        hasPrev = true;
    }
    return pen;
}

float Font::unitScale(float px) const { return m_state->unitsPerPixelScale1 * px; }

#if FUSE_TEXT_FULL
float HarfBuzzShaper::shape(const Font& font, float px, std::string_view s, std::vector<ShapedGlyph>& out) const {
    return detail::fullShape(font.m_state->full, font.unitScale(px), s, out);
}
const ITextShaper* fullShaper() {
    static const HarfBuzzShaper shaper;
    return &shaper;
}
#else
float HarfBuzzShaper::shape(const Font& font, float px, std::string_view s, std::vector<ShapedGlyph>& out) const {
    return MinimalShaper{}.shape(font, px, s, out); // FULL not compiled in
}
const ITextShaper* fullShaper() { return nullptr; }
#endif

const ITextShaper& defaultShaper() {
#if FUSE_TEXT_FULL
    return *fullShaper();
#else
    static const MinimalShaper shaper;
    return shaper;
#endif
}

// ---- Font fallback ---------------------------------------------------------------------------------------------
namespace {
// Code points that belong to the cluster before them keep its face (marks, ZWJ, emoji modifiers, VS16, tags, and
// the pictographic after a ZWJ), so a grapheme is never split across faces.
bool inheritsFace(char32_t cp, char32_t prev) {
    const GraphemeBreak p = graphemeBreakProperty(cp);
    return p == GraphemeBreak::Extend || p == GraphemeBreak::ZWJ || p == GraphemeBreak::SpacingMark ||
           (prev == 0x200D && p == GraphemeBreak::ExtPict);
}
bool mayNeedFallback(const Font& font, std::string_view s) {
    if (font.faceCount() < 2) return false;
    for (const char c : s)
        if (static_cast<unsigned char>(c) >= 0x80u) return true; // ASCII is always the primary face's
    return false;
}
/// Maximal same-face runs in logical order: fn(begin, end, face).
template <class Fn> void forEachFaceRun(const Font& font, std::string_view s, Fn&& fn) {
    std::size_t runBegin = 0;
    std::uint16_t runFace = 0;
    char32_t prev = 0;
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t at = i;
        const char32_t cp = decodeUtf8(s, i);
        std::uint16_t f = 0;
        if (at > 0 && inheritsFace(cp, prev)) f = runFace;
        else if (cp >= 0x20 && cp != 0x7F) f = font.faceFor(cp);
        if (at == 0) runFace = f;
        else if (f != runFace) {
            fn(runBegin, at, runFace);
            runBegin = at;
            runFace = f;
        }
        prev = cp;
    }
    if (!s.empty()) fn(runBegin, s.size(), runFace);
}
float shapeWithFallback(const Font& font, const ITextShaper& shaper, float px, std::string_view s,
                        std::vector<ShapedGlyph>& out) {
    if (!mayNeedFallback(font, s)) return shaper.shape(font, px, s, out);
    out.clear();
    thread_local std::vector<ShapedGlyph> run;
    float pen = 0.f;
    forEachFaceRun(font, s, [&](std::size_t b, std::size_t e, std::uint16_t f) {
        const float w = shaper.shape(font.face(f), font.facePixelSize(f, px), s.substr(b, e - b), run);
        for (ShapedGlyph g : run) {
            g.cluster += std::uint32_t(b);
            g.x += pen;
            g.face = f;
            out.push_back(g);
        }
        pen += w;
    });
    return pen;
}
#if FUSE_TEXT_FULL
// FULL layout shapes with the primary face only (bidi + line breaking + run cache); fallback runs are then
// re-shaped with their face per line and the rest of the line shifts by the width change.
void applyFallbackPerLine(const Font& font, const ITextShaper& shaper, float px, std::string_view s,
                          std::vector<ShapedGlyph>& g, std::vector<TextLine>& lines) {
    if (!mayNeedFallback(font, s)) return;
    struct Span {
        std::uint32_t b, e;
        std::uint16_t face;
    };
    thread_local std::vector<Span> spans;
    spans.clear();
    forEachFaceRun(font, s, [&](std::size_t b, std::size_t e, std::uint16_t f) {
        if (f) spans.push_back({std::uint32_t(b), std::uint32_t(e), f});
    });
    if (spans.empty()) return;
    auto spanOf = [&](const ShapedGlyph& x) -> int {
        if (x.glyph == kInvisibleGlyph) return -1;
        for (std::size_t k = 0; k < spans.size(); ++k)
            if (x.cluster >= spans[k].b && x.cluster < spans[k].e) return int(k);
        return -1;
    };
    thread_local std::vector<ShapedGlyph> outG, run;
    outG.clear();
    for (TextLine& ln : lines) {
        const std::uint32_t first = std::uint32_t(outG.size());
        const std::uint32_t end = ln.firstGlyph + ln.glyphCount;
        float shift = 0.f;
        for (std::uint32_t k = ln.firstGlyph; k < end;) {
            const int sp = spanOf(g[k]);
            if (sp < 0) {
                ShapedGlyph x = g[k++];
                x.x += shift;
                outG.push_back(x);
                continue;
            }
            std::uint32_t j = k, lo = g[k].cluster;
            float oldAdv = 0.f;
            for (; j < end && spanOf(g[j]) == sp; ++j) {
                oldAdv += g[j].advance;
                lo = std::min(lo, g[j].cluster);
            }
            const Span& span = spans[std::size_t(sp)];
            std::uint32_t hi = span.e;
            if (ln.byteEnd > lo && ln.byteEnd < hi) hi = ln.byteEnd;
            const float x0 = g[k].x + shift;
            const float w = shaper.shape(font.face(span.face), font.facePixelSize(span.face, px), s.substr(lo, hi - lo), run);
            for (ShapedGlyph r : run) {
                r.cluster += lo;
                r.x += x0;
                r.face = span.face;
                outG.push_back(r);
            }
            shift += w - oldAdv;
            k = j;
        }
        ln.firstGlyph = first;
        ln.glyphCount = std::uint32_t(outG.size()) - first;
        ln.width += shift;
    }
    g.swap(outG);
}
#endif
} // namespace

float shapeText(const Font& font, std::string_view s, float px, std::vector<ShapedGlyph>& out) {
    return shapeWithFallback(font, defaultShaper(), px, s, out);
}

float measureText(const Font& font, std::string_view s, float px, std::vector<ShapedGlyph>& scratch) {
    return shapeText(font, s, px, scratch);
}

ShapeCacheStats shapeCacheStats(const Font& font) {
    ShapeCacheStats st;
#if FUSE_TEXT_FULL
    if (font.m_state) detail::fullCacheStats(font.m_state->full, st.hits, st.misses);
#else
    (void)font;
#endif
    return st;
}

void layoutText(const Font& font, const ITextShaper& shaper, std::string_view s, const TextLayoutDesc& desc,
                TextLayout& out) {
    out.lines.clear();
    out.metrics = font.metrics(desc.pixelSize);
    const float lh = std::ceil(out.metrics.lineHeight());
    const bool wrap = desc.maxWidth > 0.f && !desc.elide;
    auto& g = out.glyphs;
#if FUSE_TEXT_FULL
    const bool full = &shaper == fullShaper() && font.m_state && font.m_state->full;
#else
    const bool full = false;
#endif
    if (full) {
#if FUSE_TEXT_FULL
        detail::fullLayout(font.m_state->full, font.unitScale(desc.pixelSize), s, wrap ? desc.maxWidth : 0.f, lh,
                           out.metrics.ascent, g, out.lines);
        applyFallbackPerLine(font, shaper, desc.pixelSize, s, g, out.lines);
#endif
    } else {
    shapeWithFallback(font, shaper, desc.pixelSize, s, out.glyphs);
    const std::uint32_t n = std::uint32_t(g.size());

    std::uint32_t lineStart = 0;
    auto emit = [&](std::uint32_t begin, std::uint32_t end, std::uint32_t nextStart) {
        TextLine ln;
        ln.firstGlyph = begin;
        ln.glyphCount = end - begin;
        ln.byteBegin = begin < n ? g[begin].cluster : std::uint32_t(s.size());
        ln.byteEnd = end < n ? g[end].cluster : std::uint32_t(s.size());
        const float x0 = begin < n ? g[begin].x : 0.f;
        float w = 0.f;
        // Trailing spaces do not count toward the width.
        std::uint32_t last = end;
        while (last > begin && (s[g[last - 1].cluster] == ' ' || s[g[last - 1].cluster] == '\n')) --last;
        if (last > begin) w = g[last - 1].x + g[last - 1].advance - x0;
        for (std::uint32_t k = begin; k < end; ++k) g[k].x -= x0;
        ln.width = w;
        ln.baseline = float(out.lines.size()) * lh + std::ceil(out.metrics.ascent);
        out.lines.push_back(ln);
        lineStart = nextStart;
    };

    std::uint32_t lastBreak = 0; // glyph index after a space (break opportunity), 0 = none
    for (std::uint32_t i = 0; i < n; ++i) {
        const char c = s[g[i].cluster];
        if (c == '\n') {
            emit(lineStart, i + 1, i + 1);
            lastBreak = 0;
            continue;
        }
        if (wrap) {
            const float right = g[i].x + g[i].advance - g[lineStart].x;
            if (right > desc.maxWidth && c != ' ' && i > lineStart) {
                const std::uint32_t at = (lastBreak > lineStart) ? lastBreak : i;
                emit(lineStart, at, at);
                lastBreak = 0;
                // re-evaluate glyphs from `at` on the new line
                i = at - 1;
                continue;
            }
        }
        if (c == ' ') lastBreak = i + 1;
    }
    if (lineStart < n || out.lines.empty() || (n > 0 && s[g[n - 1].cluster] == '\n')) emit(lineStart, n, n);
    for (auto& gl : g) if (s[gl.cluster] == '\n' || s[gl.cluster] == '\r') gl.glyph = kInvisibleGlyph;
    }
    const std::uint32_t n = std::uint32_t(g.size());
    bool anyRtl = false;
    for (const auto& ln : out.lines) anyRtl |= ln.rtl;

    if (desc.elide && !anyRtl && desc.maxWidth > 0.f && !out.lines.empty() && out.lines[0].width > desc.maxWidth) {
        // Keep glyphs while they fit with an ellipsis, then append U+2026 (or "...").
        const std::uint32_t ell = font.glyphIndex(0x2026);
        const std::uint32_t dot = font.glyphIndex('.');
        const float ellW = ell ? font.advance(ell, desc.pixelSize) : 3.f * font.advance(dot, desc.pixelSize);
        std::uint32_t keep = 0;
        while (keep < n && g[keep].x + g[keep].advance + ellW <= desc.maxWidth) ++keep;
        float pen = keep ? g[keep - 1].x + g[keep - 1].advance : 0.f;
        g.resize(keep);
        const std::uint32_t at = keep < n ? std::uint32_t(s.size()) : std::uint32_t(s.size());
        if (ell) g.push_back({ell, at, pen, ellW});
        else for (int k = 0; k < 3; ++k) {
            const float a = font.advance(dot, desc.pixelSize);
            g.push_back({dot, at, pen, a});
            pen += a;
        }
        out.lines.resize(1);
        out.lines[0].glyphCount = std::uint32_t(g.size());
        out.lines[0].width = g.empty() ? 0.f : g.back().x + g.back().advance;
    }

    out.width = 0.f;
    for (const auto& ln : out.lines) out.width = std::max(out.width, ln.width);
    const float alignW = desc.maxWidth > 0.f ? desc.maxWidth : out.width;
    if (desc.align != TextAlign::Start || anyRtl) {
        for (const auto& ln : out.lines) {
            // Start / End follow the paragraph direction: Start is the right edge of an RTL line.
            const bool toEnd = (desc.align == TextAlign::End) != ln.rtl;
            if (desc.align != TextAlign::Center && !toEnd) continue;
            const float off = desc.align == TextAlign::Center ? std::floor((alignW - ln.width) * 0.5f) : alignW - ln.width;
            for (std::uint32_t k = ln.firstGlyph; k < ln.firstGlyph + ln.glyphCount; ++k) g[k].x += off;
        }
    }
    out.height = float(out.lines.size()) * lh;
}

// ---- Glyph atlas ---------------------------------------------------------------------------------------------------
GlyphAtlas::GlyphAtlas(int size) : m_size(size), m_pixels(std::size_t(size) * std::size_t(size), 0), m_slots(4096) {}

void GlyphAtlas::clear() {
    std::fill(m_pixels.begin(), m_pixels.end(), std::uint8_t(0));
    for (auto& s : m_slots) s = Slot{};
    m_used = 0;
    m_shelfX = 1;
    m_shelfY = 1;
    m_shelfH = 0;
    ++m_generation;
}

AtlasGlyph GlyphAtlas::get(const Font& font, std::uint32_t glyph, float px) {
    const std::uint32_t q = std::uint32_t(std::lround(px * 4.f));
    const std::uint64_t key = (std::uint64_t(font.id()) << 44) ^ (std::uint64_t(q) << 24) ^ glyph ^ (1ull << 63);
    const std::size_t mask = m_slots.size() - 1;
    std::size_t h = std::size_t((key * 0x9E3779B97F4A7C15ull) >> 40) & mask;
    for (;;) {
        Slot& s = m_slots[h];
        if (s.key == key) return s.g;
        if (s.key == 0) break;
        h = (h + 1) & mask;
    }
    ++m_misses;
    if (m_used * 4 >= m_slots.size() * 3) {
        clear();
        return get(font, glyph, px);
    }
    const float pxq = float(q) / 4.f;
    int x0, y0, x1, y1;
    font.glyphBox(glyph, pxq, x0, y0, x1, y1);
    const int w = x1 - x0, hh = y1 - y0;
    AtlasGlyph ag;
    ag.x0 = std::int16_t(x0);
    ag.y0 = std::int16_t(y0);
    if (w > 0 && hh > 0) {
        if (m_shelfX + w + 1 > m_size) {
            m_shelfX = 1;
            m_shelfY += m_shelfH + 1;
            m_shelfH = 0;
        }
        if (m_shelfY + hh + 1 > m_size) {
            clear();
            return get(font, glyph, px);
        }
        font.rasterise(glyph, pxq, m_pixels.data() + std::size_t(m_shelfY) * std::size_t(m_size) + std::size_t(m_shelfX),
                       w, hh, m_size);
        ag.u = std::uint16_t(m_shelfX);
        ag.v = std::uint16_t(m_shelfY);
        ag.w = std::uint16_t(w);
        ag.h = std::uint16_t(hh);
        m_shelfX += w + 1;
        m_shelfH = std::max(m_shelfH, hh);
    }
    ag.valid = true;
    m_slots[h] = {key, ag};
    ++m_used;
    return ag;
}

bool GlyphAtlas::lookup(std::uint64_t key, AtlasGlyph& out) const {
    const std::size_t mask = m_slots.size() - 1;
    for (std::size_t h = std::size_t((key * 0x9E3779B97F4A7C15ull) >> 40) & mask;; h = (h + 1) & mask) {
        const Slot& s = m_slots[h];
        if (s.key == key) { out = s.g; return true; }
        if (s.key == 0) return false;
    }
}

AtlasGlyph GlyphAtlas::insert(std::uint64_t key, const std::uint8_t* src, int w, int hh, int stride, int x0, int y0) {
    if (AtlasGlyph g; lookup(key, g)) return g;
    ++m_misses;
    if (m_used * 4 >= m_slots.size() * 3) clear();
    AtlasGlyph ag;
    ag.x0 = std::int16_t(x0);
    ag.y0 = std::int16_t(y0);
    if (w > 0 && hh > 0) {
        if (w + 2 > m_size || hh + 2 > m_size) return {};
        if (m_shelfX + w + 1 > m_size) {
            m_shelfX = 1;
            m_shelfY += m_shelfH + 1;
            m_shelfH = 0;
        }
        if (m_shelfY + hh + 1 > m_size) clear(); // full: start a new generation (shelf state reset)
        for (int y = 0; y < hh; ++y)
            std::memcpy(m_pixels.data() + std::size_t(m_shelfY + y) * std::size_t(m_size) + std::size_t(m_shelfX),
                        src + std::size_t(y) * std::size_t(stride), std::size_t(w));
        ag.u = std::uint16_t(m_shelfX);
        ag.v = std::uint16_t(m_shelfY);
        ag.w = std::uint16_t(w);
        ag.h = std::uint16_t(hh);
        m_shelfX += w + 1;
        m_shelfH = std::max(m_shelfH, hh);
    }
    ag.valid = true;
    const std::size_t mask = m_slots.size() - 1;
    std::size_t h = std::size_t((key * 0x9E3779B97F4A7C15ull) >> 40) & mask;
    while (m_slots[h].key != 0) h = (h + 1) & mask;
    m_slots[h] = {key, ag};
    ++m_used;
    return ag;
}

// ---- Editing -------------------------------------------------------------------------------------------------------
namespace {
bool isWordByte(char c) { return (static_cast<unsigned char>(c) >= 0x80u) || std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
}

void TextEditBuffer::setText(std::string_view s) {
    m_text.assign(s.substr(0, m_maxLength));
    m_cursor = m_anchor = m_text.size();
    m_undo.clear();
    m_redo.clear();
}
std::string_view TextEditBuffer::selection() const {
    const auto a = std::min(m_cursor, m_anchor), b = std::max(m_cursor, m_anchor);
    return std::string_view(m_text).substr(a, b - a);
}
void TextEditBuffer::record(std::string before, std::size_t cursorBefore) {
    m_undo.push_back({std::move(before), m_text, cursorBefore, m_cursor});
    m_redo.clear();
}
void TextEditBuffer::replaceSelection(std::string_view s) {
    const auto a = std::min(m_cursor, m_anchor), b = std::max(m_cursor, m_anchor);
    std::string before = m_text;
    const std::size_t cb = m_cursor;
    std::size_t room = m_maxLength - (m_text.size() - (b - a));
    if (s.size() > room) {
        // never split a code point
        std::size_t k = room;
        while (k > 0 && k < s.size() && (static_cast<unsigned char>(s[k]) & 0xC0u) == 0x80u) --k;
        s = s.substr(0, k);
    }
    m_text.replace(a, b - a, s);
    m_cursor = m_anchor = a + s.size();
    if (before != m_text) record(std::move(before), cb);
}
void TextEditBuffer::insert(std::string_view s) { replaceSelection(s); }
void TextEditBuffer::backspace() {
    if (!hasSelection()) m_anchor = prevBoundary(m_text, m_cursor);
    replaceSelection({});
}
void TextEditBuffer::del() {
    if (!hasSelection()) m_anchor = nextBoundary(m_text, m_cursor);
    replaceSelection({});
}
void TextEditBuffer::moveLeft(bool sel) {
    if (!sel && hasSelection()) m_cursor = std::min(m_cursor, m_anchor);
    else m_cursor = prevBoundary(m_text, m_cursor);
    if (!sel) m_anchor = m_cursor;
}
void TextEditBuffer::moveRight(bool sel) {
    if (!sel && hasSelection()) m_cursor = std::max(m_cursor, m_anchor);
    else m_cursor = nextBoundary(m_text, m_cursor);
    if (!sel) m_anchor = m_cursor;
}
void TextEditBuffer::wordLeft(bool sel) {
    std::size_t i = m_cursor;
    while (i > 0 && !isWordByte(m_text[i - 1])) --i;
    while (i > 0 && isWordByte(m_text[i - 1])) --i;
    m_cursor = i;
    if (!sel) m_anchor = m_cursor;
}
void TextEditBuffer::wordRight(bool sel) {
    std::size_t i = m_cursor;
    const std::size_t n = m_text.size();
    while (i < n && isWordByte(m_text[i])) ++i;
    while (i < n && !isWordByte(m_text[i])) ++i;
    m_cursor = i;
    if (!sel) m_anchor = m_cursor;
}
void TextEditBuffer::home(bool sel) {
    std::size_t i = m_cursor;
    while (i > 0 && m_text[i - 1] != '\n') --i;
    m_cursor = i;
    if (!sel) m_anchor = m_cursor;
}
void TextEditBuffer::end(bool sel) {
    std::size_t i = m_cursor;
    while (i < m_text.size() && m_text[i] != '\n') ++i;
    m_cursor = i;
    if (!sel) m_anchor = m_cursor;
}
void TextEditBuffer::selectAll() {
    m_anchor = 0;
    m_cursor = m_text.size();
}
bool TextEditBuffer::undo() {
    if (m_undo.empty()) return false;
    Edit e = std::move(m_undo.back());
    m_undo.pop_back();
    m_text = e.before;
    m_cursor = m_anchor = e.cursorBefore;
    m_redo.push_back(std::move(e));
    return true;
}
bool TextEditBuffer::redo() {
    if (m_redo.empty()) return false;
    Edit e = std::move(m_redo.back());
    m_redo.pop_back();
    m_text = e.after;
    m_cursor = m_anchor = e.cursorAfter;
    m_undo.push_back(std::move(e));
    return true;
}

} // namespace fuse::text
