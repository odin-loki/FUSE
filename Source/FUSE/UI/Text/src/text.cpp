#include "fuse/text/text.hpp"

#if !FUSE_TEXT_COOKED
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
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "font_var.hpp"
#include "glyph_paint.hpp"
#include "text_internal.hpp"

#if FUSE_TEXT_FULL
#include "text_full.hpp"
#endif
#if FUSE_TEXT_COOKED
#include "fusefont.hpp"
#include "text_cooked.hpp"
#endif

namespace fuse::text {

TextProfile textProfile() {
#if FUSE_TEXT_COOKED
    return TextProfile::Cooked;
#elif FUSE_TEXT_FULL
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
#if FUSE_TEXT_COOKED
    detail::MappedFile map;               // the pack when loaded with fromFile
    detail::CookedFace* cooked = nullptr; // owned; a view over `bytes` or `map`
#else
    stbtt_fontinfo info{};
#endif
    // Variations: `file` is `bytes`, or the owner's for an instance (which shares the owner's fallback chain).
    std::span<const std::uint8_t> file;
    const State* owner = nullptr;
    std::uint32_t fvar = 0;          // fvar table offset (0: not a variable font)
    detail::VarState* var = nullptr; // owned (font_var.cpp); made on first use of the variation API
    bool varied = false;             // coordinates are not the default: metrics / advances / outlines vary
    std::span<const std::int16_t> coords; // normalised coordinates when varied (owned by `var`)
    float dAscent = 0.f, dDescent = 0.f, dLineGap = 0.f; // MVAR deltas (font units)
    bool ownVar = true;              // false for feature variants: they borrow their source's variation state
    const State* featureSource = nullptr; // feature variant: the state it was made from
    struct FeatureVariant {
        std::array<FontFeature, 8> key{};
        std::size_t count = 0;
        std::unique_ptr<Font> font;
    };
    std::vector<FeatureVariant> variants; // withFeatures (owned)
    // ---- weight-following fallbacks ----
    float weightClass = 400.f;               // OS/2 usWeightClass (an instance: its owner's)
    float weightCache = -1.f;                // weight(): computed on first use
    float synthBold = 0.f;                   // emboldened(): extra stroke width and advance as a fraction of the em
    const Font* boldFace = nullptr;          // setBoldFace
    std::unique_ptr<Font> emboldenedFont;    // emboldened() (owned)
    std::vector<const Font*> boldChain;      // chain() with each face replaced by its bold form (bold fonts only)
    std::uint32_t boldChainGen = 0;          // g_chainGen it was built at
    const std::vector<const Font*>& chain() const { return owner ? owner->fallbacks : fallbacks; }
    ~State() {
        variants.clear();
        emboldenedFont.reset();
        if (var && ownVar) detail::varHooks().destroy(var); // instances first: they point into this state
#if FUSE_TEXT_COOKED
        detail::cookedDestroy(cooked);
#endif
#if FUSE_TEXT_FULL
        detail::fullDestroy(full);
#endif
    }
    float unitsPerPixelScale1 = 0.f; // stbtt_ScaleForPixelHeight(info, 1)
    float emPerPixelSize = 1.f;      // em size in pixels for pixelSize 1 (pixel sizes are ascent-to-descent)
    std::vector<const Font*> fallbacks;
    // Direct-mapped lookup caches (layout asks for the same code points and pairs over and over; GPOS kerning and
    // fallback cmap probes are the MINIMAL hot spots). cmap: cp << 32 | glyph; kerning: (l << 16 | r) << 32 | units.
    std::array<std::uint64_t, 1024> cmapCache = filledCache();
    std::array<std::uint64_t, 2048> kernCache = filledCache<2048>();
    template <std::size_t N = 1024> static std::array<std::uint64_t, N> filledCache() {
        std::array<std::uint64_t, N> a;
        a.fill(~std::uint64_t(0));
        return a;
    }
    std::vector<ColorLayer> layers;                       // COLRv0 layer records with CPAL palette 0 resolved
    std::vector<std::array<std::uint32_t, 3>> colorBase; // (glyph, first layer, layer count), sorted by glyph
    // image colour glyphs: COLRv1 graphs without a v0 record, bitmap strikes (sorted glyph ids, one image layer each)
    std::uint32_t colr = 0, cpal = 0, cblc = 0, cbdt = 0, sbix = 0;
    std::uint16_t unitsPerEm = 1000, numGlyphs = 0;
    std::vector<ColorLayer> v1Images, bitmapImages;
#if FUSE_TEXT_FULL
    detail::FullFace* full = nullptr; // owned; HarfBuzz + FreeType over `bytes`
#endif
};

namespace {
std::atomic<std::uint32_t> g_fontIds{1};
// Bumped whenever a fallback chain, a bold face or a pinned variation changes: bold fonts rebuild their bold chain.
std::atomic<std::uint32_t> g_chainGen{1};

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

#if !FUSE_TEXT_COOKED
// COLR (version 0 layers, plus version 1 paint graphs flattened to solid layers) + CPAL palette 0. Bounds-checked: a
// malformed table leaves the font without colour glyphs.
void loadColr(std::vector<std::uint8_t>& bytes, std::vector<ColorLayer>& layers,
              std::vector<std::array<std::uint32_t, 3>>& colorBase, std::uint32_t fontStart,
              std::vector<ColorLayer>& v1Images) {
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
        ColorLayer img;
        img.glyph = glyph;
        img.image = true;
        v1Images.push_back(img);
        const std::uint32_t start = std::uint32_t(layers.size());
        v1.budget = 4096;
        v1.paint(v1.baseList + be32(r + 2), 0);
        if (layers.size() > start) colorBase.push_back({glyph, start, std::uint32_t(layers.size()) - start});
    }
    std::sort(colorBase.begin(), colorBase.end());
}
#endif
} // namespace

