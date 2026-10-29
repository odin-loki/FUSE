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
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace fuse::text {

TextProfile textProfile() { return TextProfile::Minimal; }
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

std::size_t prevBoundary(std::string_view s, std::size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0u) == 0x80u) --i;
    return i;
}
std::size_t nextBoundary(std::string_view s, std::size_t i) {
    if (i >= s.size()) return s.size();
    decodeUtf8(s, i);
    return i;
}

// ---- Font ----------------------------------------------------------------------------------------------------------
struct Font::State {
    std::vector<std::uint8_t> bytes;
    stbtt_fontinfo info{};
    float unitsPerPixelScale1 = 0.f; // stbtt_ScaleForPixelHeight(info, 1)
};

namespace {
std::atomic<std::uint32_t> g_fontIds{1};
}

std::expected<Font, TextError> Font::fromBytes(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12) return std::unexpected(TextError::InvalidFont);
    auto* st = new State;
    st->bytes.assign(bytes.begin(), bytes.end());
    const int offset = stbtt_GetFontOffsetForIndex(st->bytes.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&st->info, st->bytes.data(), offset)) {
        delete st;
        return std::unexpected(TextError::InvalidFont);
    }
    st->unitsPerPixelScale1 = stbtt_ScaleForPixelHeight(&st->info, 1.f);
    Font f;
    f.m_state = st;
    f.m_id = g_fontIds.fetch_add(1);
    return f;
}

std::expected<Font, TextError> Font::fromFile(const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return std::unexpected(TextError::InvalidFont);
    std::vector<std::uint8_t> data;
    std::uint8_t buf[16384];
    for (;;) {
        const std::size_t n = std::fread(buf, 1, sizeof buf, fp);
        data.insert(data.end(), buf, buf + n);
        if (n < sizeof buf) break;
    }
    std::fclose(fp);
    return fromBytes(data);
}

std::expected<Font, TextError> Font::bundled() {
#if defined(FUSE_TEXT_BUNDLED_FONT)
    return fromFile(FUSE_TEXT_BUNDLED_FONT);
#else
    return std::unexpected(TextError::InvalidFont);
#endif
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
    const float s = m_state->unitsPerPixelScale1 * px;
    stbtt_GetGlyphBitmapBox(&m_state->info, int(glyph), s, s, &x0, &y0, &x1, &y1);
}
void Font::rasterise(std::uint32_t glyph, float px, std::uint8_t* dst, int w, int h, int stride) const {
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

const ITextShaper& defaultShaper() {
    static const MinimalShaper shaper;
    return shaper;
}

float measureText(const Font& font, std::string_view s, float px, std::vector<ShapedGlyph>& scratch) {
    return defaultShaper().shape(font, px, s, scratch);
}

void layoutText(const Font& font, const ITextShaper& shaper, std::string_view s, const TextLayoutDesc& desc,
                TextLayout& out) {
    out.lines.clear();
    out.metrics = font.metrics(desc.pixelSize);
    shaper.shape(font, desc.pixelSize, s, out.glyphs);
    const float lh = std::ceil(out.metrics.lineHeight());
    const bool wrap = desc.maxWidth > 0.f && !desc.elide;
    auto& g = out.glyphs;
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

    if (desc.elide && desc.maxWidth > 0.f && !out.lines.empty() && out.lines[0].width > desc.maxWidth) {
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
    if (desc.align != TextAlign::Start) {
        for (const auto& ln : out.lines) {
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
