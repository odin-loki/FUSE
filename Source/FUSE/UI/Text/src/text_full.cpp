// FULL text profile (FUSE UI plan §4.8, decision D2): HarfBuzz shaping and FreeType rasterisation.
// Shaping (ligatures, GPOS kerning, marks, complex scripts), unhinted grayscale FreeType coverage, bidi (UAX #9 through
// SheenBidi: levels per byte, runs shaped per direction, L1 + L2 per line), UAX #14 line breaking (libunibreak) and a
// fixed-capacity shaped-run cache.
//
// Allocation: the run cache, its pools and the layout scratch live in FullFace and keep their capacity, so relaying
// out text already seen allocates nothing. SheenBidi (which mallocs its paragraph objects) only runs when the text
// holds a right-to-left code point (U+0590..U+08FF, U+FB1D..U+FDFF, U+FE70..U+FEFF, RLE/RLO/RLI controls), and its
// resolved levels are cached per paragraph text, so steady-state RTL layout does not reach it either.
#include "text_full.hpp"

#include <hb.h>
extern "C" {
#include <SheenBidi.h>
#include <linebreak.h>
}
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H

#include <cmath>
#include <algorithm>
#include <cstring>

namespace fuse::text::detail {

namespace {
FT_Library ftLibrary() {
    static FT_Library lib = [] {
        FT_Library l = nullptr;
        if (FT_Init_FreeType(&l) != 0) return FT_Library(nullptr);
        return l;
    }();
    return lib;
}
} // namespace

struct FullFace {
    hb_blob_t* blob = nullptr;
    hb_face_t* face = nullptr;
    hb_font_t* font = nullptr;
    hb_buffer_t* buffer = nullptr; // reused: no allocation once it has grown
    FT_Face ft = nullptr;
    FT_F26Dot6 ftSize = -1;        // last FT_Set_Char_Size (26.6)

    // ---- shaped-run cache: direct-mapped entries over two ring pools (glyphs in font units, key bytes) ----
    struct UnitGlyph {
        std::uint32_t glyph, cluster; // cluster relative to the run start
        std::int32_t xOffset, advance;
    };
    struct Entry {
        std::uint64_t hash = 0, glyphStart = 0, byteStart = 0;
        std::uint64_t var = 0; // varKey of the instance that shaped it
        std::uint32_t glyphCount = 0, len = 0;
        bool rtl = false, used = false;
    };
    // 4-way set associative, sized for a screen of text (one codeEdit frame of tokens plus ~40 wrapped paragraphs
    // relaid out without a miss). Allocated on first use, ~0.8 MB per font that shapes (fallbacks never used: none).
    static constexpr std::size_t kEntries = 4096, kWays = 4, kGlyphPool = 32768, kBytePool = 131072;
    struct RunCache {
        std::vector<Entry> entries;
        std::vector<UnitGlyph> glyphPool;
        std::vector<char> bytePool;
        std::uint64_t glyphWrite = 0, byteWrite = 0, hits = 0, misses = 0;
    };
    RunCache ownCache;
    RunCache* rc = &ownCache; // variation instances share their owner's cache (entries keyed by varKey)

    // ---- variation instance (fullCreateInstance): blob, face and FT_Face are the owner's ----
    FullFace* owner = nullptr;
    std::vector<FT_Fixed> coords;      // normalised coordinates (16.16) for FreeType; empty: default
    std::vector<int> hbCoords;         // the same as F2DOT14 for HarfBuzz
    std::uint64_t varKey = 0;          // 0: default coordinates and no features; else their hash (run cache key)
    std::uint64_t coordKey = 0, featureKey = 0;
    std::vector<hb_feature_t> features; // fullSetFeatures
    std::uint64_t ftActive = 0;        // owner: varKey whose coordinates the FT_Face holds