#if FUSE_TEXT_COOKED
// Takes ownership of `st`; `pack` (st's bytes or mapping) holds a .fusefont.
std::expected<Font, TextError> Font::fromCooked(State* st, std::span<const std::uint8_t> pack) {
    st->cooked = detail::cookedCreate(pack);
    if (!st->cooked) {
        delete st;
        return std::unexpected(TextError::InvalidFont);
    }
    const detail::CookedInfo& ci = detail::cookedInfo(st->cooked);
    st->unitsPerPixelScale1 = ci.unitsPerPixelScale1;
    st->emPerPixelSize = ci.emPerPixelSize;
    st->unitsPerEm = ci.unitsPerEm;
    st->numGlyphs = ci.numGlyphs;
    detail::cookedColorTables(st->cooked, st->layers, st->colorBase, st->v1Images);
    Font f;
    f.m_state = st;
    f.m_id = g_fontIds.fetch_add(1);
    return f;
}
#endif

std::expected<Font, TextError> Font::fromBytes(std::span<const std::uint8_t> bytes, std::uint32_t faceIndex) {
    if (bytes.size() < 12) return std::unexpected(TextError::InvalidFont);
    auto* st = new State;
    st->bytes.assign(bytes.begin(), bytes.end());
#if FUSE_TEXT_COOKED
    (void)faceIndex; // a .fusefont pack holds one face
    return fromCooked(st, st->bytes);
#else
    const int offset = stbtt_GetFontOffsetForIndex(st->bytes.data(), int(faceIndex));
    if (offset < 0 || !stbtt_InitFont(&st->info, st->bytes.data(), offset)) {
        delete st;
        return std::unexpected(TextError::InvalidFont);
    }
    st->file = st->bytes;
    st->fvar = stbtt__find_table(st->bytes.data(), std::uint32_t(offset), "fvar");
    st->unitsPerPixelScale1 = stbtt_ScaleForPixelHeight(&st->info, 1.f);
    st->emPerPixelSize = st->unitsPerPixelScale1 / stbtt_ScaleForMappingEmToPixels(&st->info, 1.f);
    loadColr(st->bytes, st->layers, st->colorBase, std::uint32_t(offset), st->v1Images);
    std::sort(st->v1Images.begin(), st->v1Images.end(), [](const ColorLayer& a, const ColorLayer& b) { return a.glyph < b.glyph; });
    {
        std::uint8_t* d = st->bytes.data();
        const std::size_t n = st->bytes.size();
        st->colr = stbtt__find_table(d, std::uint32_t(offset), "COLR");
        st->cpal = stbtt__find_table(d, std::uint32_t(offset), "CPAL");
        st->cblc = stbtt__find_table(d, std::uint32_t(offset), "CBLC");
        st->cbdt = stbtt__find_table(d, std::uint32_t(offset), "CBDT");
        st->sbix = stbtt__find_table(d, std::uint32_t(offset), "sbix");
        if (const std::uint32_t head = stbtt__find_table(d, std::uint32_t(offset), "head"); head && head + 20 <= n)
            st->unitsPerEm = be16(d + head + 18);
        if (const std::uint32_t maxp = stbtt__find_table(d, std::uint32_t(offset), "maxp"); maxp && maxp + 6 <= n)
            st->numGlyphs = be16(d + maxp + 4);
        if (const std::uint32_t os2 = stbtt__find_table(d, std::uint32_t(offset), "OS/2"); os2 && os2 + 6 <= n)
            if (const std::uint16_t wc = be16(d + os2 + 4); wc >= 1 && wc <= 1000) st->weightClass = float(wc);
        if (((st->cblc && st->cbdt) || st->sbix) && detail::paintHooks().bitmapList) {
            std::vector<std::uint32_t> ids;
            detail::paintHooks().bitmapList({st->bytes, st->cblc, st->cbdt, st->sbix, st->unitsPerEm, st->numGlyphs}, ids);
            for (const std::uint32_t g : ids) {
                ColorLayer img;
                img.glyph = g;
                img.image = true;
                st->bitmapImages.push_back(img);
            }
        }
    }
#endif
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
#if FUSE_TEXT_COOKED
    (void)faceIndex;
    auto* st = new State; // packs are mapped, not read: their pages load on first use
    if (!st->map.open(path)) {
        delete st;
        return std::unexpected(TextError::InvalidFont);
    }
    return fromCooked(st, {st->map.data(), st->map.size()});
#else
    std::vector<std::uint8_t> data;
    if (!detail::readFile(path, data)) return std::unexpected(TextError::InvalidFont);
    return fromBytes(data, faceIndex);
#endif
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
#if FUSE_TEXT_COOKED // cooked at build time into the build tree (same names, .fusefont)
    static const char* const kFiles[] = {"noto/NotoSansHebrew-Regular.fusefont", "noto/NotoSansArabic-Regular.fusefont",
                                         "noto/NotoSansDevanagari-Regular.fusefont", "twemoji/TwemojiMozilla.fusefont",
                                         "noto/NotoSansSymbols2-Regular.fusefont", "noto/NotoSansThai-Regular.fusefont",
                                         "noto/NotoSansCJKsc-Subset.fusefont"};
#else
    static const char* const kFiles[] = {"noto/NotoSansHebrew-Regular.ttf", "noto/NotoSansArabic-Regular.ttf",
                                         "noto/NotoSansDevanagari-Regular.ttf", "twemoji/TwemojiMozilla.ttf",
                                         "noto/NotoSansSymbols2-Regular.ttf", "noto/NotoSansThai-Regular.ttf",
                                         "noto/NotoSansCJKsc-Subset.otf"};
#endif
    for (const char* f : kFiles)
        if (auto font = fromFile(std::string(FUSE_TEXT_FONT_DIR) + "/" + f)) out.push_back(std::move(*font));
#endif
    return out;
}

