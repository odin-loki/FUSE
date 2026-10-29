// fuse_text — fonts, shaping, line breaking, glyph cache and text editing (FUSE UI plan §4.8, phase UI3).
//
// Profiles (FUSE_UI_TEXT, decision D2 adopted 2026-10-29): FULL (HarfBuzz + FreeType + bidi + UAX #14/#29),
// MINIMAL (cmap + kerning shaping, stb_truetype grayscale rasterisation; Latin / Greek / Cyrillic / CJK, no complex
// scripts or bidi), COOKED (pre-baked .fusefont only). This build implements MINIMAL. FULL and COOKED select the same
// interfaces (ITextShaper, GlyphAtlas) and are honest stubs until vendored: textProfile() reports what is compiled.
//
// Engine rules: no exceptions (std::expected), no owning raw pointers, no allocation on the steady-state path: shaping
// and layout write into caller-owned buffers that keep their capacity, and the glyph atlas only allocates on a miss.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::text {

enum class TextProfile : std::uint8_t { Minimal, Full, Cooked };
/// Profile compiled into this build (MINIMAL until FULL dependencies are vendored).
TextProfile textProfile();
const char* textProfileName(TextProfile p);

enum class TextError : std::uint8_t { InvalidFont, NoGlyphs, AtlasFull, InvalidUtf8 };
const char* textErrorName(TextError e);

// ---- UTF-8 ---------------------------------------------------------------------------------------------------------
/// Decodes one code point at `i` and advances it; invalid bytes decode as U+FFFD and advance one byte.
char32_t decodeUtf8(std::string_view s, std::size_t& i);
/// Appends the UTF-8 encoding of `cp` to `out`.
void appendUtf8(std::string& out, char32_t cp);
/// Byte offset of the previous / next code point boundary (grapheme approximation for the MINIMAL profile).
std::size_t prevBoundary(std::string_view s, std::size_t i);
std::size_t nextBoundary(std::string_view s, std::size_t i);

// ---- Font ----------------------------------------------------------------------------------------------------------
struct FontMetrics {
    float ascent = 0.f, descent = 0.f, lineGap = 0.f; ///< pixels at the size asked for (descent is negative)
    float lineHeight() const { return ascent - descent + lineGap; }
};

/// A TrueType / OpenType (glyf) font. Holds a copy of the file bytes; the parsed state points into it.
class Font {
public:
    static std::expected<Font, TextError> fromBytes(std::span<const std::uint8_t> bytes);
    static std::expected<Font, TextError> fromFile(const std::string& path);
    /// FUSE's bundled UI font (vendor/fonts/opensans) when the build knows its path; InvalidFont otherwise.
    static std::expected<Font, TextError> bundled();

    Font() = default;
    Font(Font&&) noexcept;
    Font& operator=(Font&&) noexcept;
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    ~Font();

    bool valid() const { return m_state != nullptr; }
    FontMetrics metrics(float pixelSize) const;
    std::uint32_t glyphIndex(char32_t cp) const;
    /// Horizontal advance and kerning in pixels.
    float advance(std::uint32_t glyph, float pixelSize) const;
    float kerning(std::uint32_t left, std::uint32_t right, float pixelSize) const;
    /// Bitmap box of a glyph at `pixelSize` (x0,y0 relative to the pen on the baseline; y down).
    void glyphBox(std::uint32_t glyph, float pixelSize, int& x0, int& y0, int& x1, int& y1) const;
    /// Rasterises 8-bit coverage into `dst` (row stride `stride`) of size w*h.
    void rasterise(std::uint32_t glyph, float pixelSize, std::uint8_t* dst, int w, int h, int stride) const;
    /// Stable id for caches (unique per loaded font in this process).
    std::uint32_t id() const { return m_id; }

private:
    struct State;
    State* m_state = nullptr; // owned (pimpl); released in ~Font
    std::uint32_t m_id = 0;
};

// ---- Shaping -------------------------------------------------------------------------------------------------------
struct ShapedGlyph {
    std::uint32_t glyph = 0;
    std::uint32_t cluster = 0; ///< byte offset of the source code point
    float x = 0.f;             ///< pen position (pixels) relative to the run start
    float advance = 0.f;
};

/// Shaper interface (FULL: HarfBuzz; MINIMAL: cmap + kerning).
class ITextShaper {
public:
    virtual ~ITextShaper() = default;
    /// Shapes `utf8` with `font` at `pixelSize`; clears and fills `out` (keeps its capacity). Returns the run width.
    virtual float shape(const Font& font, float pixelSize, std::string_view utf8, std::vector<ShapedGlyph>& out) const = 0;
};