    // ---- layout scratch (capacity kept) ----
    std::vector<std::uint8_t> breaks, levels, paraLevels, glyphLevels;
    std::vector<ShapedGlyph> lineTmp;
    std::vector<std::uint8_t> levelTmp;

    // ---- resolved bidi levels of recent right-to-left paragraphs (direct-mapped; buffers keep their capacity) ----
    struct BidiEntry {
        std::uint64_t hash = 0;
        std::string text;
        std::vector<std::uint8_t> levels, paraLevels;
    };
    static constexpr std::size_t kBidiEntries = 64;
    std::vector<BidiEntry> bidiCache = std::vector<BidiEntry>(kBidiEntries);
};

FullFace* fullCreate(std::span<const std::uint8_t> bytes, std::uint32_t faceIndex) {
    auto* f = new FullFace;
    f->blob = hb_blob_create(reinterpret_cast<const char*>(bytes.data()), unsigned(bytes.size()),
                             HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    f->face = hb_face_create(f->blob, faceIndex);
    f->font = hb_font_create(f->face); // scale defaults to upem: positions come back in font units
    f->buffer = hb_buffer_create();
    FT_Library lib = ftLibrary();
    if (!lib || FT_New_Memory_Face(lib, bytes.data(), FT_Long(bytes.size()), FT_Long(faceIndex), &f->ft) != 0 ||
        !hb_buffer_allocation_successful(f->buffer)) {
        fullDestroy(f);
        return nullptr;
    }
    return f;
}

void fullCacheStats(const FullFace* f, std::uint64_t& hits, std::uint64_t& misses) {
    hits = f ? f->rc->hits : 0;
    misses = f ? f->rc->misses : 0;
}

void fullDestroy(FullFace* f) {
    if (!f) return;
    if (f->ft && !f->owner) FT_Done_Face(f->ft);
    hb_buffer_destroy(f->buffer);
    hb_font_destroy(f->font);
    hb_face_destroy(f->face);
    hb_blob_destroy(f->blob);
    delete f;
}

FullFace* fullCreateInstance(FullFace* owner) {
    if (!owner) return nullptr;
    auto* f = new FullFace;
    f->owner = owner;
    f->rc = owner->rc;
    f->blob = hb_blob_reference(owner->blob);
    f->face = hb_face_reference(owner->face);
    f->font = hb_font_create(f->face);
    f->buffer = hb_buffer_create();
    f->ft = owner->ft;
    return f;
}

void fullSetCoords(FullFace* f, std::span<const std::int16_t> norm) {
    if (!f) return;
    f->coords.assign(norm.size(), 0);
    f->hbCoords.assign(norm.size(), 0);
    std::uint64_t key = norm.empty() ? 0 : 0x9E3779B97F4A7C15ull;
    for (std::size_t i = 0; i < norm.size(); ++i) {
        f->coords[i] = FT_Fixed(norm[i]) * 4; // F2DOT14 -> 16.16
        f->hbCoords[i] = norm[i];
        key = (key ^ std::uint16_t(norm[i])) * 0x100000001b3ull;
    }
    if (norm.empty()) {
        f->coords.clear();
        f->hbCoords.clear();
    }
    f->coordKey = key;
    f->varKey = f->coordKey ^ f->featureKey;
    hb_font_set_var_coords_normalized(f->font, f->hbCoords.data(), unsigned(f->hbCoords.size()));
    FullFace* o = f->owner ? f->owner : f;
    o->ftActive = ~std::uint64_t(0); // re-apply on the next glyph load
}

void fullSetFeatures(FullFace* f, std::span<const FontFeature> features) {
    if (!f) return;
    f->features.clear();
    std::uint64_t key = features.empty() ? 0 : 0x2545F4914F6CDD1Dull;
    for (const FontFeature& ft : features) {
        f->features.push_back({ft.tag, ft.value, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END});
        key = ((key ^ ft.tag) * 0x100000001b3ull ^ ft.value) * 0x100000001b3ull;
    }
    f->featureKey = key;
    f->varKey = f->coordKey ^ f->featureKey;
}

namespace {
std::uint64_t hashRun(std::string_view s, bool rtl);
FullFace::Entry& cacheEntry(FullFace* f, std::string_view run, bool rtl, std::uint64_t h);
bool entryMatches(const FullFace* f, const FullFace::Entry& e, std::string_view s, bool rtl, std::uint64_t h);
} // namespace

float fullShape(FullFace* f, float unitScale, std::string_view s, std::vector<ShapedGlyph>& out) {
    out.clear();
    // Cached like layout runs, under a key of its own: the direction is guessed and glyphs stay in HarfBuzz order.
    const std::uint64_t h = (hashRun(s, false) * 0x2545F4914F6CDD1Dull + 1) ^ f->varKey;
    FullFace::Entry& e = cacheEntry(f, s, false, h);
    const bool fits = s.size() <= FullFace::kBytePool / 4;
    if (fits && entryMatches(f, e, s, false, h)) {
        ++f->rc->hits;
        float pen = 0.f;
        for (std::uint32_t k = 0; k < e.glyphCount; ++k) {
            const auto& u = f->rc->glyphPool[(e.glyphStart + k) % FullFace::kGlyphPool];
            const float adv = float(u.advance) * unitScale;
            out.push_back({u.glyph, u.cluster, pen + float(u.xOffset) * unitScale, adv});
            pen += adv;
        }
        return pen;
    }
    ++f->rc->misses;
    hb_buffer_clear_contents(f->buffer);
    hb_buffer_add_utf8(f->buffer, s.data(), int(s.size()), 0, int(s.size()));
    hb_buffer_guess_segment_properties(f->buffer);
    hb_shape(f->font, f->buffer, f->features.data(), unsigned(f->features.size()));
    unsigned n = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(f->buffer, &n);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(f->buffer, &n);
    float pen = 0.f;
    for (unsigned i = 0; i < n; ++i) {
        const float adv = float(pos[i].x_advance) * unitScale;
        out.push_back({info[i].codepoint, info[i].cluster, pen + float(pos[i].x_offset) * unitScale, adv});
        pen += adv;
    }
    if (fits && n <= FullFace::kGlyphPool / 4) {
        e = {};
        e.hash = h;
        e.len = std::uint32_t(s.size());
        e.used = true;
        e.var = f->varKey;
        e.glyphStart = f->rc->glyphWrite;
        e.glyphCount = n;
        e.byteStart = f->rc->byteWrite;
        for (unsigned i = 0; i < n; ++i)
            f->rc->glyphPool[(f->rc->glyphWrite++) % FullFace::kGlyphPool] = {info[i].codepoint, info[i].cluster, pos[i].x_offset,
                                                                        pos[i].x_advance};
        for (char c : s) f->rc->bytePool[(f->rc->byteWrite++) % FullFace::kBytePool] = c;
    }
    return pen;
}

namespace {

std::uint64_t hashRun(std::string_view s, bool rtl) {
    std::uint64_t h = rtl ? 0x9E3779B97F4A7C15ull : 0xcbf29ce484222325ull;
    for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ull;
    return h;
}

bool entryValid(const FullFace* f, const FullFace::Entry& e) {
    return e.used && f->rc->glyphWrite - e.glyphStart <= FullFace::kGlyphPool &&
           f->rc->byteWrite - e.byteStart <= FullFace::kBytePool;
}

bool entryMatches(const FullFace* f, const FullFace::Entry& e, std::string_view s, bool rtl, std::uint64_t h) {
    if (!entryValid(f, e) || e.hash != h || e.len != s.size() || e.rtl != rtl || e.var != f->varKey) return false;
    const std::size_t at = std::size_t(e.byteStart % FullFace::kBytePool);
    const std::size_t head = std::min(s.size(), FullFace::kBytePool - at); // the run may wrap around the ring
    return std::memcmp(f->rc->bytePool.data() + at, s.data(), head) == 0 &&
           std::memcmp(f->rc->bytePool.data(), s.data() + head, s.size() - head) == 0;
}

/// The cache slot for a run: its entry when cached, else the way of its set to replace (an empty or stale one, or
/// the oldest). Allocates the cache on first use.
FullFace::Entry& cacheEntry(FullFace* f, std::string_view run, bool rtl, std::uint64_t h) {
    if (f->rc->entries.empty()) {
        f->rc->entries.resize(FullFace::kEntries);
        f->rc->glyphPool.resize(FullFace::kGlyphPool);
        f->rc->bytePool.resize(FullFace::kBytePool);
    }
    FullFace::Entry* set = f->rc->entries.data() + std::size_t(h % (FullFace::kEntries / FullFace::kWays)) * FullFace::kWays;
    FullFace::Entry* victim = set;
    for (std::size_t w = 0; w < FullFace::kWays; ++w) {
        FullFace::Entry& e = set[w];
        if (entryMatches(f, e, run, rtl, h)) return e;
        if (!entryValid(f, *victim)) continue;
        if (!entryValid(f, e) || e.glyphStart < victim->glyphStart) victim = &e;
    }
    return *victim;
}

/// Shapes bytes [begin, begin+len) of `s` (the whole paragraph is context) in one direction; appends glyphs in logical
/// order (clusters ascending) with absolute byte clusters, x = 0 and advance in pixels. Cached by (bytes, direction).
void shapeRun(FullFace* f, float unitScale, std::string_view s, std::uint32_t begin, std::uint32_t len, bool rtl,
              std::vector<ShapedGlyph>& out) {
    const std::string_view run = s.substr(begin, len);
    const std::uint64_t h = hashRun(run, rtl) ^ f->varKey;
    FullFace::Entry& e = cacheEntry(f, run, rtl, h);
    if (!entryMatches(f, e, run, rtl, h)) {
        ++f->rc->misses;
        hb_buffer_clear_contents(f->buffer);
        // The run alone: the cache key is the run's bytes, so shaping must not depend on the surrounding context.
        hb_buffer_add_utf8(f->buffer, run.data(), int(run.size()), 0, int(run.size()));
        hb_buffer_guess_segment_properties(f->buffer);
        hb_buffer_set_direction(f->buffer, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_shape(f->font, f->buffer, f->features.data(), unsigned(f->features.size()));
        unsigned n = 0;
        const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(f->buffer, &n);
        const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(f->buffer, &n);
        const bool fits = n <= FullFace::kGlyphPool / 4 && run.size() <= FullFace::kBytePool / 4;
        if (!fits) { // too large to cache: emit directly
            for (unsigned k = 0; k < n; ++k) {
                const unsigned i = rtl ? n - 1 - k : k;
                out.push_back({info[i].codepoint, begin + info[i].cluster, float(pos[i].x_offset) * unitScale,
                               float(pos[i].x_advance) * unitScale});
            }
            return;
        }
        e = {};
        e.hash = h;
        e.len = std::uint32_t(run.size());
        e.rtl = rtl;
        e.used = true;
        e.var = f->varKey;
        e.glyphStart = f->rc->glyphWrite;
        e.glyphCount = n;
        e.byteStart = f->rc->byteWrite;
        for (unsigned k = 0; k < n; ++k) { // logical order: HarfBuzz returns RTL runs in visual (reversed) order
            const unsigned i = rtl ? n - 1 - k : k;
            f->rc->glyphPool[(f->rc->glyphWrite++) % FullFace::kGlyphPool] = {info[i].codepoint, info[i].cluster,
                                                                        pos[i].x_offset, pos[i].x_advance};
        }
        for (char c : run) f->rc->bytePool[(f->rc->byteWrite++) % FullFace::kBytePool] = c;
    } else {
        ++f->rc->hits;
    }
    for (std::uint32_t k = 0; k < e.glyphCount; ++k) {
        const auto& u = f->rc->glyphPool[(e.glyphStart + k) % FullFace::kGlyphPool];
        out.push_back({u.glyph, begin + u.cluster, float(u.xOffset) * unitScale, float(u.advance) * unitScale});
    }
}

bool hasRtl(std::string_view s) {
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0xD6 && c <= 0xDF) return true; // U+0580..U+07FF (Hebrew, Arabic, Syriac, Thaana, NKo); U+0580 harmless
        if (c == 0xE0 && i + 1 < s.size()) {
            const unsigned char d = static_cast<unsigned char>(s[i + 1]);
            if (d >= 0xA0 && d <= 0xA3) return true; // U+0800..U+08FF
        }
        if (c == 0xEF && i + 1 < s.size()) {
            const unsigned char d = static_cast<unsigned char>(s[i + 1]);
            if (d >= 0xAC && d <= 0xBB) return true; // U+FB00..U+FEFF (presentation forms; U+FB00..1C harmless)
        }
        if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80) {
            const unsigned char d = static_cast<unsigned char>(s[i + 2]);
            if (d == 0x8F || d == 0xAB || d == 0xAE) return true; // RLM, RLE, RLO
        }
        if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x81 &&
            static_cast<unsigned char>(s[i + 2]) == 0xA7)
            return true; // RLI
    }
    return false;
}