void Font::setFallbacks(std::vector<const Font*> chain) {
    if (!m_state) return;
    std::erase_if(chain, [&](const Font* f) { return !f || !f->valid() || f == this; });
    m_state->fallbacks = std::move(chain);
    g_chainGen.fetch_add(1, std::memory_order_relaxed);
}
void Font::setBoldFace(const Font* bold) {
    if (!m_state || bold == this) return;
    m_state->boldFace = bold && bold->valid() ? bold : nullptr;
    g_chainGen.fetch_add(1, std::memory_order_relaxed);
}
const Font* Font::boldFace() const { return m_state ? m_state->boldFace : nullptr; }
float Font::weight() const {
    if (!m_state) return 400.f;
    State& st = *m_state;
    if (st.weightCache >= 0.f) return st.weightCache;
    float w = st.weightClass;
    const auto ax = axes();
    const auto co = variationCoords();
    for (std::size_t k = 0; k < ax.size() && k < co.size(); ++k)
        if (ax[k].tag == fontTag("wght")) w = co[k];
    st.weightCache = w;
    return w;
}
namespace {
// The face fallback text of a primary at weight `w` (>= 600) uses: the face set with setBoldFace, else a variable
// face's instance at that weight, else synthetic bold (colour fonts stay as they are).
const Font& boldFallback(const Font& c, float w) {
    if (const Font* b = c.boldFace()) return *b;
    if (c.isVariable())
        for (const VariationAxis& a : c.axes())
            if (a.tag == fontTag("wght")) {
                const FontVariation v{fontTag("wght"), w};
                return c.instance(std::span<const FontVariation>(&v, 1));
            }
    return c.emboldened();
}
} // namespace
std::size_t Font::faceCount() const { return m_state ? 1 + m_state->chain().size() : 1; }
const Font& Font::face(std::uint16_t i) const {
    if (i == 0 || !m_state) return *this;
    State& st = *m_state;
    const auto& chain = st.chain();
    if (i > chain.size()) return *this;
    if (const std::uint32_t gen = g_chainGen.load(std::memory_order_relaxed); st.boldChainGen != gen) {
        st.boldChain.clear();
        if (const float w = weight(); w >= 600.f)
            for (const Font* c : chain) st.boldChain.push_back(&boldFallback(*c, w));
        st.boldChainGen = gen;
    }
    return st.boldChain.empty() ? *chain[i - 1] : *st.boldChain[i - 1];
}
std::uint16_t Font::faceFor(char32_t cp) const {
    if (!m_state) return 0;
    if (glyphIndex(cp)) return 0;
    const auto& chain = m_state->chain();
    for (std::size_t k = 0; k < chain.size(); ++k)
        if (chain[k]->glyphIndex(cp)) return std::uint16_t(k + 1);
    return 0;
}
float Font::facePixelSize(std::uint16_t i, float px) const {
    const Font& f = face(i);
    if (&f == this) return px;
    return px * m_state->emPerPixelSize / f.m_state->emPerPixelSize;
}
bool Font::hasColorGlyphs() const {
    return m_state && (!m_state->colorBase.empty() || !m_state->v1Images.empty() || !m_state->bitmapImages.empty());
}
namespace {
const ColorLayer* findImage(const std::vector<ColorLayer>& v, std::uint32_t glyph) {
    auto it = std::lower_bound(v.begin(), v.end(), glyph, [](const ColorLayer& e, std::uint32_t g) { return e.glyph < g; });
    return it != v.end() && it->glyph == glyph ? &*it : nullptr;
}
} // namespace
std::span<const ColorLayer> Font::colorLayers(std::uint32_t glyph) const {
    if (!m_state) return {};
    const detail::PaintHooks& hooks = detail::paintHooks();
    if (hooks.colorEnabled) {
        if (const ColorLayer* l = findImage(m_state->bitmapImages, glyph)) return {l, 1};
        if (const ColorLayer* l = findImage(m_state->v1Images, glyph)) return {l, 1};
    }
    if (m_state->colorBase.empty()) return {};
    const auto& b = m_state->colorBase;
    auto it = std::lower_bound(b.begin(), b.end(), glyph, [](const auto& e, std::uint32_t g) { return e[0] < g; });
    if (it == b.end() || (*it)[0] != glyph) return {};
    return std::span<const ColorLayer>(m_state->layers.data() + (*it)[1], (*it)[2]);
}

// Variations go through varHooks() (font_var.cpp, installed by enableVariableFonts) so binaries that never enable
// them do not link the glyf / gvar decoder. COOKED packs hold one instance: no variation data at run time.
bool Font::isVariable() const { return m_state && m_state->fvar != 0; }
std::span<const VariationAxis> Font::axes() const {
    const auto fn = detail::varHooks().axes;
    return fn && isVariable() ? fn(*this) : std::span<const VariationAxis>{};
}
std::span<const NamedInstance> Font::namedInstances() const {
    const auto fn = detail::varHooks().named;
    return fn && isVariable() ? fn(*this) : std::span<const NamedInstance>{};
}
std::span<const float> Font::variationCoords() const {
    const auto fn = detail::varHooks().coords;
    return fn && isVariable() ? fn(*this) : std::span<const float>{};
}
const Font& Font::instance(std::span<const FontVariation> settings) const {
    const auto fn = detail::varHooks().instance;
    return fn && isVariable() ? fn(*this, settings) : *this;
}
const Font& Font::namedInstance(std::size_t i) const {
    const auto named = namedInstances();
    const auto ax = axes();
    if (i >= named.size()) return *this;
    std::array<FontVariation, detail::kMaxAxes> s;
    std::size_t n = 0;
    for (; n < ax.size() && n < named[i].coords.size() && n < s.size(); ++n) s[n] = {ax[n].tag, named[i].coords[n]};
    return instance(std::span<const FontVariation>(s.data(), n));
}
void Font::setVariation(std::span<const FontVariation> settings) {
    if (const auto fn = detail::varHooks().setVariation; fn && isVariable()) {
        fn(*this, settings);
        m_state->weightCache = -1.f;
        g_chainGen.fetch_add(1, std::memory_order_relaxed);
    }
}
bool variableFontsEnabled() { return detail::varHooks().instance != nullptr; }