class MinimalShaper final : public ITextShaper {
public:
    float shape(const Font& font, float pixelSize, std::string_view utf8, std::vector<ShapedGlyph>& out) const override;
};

/// The shaper of the compiled profile.
const ITextShaper& defaultShaper();

// ---- Line breaking and layout --------------------------------------------------------------------------------------
struct TextLine {
    std::uint32_t firstGlyph = 0, glyphCount = 0;
    std::uint32_t byteBegin = 0, byteEnd = 0;
    float width = 0.f;
    float baseline = 0.f; ///< y of the baseline from the top of the block
};

enum class TextAlign : std::uint8_t { Start, Center, End };

struct TextLayoutDesc {
    float pixelSize = 14.f;
    float maxWidth = 0.f; ///< <= 0: no wrapping
    TextAlign align = TextAlign::Start;
    bool elide = false; ///< single line clipped to maxWidth with an ellipsis
};

/// A shaped, line-broken block. Reuse one object per text node: relayout keeps every buffer's capacity.
struct TextLayout {
    std::vector<ShapedGlyph> glyphs; ///< x already includes line alignment; y comes from the line
    std::vector<TextLine> lines;
    float width = 0.f, height = 0.f;
    FontMetrics metrics;
};

/// Shapes and breaks `utf8` (break opportunities after spaces and at hard newlines, UAX #14 subset; words longer than
/// the line break at a glyph). Clears and fills `out`.
void layoutText(const Font& font, const ITextShaper& shaper, std::string_view utf8, const TextLayoutDesc& desc,
                TextLayout& out);
/// Width of a single line without wrapping (no allocation when `scratch` has capacity).
float measureText(const Font& font, std::string_view utf8, float pixelSize, std::vector<ShapedGlyph>& scratch);

// ---- Glyph atlas ---------------------------------------------------------------------------------------------------
struct AtlasGlyph {
    std::uint16_t u = 0, v = 0, w = 0, h = 0; ///< texels in the atlas page
    std::int16_t x0 = 0, y0 = 0;              ///< offset from the pen on the baseline
    bool valid = false;
};

/// One 8-bit coverage page with shelf packing and an open-addressed lookup (no allocation on hit).
/// When full, the page is reset and the generation bumps (callers re-request their glyphs: LRU by page).
class GlyphAtlas {
public:
    explicit GlyphAtlas(int size = 1024);
    /// Finds or rasterises `glyph` of `font` at `pixelSize` (sizes are bucketed to 1/4 px).
    AtlasGlyph get(const Font& font, std::uint32_t glyph, float pixelSize);
    int size() const { return m_size; }
    const std::uint8_t* pixels() const { return m_pixels.data(); }
    std::uint32_t generation() const { return m_generation; }
    std::uint32_t misses() const { return m_misses; }
    void clear();

private:
    struct Slot {
        std::uint64_t key = 0;
        AtlasGlyph g;
    };
    int m_size;
    std::vector<std::uint8_t> m_pixels;
    std::vector<Slot> m_slots;
    std::uint32_t m_used = 0;
    int m_shelfX = 1, m_shelfY = 1, m_shelfH = 0;
    std::uint32_t m_generation = 0;
    std::uint32_t m_misses = 0;
};

// ---- Editing -------------------------------------------------------------------------------------------------------
/// Single-line / multi-line edit buffer: cursor + anchor selection, boundary-aware movement, word jumps, undo / redo.
/// (Piece table per plan §4.8 is the FULL-profile structure; this MINIMAL buffer is a flat string with an undo log.)
class TextEditBuffer {
public:
    explicit TextEditBuffer(std::size_t maxLength = 4096) : m_maxLength(maxLength) {}
    const std::string& text() const { return m_text; }
    void setText(std::string_view s);
    std::size_t cursor() const { return m_cursor; }
    std::size_t anchor() const { return m_anchor; }
    bool hasSelection() const { return m_cursor != m_anchor; }
    std::string_view selection() const;

    void insert(std::string_view utf8); ///< replaces the selection
    void backspace();
    void del();
    void moveLeft(bool select);
    void moveRight(bool select);
    void wordLeft(bool select);
    void wordRight(bool select);
    void home(bool select);
    void end(bool select);
    void selectAll();
    bool undo();
    bool redo();
    std::size_t maxLength() const { return m_maxLength; }

private:
    struct Edit {
        std::string before, after;
        std::size_t cursorBefore, cursorAfter;
    };
    void replaceSelection(std::string_view s);
    void record(std::string before, std::size_t cursorBefore);
    std::string m_text;
    std::size_t m_cursor = 0, m_anchor = 0, m_maxLength;
    std::vector<Edit> m_undo, m_redo;
};

} // namespace fuse::text