bool isSpaceByte(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Characters rule L1 resets when they precede a separator or end the line: whitespace, isolate controls and the
// characters X9 removes (embedding controls, BN).
bool l1Sequence(SBBidiType t) {
    return t == SBBidiTypeWS || t == SBBidiTypeBN || (t >= SBBidiTypeLRI && t <= SBBidiTypePDF);
}

// Rule L1 over bytes [b, e) of one line: segment and paragraph separators, and the whitespace sequences before them
// or at the end of the line, take the paragraph level.
void applyL1(std::string_view s, std::size_t b, std::size_t e, std::uint8_t para, std::uint8_t* lv) {
    constexpr std::size_t kNone = ~std::size_t(0);
    std::size_t run = kNone;
    for (std::size_t i = b; i < e;) {
        const std::size_t at = i;
        const SBBidiType t = SBCodepointGetBidiType(SBCodepoint(decodeUtf8(s, i)));
        if (t == SBBidiTypeS || t == SBBidiTypeB) {
            std::fill(lv + (run != kNone ? run : at), lv + i, para);
            run = kNone;
        } else if (l1Sequence(t)) {
            if (run == kNone) run = at;
        } else {
            run = kNone;
        }
    }
    if (run != kNone) std::fill(lv + run, lv + e, para);
}

} // namespace