const Font& Font::withFeatures(std::span<const FontFeature> features) const {
#if FUSE_TEXT_FULL
    if (!m_state || !m_state->full) return *this;
    State* src = const_cast<State*>(m_state->featureSource ? m_state->featureSource : m_state);
    if (features.empty()) return *this;
    const std::size_t n = std::min<std::size_t>(features.size(), 8);
    for (const State::FeatureVariant& v : src->variants)
        if (v.count == n && std::equal(features.begin(), features.begin() + std::ptrdiff_t(n), v.key.begin(),
                                       [](const FontFeature& a, const FontFeature& b) { return a.tag == b.tag && a.value == b.value; }))
            return *v.font;
    if (src->variants.size() >= 64) return *src->variants.back().font;
    State* st = detail::FontAccess::shareState(src);
    st->var = src->var; // borrowed: same coordinates as the source
    st->ownVar = false;
    st->varied = src->varied;
    st->coords = src->coords;
    st->dAscent = src->dAscent;
    st->dDescent = src->dDescent;
    st->dLineGap = src->dLineGap;
    st->featureSource = src;
    st->synthBold = src->synthBold; // an emboldened() font keeps its synthetic bold
    st->weightCache = src->weightCache;
    detail::fullSetCoords(st->full, st->coords);
    detail::fullSetFeatures(st->full, features.first(n));
    State::FeatureVariant v;
    std::copy(features.begin(), features.begin() + std::ptrdiff_t(n), v.key.begin());
    v.count = n;
    v.font = std::make_unique<Font>();
    v.font->m_state = st;
    v.font->m_id = g_fontIds.fetch_add(1);
    src->variants.push_back(std::move(v));
    return *src->variants.back().font;
#else
    (void)features; // no GSUB / GPOS shaping
    return *this;
#endif
}
const Font& Font::emboldened() const {
#if FUSE_TEXT_COOKED
    return *this; // packs are cooked at one instance
#else
    if (!m_state || m_state->synthBold > 0.f || hasColorGlyphs() || weight() >= 600.f) return *this;
    State* src = m_state;
    if (!src->emboldenedFont) {
        State* st = detail::FontAccess::shareState(src);
        st->var = src->var; // borrowed: same coordinates as the source
        st->ownVar = false;
        st->varied = src->varied;
        st->coords = src->coords;
        st->dAscent = src->dAscent;
        st->dDescent = src->dDescent;
        st->dLineGap = src->dLineGap;
        st->weightCache = 700.f;
        st->synthBold = 1.f / 24.f; // FreeType's FT_GlyphSlot_Embolden strength
#if FUSE_TEXT_FULL
        detail::fullSetCoords(st->full, st->coords);
#endif
        auto f = std::make_unique<Font>();
        f->m_state = st;
        f->m_id = g_fontIds.fetch_add(1);
        src->emboldenedFont = std::move(f);
    }
    return *src->emboldenedFont;
#endif
}
float Font::syntheticBold(float px) const { return m_state ? m_state->synthBold * m_state->emPerPixelSize * px : 0.f; }

#if FUSE_TEXT_COOKED
void enableVariableFonts() {}
#endif

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
#if FUSE_TEXT_COOKED
    const detail::CookedInfo& ci = detail::cookedInfo(m_state->cooked);
    a = ci.ascent;
    d = ci.descent;
    g = ci.lineGap;
#else
    stbtt_GetFontVMetrics(&m_state->info, &a, &d, &g);
#endif
    const float s = m_state->unitsPerPixelScale1 * px;
    return {(float(a) + m_state->dAscent) * s, (float(d) + m_state->dDescent) * s, (float(g) + m_state->dLineGap) * s};
}
std::uint32_t Font::glyphIndex(char32_t cp) const {
    std::uint64_t& slot = m_state->cmapCache[(cp * 0x9E3779B1u) >> 22];
    if (std::uint32_t(slot >> 32) == std::uint32_t(cp)) return std::uint32_t(slot);
#if FUSE_TEXT_COOKED
    const std::uint32_t g = detail::cookedGlyphIndex(m_state->cooked, cp);
#else
    const std::uint32_t g = std::uint32_t(stbtt_FindGlyphIndex(&m_state->info, int(cp)));
#endif
    slot = (std::uint64_t(cp) << 32) | g;
    return g;
}
float Font::advance(std::uint32_t glyph, float px) const {
    const float a = baseAdvance(glyph, px);
    return m_state->synthBold > 0.f && a > 0.f ? a + syntheticBold(px) : a;
}
float Font::baseAdvance(std::uint32_t glyph, float px) const {
#if FUSE_TEXT_COOKED
    const int adv = detail::cookedAdvance(m_state->cooked, glyph);
#else
    if (m_state->varied) return detail::varHooks().advance(m_state->var, glyph) * m_state->unitsPerPixelScale1 * px;
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&m_state->info, int(glyph), &adv, &lsb);
#endif
    return float(adv) * m_state->unitsPerPixelScale1 * px;
}
float Font::kerning(std::uint32_t l, std::uint32_t r, float px) const {
    // glyph ids are 16-bit; the all-ones key (65535, 65535) is the empty slot, so that pair is never cached
    const std::uint32_t key = ((l & 0xFFFFu) << 16) | (r & 0xFFFFu);
    std::uint64_t& slot = m_state->kernCache[(key * 0x9E3779B1u) >> 21];
    int units = 0;
    if (std::uint32_t(slot >> 32) == key && key != ~0u) {
        units = int(std::int32_t(std::uint32_t(slot)));
    } else {
#if FUSE_TEXT_COOKED
        units = detail::cookedKern(m_state->cooked, l, r);
#else
        units = stbtt_GetGlyphKernAdvance(&m_state->info, int(l), int(r));
#endif
        slot = (std::uint64_t(key) << 32) | std::uint32_t(units);
    }
    return float(units) * m_state->unitsPerPixelScale1 * px;
}
void Font::glyphBox(std::uint32_t glyph, float px, int& x0, int& y0, int& x1, int& y1) const {
    baseGlyphBox(glyph, px, x0, y0, x1, y1);
    if (m_state->synthBold > 0.f && x1 > x0) x1 += int(std::ceil(syntheticBold(px)));
}
void Font::rasterise(std::uint32_t glyph, float px, std::uint8_t* dst, int w, int h, int stride) const {
    if (!(m_state->synthBold > 0.f)) return baseRasterise(glyph, px, dst, w, h, stride);
    // Synthetic bold: the glyph smeared to the right by the strength (GDI-style double strike, fractional last
    // column), inside the box glyphBox widened by the same amount.
    const float e = syntheticBold(px);
    const int ext = int(std::ceil(e)), whole = int(std::floor(e));
    const float frac = e - float(whole);
    const int bw = std::max(0, w - ext);
    for (int y = 0; y < h; ++y) std::memset(dst + std::ptrdiff_t(y) * stride, 0, std::size_t(w));
    baseRasterise(glyph, px, dst, bw, h, stride);
    for (int y = 0; y < h; ++y) {
        std::uint8_t* row = dst + std::ptrdiff_t(y) * stride;
        for (int x = w - 1; x >= 0; --x) {
            float v = row[x];
            for (int k = 1; k <= whole && x - k >= 0; ++k) v = std::max(v, float(row[x - k]));
            if (frac > 0.f && x - whole - 1 >= 0) v = std::max(v, float(row[x - whole - 1]) * frac);
            row[x] = std::uint8_t(std::lround(v));
        }
    }
}
void Font::baseGlyphBox(std::uint32_t glyph, float px, int& x0, int& y0, int& x1, int& y1) const {
#if FUSE_TEXT_FULL
    return detail::fullGlyphBox(m_state->full, glyph, m_state->emPerPixelSize * px, x0, y0, x1, y1);
#endif
#if FUSE_TEXT_COOKED
    return detail::cookedGlyphBox(m_state->cooked, glyph, px, x0, y0, x1, y1);
#else
    const float s = m_state->unitsPerPixelScale1 * px;
    if (m_state->varied) return detail::varHooks().glyphBox(m_state->var, glyph, s, x0, y0, x1, y1);
    stbtt_GetGlyphBitmapBox(&m_state->info, int(glyph), s, s, &x0, &y0, &x1, &y1);
#endif
}
void Font::baseRasterise(std::uint32_t glyph, float px, std::uint8_t* dst, int w, int h, int stride) const {
#if FUSE_TEXT_FULL
    return detail::fullRasterise(m_state->full, glyph, m_state->emPerPixelSize * px, dst, w, h, stride);
#endif
#if FUSE_TEXT_COOKED
    return detail::cookedRasterise(m_state->cooked, glyph, px, dst, w, h, stride);
#else
    const float s = m_state->unitsPerPixelScale1 * px;
    if (m_state->varied) return detail::varHooks().rasterise(m_state->var, glyph, s, dst, w, h, stride);
    stbtt_MakeGlyphBitmap(&m_state->info, dst, w, h, stride, s, s, int(glyph));
#endif
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

namespace detail {
const Font::State* FontAccess::state(const Font& f) { return f.m_state; }
VarState*& FontAccess::var(const Font& f) { return f.m_state->var; }
std::span<const std::uint8_t> FontAccess::file(const Font& f) {
    return f.m_state ? f.m_state->file : std::span<const std::uint8_t>{};
}
std::uint32_t FontAccess::fontStart(const Font& f) {
#if FUSE_TEXT_COOKED
    (void)f;
    return 0;
#else
    return f.m_state ? std::uint32_t(f.m_state->info.fontstart) : 0;
#endif
}
VarHooks& varHooks() {
    static VarHooks hooks;
    return hooks;
}
#if !FUSE_TEXT_COOKED
void FontAccess::setVaried(const Font& f, std::span<const std::int16_t> coords, float dAscent, float dDescent, float dLineGap) {
    Font::State& st = *f.m_state;
    const bool varied = std::any_of(coords.begin(), coords.end(), [](std::int16_t c) { return c != 0; });
    st.varied = varied;
    st.coords = varied ? coords : std::span<const std::int16_t>{};
    st.dAscent = dAscent;
    st.dDescent = dDescent;
    st.dLineGap = dLineGap;
#if FUSE_TEXT_FULL
    fullSetCoords(st.full, st.coords);
#endif
}
// A state over `src`'s file (the owner's bytes), colour tables and fallback chain.
Font::State* FontAccess::shareState(const Font::State* src) {
    const Font::State* o = src->owner ? src->owner : src;
    auto* st = new Font::State;
    st->info = o->info; // points into the owner's bytes
    st->file = o->file;
    st->owner = o;
    st->fvar = o->fvar;
    st->unitsPerPixelScale1 = o->unitsPerPixelScale1; // pixel sizes keep the default instance's scale
    st->emPerPixelSize = o->emPerPixelSize;
    st->layers = o->layers;
    st->colorBase = o->colorBase;
    st->colr = o->colr;
    st->cpal = o->cpal;
    st->cblc = o->cblc;
    st->cbdt = o->cbdt;
    st->sbix = o->sbix;
    st->unitsPerEm = o->unitsPerEm;
    st->numGlyphs = o->numGlyphs;
    st->weightClass = o->weightClass;
    st->v1Images = o->v1Images;
    st->bitmapImages = o->bitmapImages;
#if FUSE_TEXT_FULL
    st->full = fullCreateInstance(o->full);
#endif
    return st;
}

std::unique_ptr<Font> FontAccess::makeInstance(const Font& from, VarState* v, std::span<const std::int16_t> coords,
                                               float dAscent, float dDescent, float dLineGap) {
    Font::State* st = shareState(from.m_state);
    st->var = v;
    auto f = std::make_unique<Font>();
    f->m_state = st;
    f->m_id = g_fontIds.fetch_add(1);
    setVaried(*f, coords, dAscent, dDescent, dLineGap);
    return f;
}
#endif

PaintHooks& paintHooks() {
    static PaintHooks hooks;
    return hooks;
}

bool glyphOutline(const Font& font, std::uint32_t glyph, std::vector<OutlineCmd>& out) {
    out.clear();
    const auto* st = FontAccess::state(font);
#if FUSE_TEXT_COOKED
    (void)st;
    (void)glyph;
    return false; // cooked packs carry no outlines
#else
    if (!st) return false;
    if (st->varied) return varHooks().outline(st->var, glyph, out);
    stbtt_vertex* v = nullptr;
    const int n = stbtt_GetGlyphShape(&st->info, int(glyph), &v);
    out.reserve(std::size_t(std::max(n, 0)));
    for (int i = 0; i < n; ++i) {
        OutlineCmd c;
        switch (v[i].type) {
        case STBTT_vmove: c.kind = OutlineCmd::Move; break;
        case STBTT_vline: c.kind = OutlineCmd::Line; break;
        case STBTT_vcurve: c.kind = OutlineCmd::Quad; break;
        default: c.kind = OutlineCmd::Cubic; break;
        }
        c.x = float(v[i].x);
        c.y = float(v[i].y);
        c.cx = float(v[i].cx);
        c.cy = float(v[i].cy);
        c.cx1 = float(v[i].cx1);
        c.cy1 = float(v[i].cy1);
        out.push_back(c);
    }
    stbtt_FreeShape(&st->info, v);
    return !out.empty();
#endif
}
} // namespace detail

#if !FUSE_TEXT_COOKED
// ---- what the font cooker (font_cook.cpp) reads ----
namespace detail {
CookSource cookSource(const Font& font) {
    CookSource c;
    const auto* st = FontAccess::state(font);
    if (!st) return c;
    stbtt_GetFontVMetrics(&st->info, &c.ascent, &c.descent, &c.lineGap);
    c.ascent = int(std::lround(float(c.ascent) + st->dAscent));
    c.descent = int(std::lround(float(c.descent) + st->dDescent));
    c.lineGap = int(std::lround(float(c.lineGap) + st->dLineGap));
    c.unitsPerPixelScale1 = st->unitsPerPixelScale1;
    c.emPerPixelSize = st->emPerPixelSize;
    c.unitsPerEm = st->unitsPerEm;
    c.numGlyphs = st->numGlyphs;
    return c;
}
int advanceUnits(const Font& font, std::uint32_t glyph) {
    if (const auto* st = FontAccess::state(font); st->varied) return int(std::lround(varHooks().advance(st->var, glyph)));
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&FontAccess::state(font)->info, int(glyph), &adv, &lsb);
    return adv;
}
int kernUnits(const Font& font, std::uint32_t l, std::uint32_t r) {
    return stbtt_GetGlyphKernAdvance(&FontAccess::state(font)->info, int(l), int(r));
}
bool hasKerning(const Font& font) {
    const auto* st = FontAccess::state(font);
    return st->info.kern != 0 || st->info.gpos != 0;
}
void rawColorTables(const Font& font, std::vector<ColorLayer>& layers, std::vector<std::array<std::uint32_t, 3>>& base) {
    const auto* st = FontAccess::state(font);
    layers = st->layers;
    base = st->colorBase;
}
void imageGlyphs(const Font& font, std::vector<std::uint32_t>& out) {
    const auto* st = FontAccess::state(font);
    out.clear();
    for (const ColorLayer& l : st->v1Images) out.push_back(l.glyph);
    for (const ColorLayer& l : st->bitmapImages) out.push_back(l.glyph);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}
} // namespace detail
#endif