void fullLayout(std::span<const FullRunFace> faces, std::span<const FullFaceSpan> spans, std::string_view s,
                float maxWidth, float lineHeight, float ascent, std::vector<ShapedGlyph>& g, std::vector<TextLine>& lines,
                int base) {
    FullFace* const f = faces[0].face; // the layout font: its scratch and bidi cache
    g.clear();
    lines.clear();
    const std::size_t n = s.size();
    static const bool lbInit = [] { init_linebreak(); return true; }();
    (void)lbInit;
    f->breaks.resize(n);
    if (n) set_linebreaks_utf8(reinterpret_cast<const utf8_t*>(s.data()), n, nullptr,
                               reinterpret_cast<char*>(f->breaks.data()));
    f->levels.assign(n, 0);
    f->paraLevels.assign(n, 0);
    // a given RTL base direction needs the bidi pass for any text (neutrals at the ends take the paragraph's level)
    const bool bidi = n && (base == 1 || hasRtl(s));
    FullFace::BidiEntry* be = nullptr;
    bool cached = false;
    std::uint64_t h = 0;
    if (bidi) {
        h = hashRun(s, false) ^ (0x9E3779B97F4A7C15ull * std::uint64_t(base + 1)); // keyed on the base direction too
        be = &f->bidiCache[h % FullFace::kBidiEntries];
        if (be->hash == h && be->text == s) { // seen before: no SheenBidi run
            f->levels.assign(be->levels.begin(), be->levels.end());
            f->paraLevels.assign(be->paraLevels.begin(), be->paraLevels.end());
            cached = true;
        }
    }
    if (bidi && !cached) {
        be->hash = h;
        const SBLevel want = base == 0 ? 0 : base == 1 ? 1 : SBLevelDefaultLTR;
        SBCodepointSequence seq{SBStringEncodingUTF8, const_cast<char*>(s.data()), SBUInteger(n)};
        SBAlgorithmRef alg = SBAlgorithmCreate(&seq);
        SBUInteger off = 0;
        while (alg && off < n) {
            SBParagraphRef para = SBAlgorithmCreateParagraph(alg, off, SBUInteger(n - off), want);
            if (!para) break;
            const SBUInteger plen = SBParagraphGetLength(para);
            const SBLevel* lv = SBParagraphGetLevelsPtr(para);
            const SBLevel base = SBParagraphGetBaseLevel(para);
            for (SBUInteger k = 0; k < plen; ++k) {
                f->levels[off + k] = lv[k];
                f->paraLevels[off + k] = base;
            }
            SBParagraphRelease(para);
            if (plen == 0) break;
            off += plen;
        }
        if (alg) SBAlgorithmRelease(alg);
        be->text.assign(s);
        be->levels.assign(f->levels.begin(), f->levels.end());
        be->paraLevels.assign(f->paraLevels.begin(), f->paraLevels.end());
    }

    // Shape level runs (never across a newline) in logical order, split where the face changes: every piece is
    // shaped once, by the face that draws it, so line breaking sees the real advances.
    f->glyphLevels.clear();
    std::uint32_t i = 0;
    std::size_t sp = 0;
    while (i < n) {
        std::uint32_t j = i + 1;
        if (s[i] != '\n')
            while (j < n && s[j] != '\n' && f->levels[j] == f->levels[i]) ++j;
        for (std::uint32_t a = i; a < j;) {
            while (sp < spans.size() && spans[sp].e <= a) ++sp;
            std::uint16_t face = 0;
            std::uint32_t b = j;
            if (sp < spans.size() && spans[sp].b <= a) {
                face = spans[sp].face;
                b = std::min(j, spans[sp].e);
            } else if (sp < spans.size()) {
                b = std::min(j, spans[sp].b);
            }
            const FullRunFace& rf = faces[face];
            const std::size_t before = g.size();
            shapeRun(rf.face, rf.unitScale, s, a, b - a, (f->levels[i] & 1) != 0, g);
            for (std::size_t k = before; k < g.size(); ++k) {
                g[k].face = face;
                if (rf.bold > 0.f && g[k].advance > 0.f) g[k].advance += rf.bold;
            }
            a = b;
        }
        f->glyphLevels.resize(g.size(), f->levels[i]);
        i = j;
    }

    // Break lines over the logical glyph sequence.
    const std::uint32_t gn = std::uint32_t(g.size());
    const bool wrap = maxWidth > 0.f;
    auto canBreakBefore = [&](std::uint32_t k) {
        if (k == 0 || k >= gn || g[k].cluster == g[k - 1].cluster) return false;
        const std::uint32_t c = g[k].cluster;
        return c > 0 && f->breaks[c - 1] == LINEBREAK_ALLOWBREAK;
    };
    auto emit = [&](std::uint32_t b, std::uint32_t e) {
        TextLine ln;
        ln.firstGlyph = b;
        ln.glyphCount = e - b;
        std::uint32_t lo = std::uint32_t(n), hi = 0;
        for (std::uint32_t k = b; k < e; ++k) {
            lo = std::min(lo, g[k].cluster);
            hi = std::max(hi, g[k].cluster);
        }
        ln.byteBegin = b < e ? lo : (b < gn ? g[b].cluster : std::uint32_t(n));
        ln.byteEnd = e < gn ? g[e].cluster : std::uint32_t(n);
        if (b < e && hi >= ln.byteEnd) ln.byteEnd = std::uint32_t(n);
        const std::uint8_t para = b < e ? f->paraLevels[g[b].cluster] : 0;
        ln.rtl = (para & 1) != 0;
        if (bidi && b < e) { // L1 over the line's bytes, then glyph levels from their clusters
            applyL1(s, ln.byteBegin, ln.byteEnd, para, f->levels.data());
            for (std::uint32_t k = b; k < e; ++k) f->glyphLevels[k] = f->levels[g[k].cluster];
        }
        // Trailing whitespace (paragraph level after L1) does not count toward the width.
        std::uint32_t last = e;
        float trailing = 0.f;
        while (last > b && isSpaceByte(s[g[last - 1].cluster])) {
            --last;
            f->glyphLevels[last] = para;
            trailing += g[last].advance;
        }
        // L2: reverse every maximal sequence at or above each level, from the highest down to the lowest odd one.
        std::uint8_t hiLv = 0, loOdd = 0xff;
        for (std::uint32_t k = b; k < e; ++k) {
            hiLv = std::max(hiLv, f->glyphLevels[k]);
            if (f->glyphLevels[k] & 1) loOdd = std::min(loOdd, f->glyphLevels[k]);
        }
        for (int lv = hiLv; loOdd != 0xff && lv >= loOdd; --lv) {
            std::uint32_t k = b;
            while (k < e) {
                if (f->glyphLevels[k] < lv) { ++k; continue; }
                std::uint32_t m = k;
                while (m < e && f->glyphLevels[m] >= lv) ++m;
                std::reverse(g.begin() + k, g.begin() + m);
                std::reverse(f->glyphLevels.begin() + k, f->glyphLevels.begin() + m);
                k = m;
            }
        }
        // Pen positions in visual order; trailing whitespace of an RTL line hangs off the left edge.
        float pen = ln.rtl ? -trailing : 0.f;
        for (std::uint32_t k = b; k < e; ++k) {
            const float adv = g[k].advance;
            g[k].x += pen; // x held the glyph offset until now
            pen += adv;
        }
        float w = 0.f;
        for (std::uint32_t k = b; k < e; ++k) w += g[k].advance;
        ln.width = w - trailing;
        ln.baseline = float(lines.size()) * lineHeight + std::ceil(ascent);
        lines.push_back(ln);
    };
    std::uint32_t start = 0, lastBreak = 0;
    float lineW = 0.f;
    for (std::uint32_t k = 0; k < gn; ++k) {
        const char c = s[g[k].cluster];
        if (canBreakBefore(k)) lastBreak = k;
        if (c == '\n') {
            emit(start, k + 1);
            start = k + 1;
            lastBreak = 0;
            lineW = 0.f;
            continue;
        }
        if (wrap && !isSpaceByte(c) && k > start && lineW + g[k].advance > maxWidth) {
            const std::uint32_t at = lastBreak > start ? lastBreak : k;
            emit(start, at);
            start = at;
            lastBreak = 0;
            lineW = 0.f;
            k = at - 1; // re-measure from the break
            continue;
        }
        lineW += g[k].advance;
    }
    if (start < gn || lines.empty() || (gn > 0 && s[g[gn - 1].cluster] == '\n')) emit(start, gn);
    for (std::uint32_t k = 0; k < gn; ++k) if (s[g[k].cluster] == '\n' || s[g[k].cluster] == '\r') g[k].glyph = kInvisibleGlyph;
}