bool Font::renderColor(std::uint32_t glyph, float px, std::uint32_t foreground, ColorImage& out) const {
    out = {};
    if (!m_state) return false;
    const detail::PaintHooks& hooks = detail::paintHooks();
    const State& st = *m_state;
#if FUSE_TEXT_COOKED
    (void)hooks;
    return hooks.colorEnabled && detail::cookedColor(st.cooked, glyph, px, foreground, out);
#else
    if (hooks.bitmapRender && findImage(st.bitmapImages, glyph))
        return hooks.bitmapRender({st.file, st.cblc, st.cbdt, st.sbix, st.unitsPerEm, st.numGlyphs}, glyph,
                                  unitScale(px) * float(st.unitsPerEm), out);
    if (hooks.colrRender && findImage(st.v1Images, glyph))
        return hooks.colrRender({this, st.file, st.colr, st.cpal, unitScale(px), st.coords}, glyph,
                                foreground, out);
    return false;
#endif
}

bool Font::renderMsdf(std::uint32_t glyph, float px, float range, ColorImage& out) const {
    out = {};
    const detail::PaintHooks& hooks = detail::paintHooks();
#if FUSE_TEXT_COOKED
    return m_state && hooks.msdfEnabled && detail::cookedMsdf(m_state->cooked, glyph, px, range, out);
#else
    if (!m_state || !hooks.msdf || !(range > 0.f)) return false;
    thread_local std::vector<detail::OutlineCmd> path;
    if (!detail::glyphOutline(*this, glyph, path)) return false;
    int ux0 = 0, uy0 = 0, ux1 = 0, uy1 = 0;
    if (m_state->varied) { // bounds of the varied outline (end and control points)
        float b[4] = {1e30f, 1e30f, -1e30f, -1e30f};
        for (const detail::OutlineCmd& c : path) {
            const bool ctl = c.kind == detail::OutlineCmd::Quad || c.kind == detail::OutlineCmd::Cubic;
            b[0] = std::min({b[0], c.x, ctl ? c.cx : c.x});
            b[1] = std::min({b[1], c.y, ctl ? c.cy : c.y});
            b[2] = std::max({b[2], c.x, ctl ? c.cx : c.x});
            b[3] = std::max({b[3], c.y, ctl ? c.cy : c.y});
        }
        ux0 = int(std::floor(b[0]));
        uy0 = int(std::floor(b[1]));
        ux1 = int(std::ceil(b[2]));
        uy1 = int(std::ceil(b[3]));
    } else if (!stbtt_GetGlyphBox(&m_state->info, int(glyph), &ux0, &uy0, &ux1, &uy1)) {
        return false;
    }
    const float s = unitScale(px);
    const float pad = range * 0.5f + 1.f;
    const float bold = syntheticBold(px); // synthetic bold: distances biased outward by half the strength, shifted right
    ux1 += int(std::ceil(bold / s));
    out.x0 = int(std::floor(float(ux0) * s - pad));
    out.y0 = int(std::floor(-float(uy1) * s - pad));
    out.w = int(std::ceil(float(ux1) * s + pad)) - out.x0;
    out.h = int(std::ceil(-float(uy0) * s + pad)) - out.y0;
    if (out.w <= 0 || out.h <= 0 || out.w > 2048 || out.h > 2048) return false;
    out.rgba.assign(std::size_t(out.w) * std::size_t(out.h) * 4, 0);
    detail::Affine m;
    m.xx = s;
    m.yy = -s;
    m.dx = -float(out.x0) + bold * 0.5f;
    m.dy = -float(out.y0);
    hooks.msdf(path, m, range, out.rgba.data(), out.w, out.h, out.w * 4);
    if (bold > 0.f) {
        const int bias = int(std::lround(255.f * bold * 0.5f / range));
        for (std::uint8_t& c : out.rgba) c = std::uint8_t(std::min(255, int(c) + bias));
    }
    return true;
#endif
}

#if FUSE_TEXT_FULL
float HarfBuzzShaper::shape(const Font& font, float px, std::string_view s, std::vector<ShapedGlyph>& out) const {
    const float w = detail::fullShape(font.m_state->full, font.unitScale(px), s, out);
    if (!(font.m_state->synthBold > 0.f)) return w;
    const float e = font.syntheticBold(px);
    float shift = 0.f;
    for (ShapedGlyph& g : out) { // widen every spacing glyph, push the rest along
        g.x += shift;
        if (g.advance > 0.f) {
            g.advance += e;
            shift += e;
        }
    }
    return w + shift;
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
    if (cp < 0x300 && prev != 0x200D) return false; // no Extend / ZWJ / SpacingMark below U+0300
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
        // Fallback runs are shaped once, by their own face, inside the bidi / line-breaking pass.
        thread_local std::vector<detail::FullRunFace> faces;
        thread_local std::vector<detail::FullFaceSpan> spans;
        const float px = desc.pixelSize;
        faces.assign(1, {font.m_state->full, font.unitScale(px), font.syntheticBold(px)});
        spans.clear();
        if (mayNeedFallback(font, s)) {
            forEachFaceRun(font, s, [&](std::size_t b, std::size_t e, std::uint16_t f) {
                if (f) spans.push_back({std::uint32_t(b), std::uint32_t(e), f});
            });
            if (!spans.empty())
                for (std::uint16_t k = 1; k < font.faceCount(); ++k) {
                    const Font& fk = font.face(k);
                    const float fpx = font.facePixelSize(k, px);
                    faces.push_back({fk.m_state->full, fk.unitScale(fpx), fk.syntheticBold(fpx)});
                }
            std::erase_if(spans, [](const detail::FullFaceSpan& sp) { return !faces[sp.face].face; });
        }
        detail::fullLayout(faces, spans, s, wrap ? desc.maxWidth : 0.f, lh, out.metrics.ascent, g, out.lines);
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
    if (desc.align != TextAlign::Start || anyRtl || desc.rtl) {
        for (const auto& ln : out.lines) {
            // Start / End follow the paragraph (or base) direction: Start is the right edge of an RTL line.
            const bool rtl = ln.rtl || desc.rtl;
            const bool toEnd = desc.align == TextAlign::Right || (desc.align == TextAlign::End && !rtl) ||
                               (desc.align == TextAlign::Start && rtl);
            if (desc.align != TextAlign::Center && !toEnd) continue;
            const float off = desc.align == TextAlign::Center ? std::floor((alignW - ln.width) * 0.5f) : alignW - ln.width;
            for (std::uint32_t k = ln.firstGlyph; k < ln.firstGlyph + ln.glyphCount; ++k) g[k].x += off;
        }
    }
    out.height = float(out.lines.size()) * lh;
}

// ---- Glyph atlas ---------------------------------------------------------------------------------------------------
GlyphAtlas::GlyphAtlas(int size)
    : m_size(size & ~1), m_pixels(std::size_t(size & ~1) * std::size_t(size & ~1) * 2, 0), m_slots(4096) {}

void GlyphAtlas::clear() {
    std::fill(m_pixels.begin(), m_pixels.end(), std::uint8_t(0));
    for (auto& s : m_slots) s = Slot{};
    m_used = 0;
    m_shelf = {};
    m_colorShelf = {};
    ++m_generation;
}

bool GlyphAtlas::find(std::uint64_t key, std::uint32_t aux, std::size_t& slot) const {
    const std::size_t mask = m_slots.size() - 1;
    for (std::size_t h = std::size_t(((key ^ aux) * 0x9E3779B97F4A7C15ull) >> 40) & mask;; h = (h + 1) & mask) {
        const Slot& s = m_slots[h];
        slot = h;
        if (s.key == key && s.aux == aux) return true;
        if (s.key == 0) return false;
    }
}

// Shelf packing with a 1-texel gutter. False when the page is full (the caller resets the atlas).
bool GlyphAtlas::place(Shelf& s, int page, int w, int h, int& u, int& v) {
    if (w + 2 > page || h + 2 > page) return false;
    if (s.x + w + 1 > page) {
        s.x = 1;
        s.y += s.h + 1;
        s.h = 0;
    }
    if (s.y + h + 1 > page) return false;
    u = s.x;
    v = s.y;
    s.x += w + 1;
    s.h = std::max(s.h, h);
    return true;
}

AtlasGlyph GlyphAtlas::store(std::uint64_t key, std::uint32_t aux, const AtlasGlyph& g) {
    if (m_used * 4 >= m_slots.size() * 3) clear();
    std::size_t h = 0;
    find(key, aux, h);
    m_slots[h] = {key, aux, g};
    ++m_used;
    return g;
}