void fullLineBreaks(std::string_view s, std::vector<LineBreak>& out, bool strict) {
    static const bool lbInit = [] { init_linebreak(); return true; }();
    (void)lbInit;
    out.resize(s.size());
    if (s.empty()) return;
    // LINEBREAK_MUSTBREAK / ALLOWBREAK / NOBREAK / INSIDEACHAR are LineBreak's values in order; a "-strict" language
    // suffix resolves CJ to NS.
    set_linebreaks_utf8(reinterpret_cast<const utf8_t*>(s.data()), s.size(), strict ? "-strict" : nullptr,
                        reinterpret_cast<char*>(out.data()));
    out.back() = LineBreak::Must; // LB3 (libunibreak reports "indeterminate" for the end of the input)
}

void fullBidiLevels(std::string_view s, int base, std::vector<std::uint8_t>& levels, std::uint8_t& paraLevel) {
    const std::size_t n = s.size();
    paraLevel = base == 1 ? 1 : 0;
    levels.assign(n, paraLevel);
    if (!n) return;
    SBCodepointSequence seq{SBStringEncodingUTF8, const_cast<char*>(s.data()), SBUInteger(n)};
    SBAlgorithmRef alg = SBAlgorithmCreate(&seq);
    SBUInteger off = 0;
    const SBLevel want = base == 0 ? 0 : base == 1 ? 1 : SBLevelDefaultLTR;
    while (alg && off < n) {
        SBParagraphRef para = SBAlgorithmCreateParagraph(alg, off, SBUInteger(n - off), want);
        if (!para) break;
        const SBUInteger plen = SBParagraphGetLength(para);
        const SBLevel* lv = SBParagraphGetLevelsPtr(para);
        paraLevel = SBParagraphGetBaseLevel(para);
        std::copy(lv, lv + plen, levels.begin() + std::ptrdiff_t(off));
        applyL1(s, off, off + plen, paraLevel, levels.data());
        SBParagraphRelease(para);
        if (plen == 0) break;
        off += plen;
    }
    if (alg) SBAlgorithmRelease(alg);
}