AtlasGlyph GlyphAtlas::get(const Font& font, std::uint32_t glyph, float px) {
    const std::uint32_t q = std::uint32_t(std::lround(px * 4.f));
    const std::uint64_t key = (std::uint64_t(font.id()) << 44) ^ (std::uint64_t(q) << 24) ^ glyph ^ (1ull << 63);
    std::size_t slot = 0;
    if (find(key, 0, slot)) return m_slots[slot].g;
    ++m_misses;
    if (m_used * 4 >= m_slots.size() * 3) clear();
    const float pxq = float(q) / 4.f;
    int x0, y0, x1, y1;
    font.glyphBox(glyph, pxq, x0, y0, x1, y1);
    const int w = x1 - x0, hh = y1 - y0;
    AtlasGlyph ag;
    ag.x0 = std::int16_t(x0);
    ag.y0 = std::int16_t(y0);
    if (w > 0 && hh > 0) {
        int u = 0, v = 0;
        if (!place(m_shelf, m_size, w, hh, u, v)) {
            if (w + 2 > m_size || hh + 2 > m_size) return {};
            clear();
            place(m_shelf, m_size, w, hh, u, v);
        }
        font.rasterise(glyph, pxq, m_pixels.data() + std::size_t(v) * std::size_t(m_size) + std::size_t(u), w, hh, m_size);
        ag.u = std::uint16_t(u);
        ag.v = std::uint16_t(v);
        ag.w = std::uint16_t(w);
        ag.h = std::uint16_t(hh);
    }
    ag.valid = true;
    return store(key, 0, ag);
}

AtlasGlyph GlyphAtlas::getColor(const Font& font, std::uint32_t glyph, float px, std::uint32_t foreground) {
    const std::uint32_t q = std::uint32_t(std::lround(px * 4.f));
    const std::uint64_t key = ((std::uint64_t(font.id()) << 44) ^ (std::uint64_t(q) << 24) ^ glyph ^ (1ull << 63)) | (1ull << 16);
    // bitmap strikes ignore the text colour: one entry for every colour
    const auto* st = detail::FontAccess::state(font);
#if FUSE_TEXT_COOKED
    const std::uint32_t aux = st && detail::cookedForeground(st->cooked, glyph) ? (foreground | 1u) : 0u;
#else
    const std::uint32_t aux = st && findImage(st->bitmapImages, glyph) ? 0u : (foreground | 1u);
#endif
    std::size_t slot = 0;
    if (find(key, aux, slot)) return m_slots[slot].g;
    ++m_misses;
    thread_local ColorImage img;
    AtlasGlyph g;
    g.valid = true; // no image: remembered as an empty colour glyph
    g.kind = AtlasKind::Color;
    if (font.renderColor(glyph, float(q) / 4.f, foreground, img)) {
        g = insertColor(0, img.rgba.data(), img.w, img.h, img.w * 4, img.x0, img.y0);
        if (!g.valid) return g;
    }
    return store(key, aux, g);
}

AtlasGlyph GlyphAtlas::getMsdf(const Font& font, std::uint32_t glyph) {
    const std::uint64_t key = ((std::uint64_t(font.id()) << 44) ^ glyph ^ (1ull << 63)) | (2ull << 16);
    std::size_t slot = 0;
    if (find(key, 0, slot)) return m_slots[slot].g;
    ++m_misses;
    thread_local ColorImage img;
    AtlasGlyph g;
    g.valid = true; // no outline: an empty MSDF glyph
    g.kind = AtlasKind::Msdf;
    if (font.renderMsdf(glyph, kMsdfPixelSize, kMsdfRange, img)) {
        g = insertColor(0, img.rgba.data(), img.w, img.h, img.w * 4, img.x0, img.y0, AtlasKind::Msdf);
        if (!g.valid) return g;
    }
    return store(key, 0, g);
}

bool GlyphAtlas::lookup(std::uint64_t key, AtlasGlyph& out) const {
    std::size_t slot = 0;
    if (!find(key, 0, slot)) return false;
    out = m_slots[slot].g;
    return true;
}

AtlasGlyph GlyphAtlas::insert(std::uint64_t key, const std::uint8_t* src, int w, int hh, int stride, int x0, int y0) {
    if (AtlasGlyph g; lookup(key, g)) return g;
    ++m_misses;
    if (m_used * 4 >= m_slots.size() * 3) clear();
    AtlasGlyph ag;
    ag.x0 = std::int16_t(x0);
    ag.y0 = std::int16_t(y0);
    if (w > 0 && hh > 0) {
        int u = 0, v = 0;
        if (!place(m_shelf, m_size, w, hh, u, v)) {
            if (w + 2 > m_size || hh + 2 > m_size) return {};
            clear(); // full: start a new generation
            place(m_shelf, m_size, w, hh, u, v);
        }
        for (int y = 0; y < hh; ++y)
            std::memcpy(m_pixels.data() + std::size_t(v + y) * std::size_t(m_size) + std::size_t(u),
                        src + std::size_t(y) * std::size_t(stride), std::size_t(w));
        ag.u = std::uint16_t(u);
        ag.v = std::uint16_t(v);
        ag.w = std::uint16_t(w);
        ag.h = std::uint16_t(hh);
    }
    ag.valid = true;
    return store(key, 0, ag);
}

AtlasGlyph GlyphAtlas::insertColor(std::uint64_t key, const std::uint8_t* rgba, int w, int hh, int stride, int x0, int y0,
                                   AtlasKind kind) {
    if (key) {
        if (AtlasGlyph g; lookup(key, g)) return g;
        ++m_misses;
    }
    if (m_used * 4 >= m_slots.size() * 3) clear();
    AtlasGlyph ag;
    ag.x0 = std::int16_t(x0);
    ag.y0 = std::int16_t(y0);
    ag.kind = kind;
    const int cs = colorSize();
    if (w > 0 && hh > 0) {
        int u = 0, v = 0;
        if (!place(m_colorShelf, cs, w, hh, u, v)) {
            if (w + 2 > cs || hh + 2 > cs) return {};
            clear();
            place(m_colorShelf, cs, w, hh, u, v);
        }
        std::uint8_t* page = m_pixels.data() + std::size_t(m_size) * std::size_t(m_size);
        for (int y = 0; y < hh; ++y)
            std::memcpy(page + (std::size_t(v + y) * std::size_t(cs) + std::size_t(u)) * 4,
                        rgba + std::size_t(y) * std::size_t(stride), std::size_t(w) * 4);
        ag.u = std::uint16_t(u);
        ag.v = std::uint16_t(v);
        ag.w = std::uint16_t(w);
        ag.h = std::uint16_t(hh);
    }
    ag.valid = true;
    return key ? store(key, 0, ag) : ag;
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