namespace {
bool loadRendered(FullFace* f, std::uint32_t glyph, float emPx) {
    FullFace* o = f->owner ? f->owner : f; // instances render through the owner's FT_Face at their coordinates
    if (o->ftActive != f->varKey) {
        FT_Set_Var_Blend_Coordinates(o->ft, FT_UInt(f->coords.size()), f->coords.empty() ? nullptr : f->coords.data());
        o->ftActive = f->varKey;
        o->ftSize = -1;
    }
    const FT_F26Dot6 size = FT_F26Dot6(std::lround(emPx * 64.f));
    if (size != o->ftSize) {
        if (FT_Set_Char_Size(o->ft, 0, size, 72, 72) != 0) return false;
        o->ftSize = size;
    }
    if (FT_Load_Glyph(o->ft, glyph, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) return false;
    return FT_Render_Glyph(f->ft->glyph, FT_RENDER_MODE_NORMAL) == 0;
}
} // namespace

void fullGlyphBox(FullFace* f, std::uint32_t glyph, float emPx, int& x0, int& y0, int& x1, int& y1) {
    x0 = y0 = x1 = y1 = 0;
    if (!loadRendered(f, glyph, emPx)) return;
    const FT_GlyphSlot g = f->ft->glyph;
    x0 = g->bitmap_left;
    y0 = -g->bitmap_top;
    x1 = x0 + int(g->bitmap.width);
    y1 = y0 + int(g->bitmap.rows);
}

void fullRasterise(FullFace* f, std::uint32_t glyph, float emPx, std::uint8_t* dst, int w, int h, int stride) {
    if (!loadRendered(f, glyph, emPx)) return;
    const FT_Bitmap& b = f->ft->glyph->bitmap;
    const int rows = std::min(h, int(b.rows)), cols = std::min(w, int(b.width));
    for (int y = 0; y < rows; ++y) std::memcpy(dst + y * stride, b.buffer + y * b.pitch, std::size_t(cols));
}

} // namespace fuse::text::detail
