// fuse_text — fonts, shaping, line breaking, glyph cache and text editing (FUSE UI plan §4.8, phase UI3).
//
// Profiles (FUSE_UI_TEXT, decision D2 adopted 2026-10-29): FULL (HarfBuzz + FreeType + bidi + UAX #14/#29),
// MINIMAL (cmap + kerning shaping, stb_truetype grayscale rasterisation; Latin / Greek / Cyrillic / CJK, no complex
// scripts or bidi), COOKED (pre-baked .fusefont packs only, fuse/text/font_cook.hpp). MINIMAL is the default; FULL
// (FUSE_UI_TEXT=FULL, fetched HarfBuzz + FreeType) swaps in HarfBuzzShaper and FreeType coverage behind the same
// interfaces; COOKED reads advances, kerning and pre-rasterised glyphs from the pack (MINIMAL shaping, no font parsing).
// textProfile() reports what is compiled.
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
/// Byte offset of the previous / next code point boundary.
std::size_t prevCodepoint(std::string_view s, std::size_t i);
std::size_t nextCodepoint(std::string_view s, std::size_t i);
/// UAX #29 extended grapheme cluster property (tables generated from the UCD by Text/tools/gen_unicode_tables.py;
/// Extended_Pictographic folded in as ExtPict). Gate fuse_text_grapheme_conformance runs GraphemeBreakTest.txt.
enum class GraphemeBreak : std::uint8_t { Other, CR, LF, Control, Extend, ZWJ, RegionalIndicator, Prepend, SpacingMark, L, V, T, LV, LVT, ExtPict };
GraphemeBreak graphemeBreakProperty(char32_t cp);
/// Unicode version of the built-in grapheme tables ("15.1.0").
const char* unicodeVersion();
/// Byte offset of the previous / next extended grapheme cluster boundary (UAX #29 rules GB3-GB13): caret moves,
/// Backspace and Delete never split "e + U+0301", a flag, a skin-toned or ZWJ emoji, CR LF or a Hangul syllable.
std::size_t prevBoundary(std::string_view s, std::size_t i);
std::size_t nextBoundary(std::string_view s, std::size_t i);
/// Number of extended grapheme clusters in `s` (user-perceived characters).
std::size_t graphemeCount(std::string_view s);

// ---- Line breaking and bidi (segmentation used by layoutText) ------------------------------------------------------
enum class LineBreak : std::uint8_t { Must, Allow, None, Inside };
/// Normal: Conditional Japanese Starters (small kana, U+30FC ...) break like ideographs (CSS line-break: normal; what
/// layoutText uses). Strict: they resolve to NS, the UAX #14 default.
enum class LineBreakStyle : std::uint8_t { Normal, Strict };
/// Break opportunity after each byte of `utf8` (`out` gets one entry per byte; Inside for bytes within a code point;
/// the last byte is Must). FULL: UAX #14 through libunibreak (gate fuse_text_linebreak_conformance). MINIMAL: Allow
/// after a space that precedes a non-space, Must after '\n'.
void lineBreaks(std::string_view utf8, std::vector<LineBreak>& out, LineBreakStyle style = LineBreakStyle::Normal);

enum class BaseDirection : std::uint8_t { Ltr, Rtl, Auto };
/// FULL: UAX #9 embedding levels per byte of one line (SheenBidi for X1-I2, then rule L1 over the line) and the
/// level of its (last) paragraph; Auto applies P2/P3 with LTR as default. MINIMAL: no bidi, returns false with every
/// level at the base level. Gate fuse_text_bidi_conformance runs BidiCharacterTest.txt.
bool bidiLevels(std::string_view utf8, BaseDirection base, std::vector<std::uint8_t>& levels, std::uint8_t& paraLevel);
/// Rule L2: visual order of units with `levels`; out[k] = logical index shown at visual position k.
void reorderVisual(std::span<const std::uint8_t> levels, std::vector<std::uint32_t>& out);

// ---- Font ----------------------------------------------------------------------------------------------------------
struct FontMetrics {
    float ascent = 0.f, descent = 0.f, lineGap = 0.f; ///< pixels at the size asked for (descent is negative)
    float lineHeight() const { return ascent - descent + lineGap; }
};

/// One layer of a colour glyph: an ordinary outline glyph filled with a CPAL palette colour (straight RGBA), or with
/// the text colour when `foreground` is set. Layers paint bottom to top.
struct ColorLayer {
    std::uint32_t glyph = 0;
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
    bool foreground = false;
    /// The whole glyph is one RGBA image (CBDT / sbix bitmap strike or an exactly rendered COLRv1 paint graph):
    /// draw it with GlyphAtlas::getColor instead of a tinted coverage glyph.
    bool image = false;
};

/// A premultiplied RGBA8 image of a glyph (row stride w * 4); x0 / y0 place its top-left texel relative to the pen
/// on the baseline (y down), like AtlasGlyph.
struct ColorImage {
    int w = 0, h = 0;
    int x0 = 0, y0 = 0;
    std::vector<std::uint8_t> rgba;
};

/// Opt-in rasterisers (each links its own code only when called):
/// enableColorGlyphs: exact COLRv1 (gradients, transforms, clip boxes, composite modes) and CBDT / sbix bitmap
/// strikes (PNG). Without it COLRv1 glyphs fall back to flattened solid layers and bitmap-only glyphs draw nothing.
/// Call it before loading bitmap fonts (their strike index is read at load).
void enableColorGlyphs();
bool colorGlyphsEnabled();
/// enableMsdf: multi-channel signed distance fields (GlyphAtlas::getMsdf, Font::renderMsdf).
void enableMsdf();
bool msdfEnabled();
/// enableVariableFonts: the Font variation API (axes, instance, setVariation ...: fvar / avar / gvar / HVAR / MVAR,
/// COLRv1 Var* paints). Without it variable fonts draw at their default coordinates and instance() returns the font
/// itself (UI font-weight / font-stretch then do nothing). No-op in COOKED builds (packs are cooked at one instance).
/// Also makes FontCatalog list fvar named instances: call it before the first FontCatalog scan (FontCatalog::system).
void enableVariableFonts();
bool variableFontsEnabled();

// ---- Variable fonts (fvar / avar / gvar / HVAR / MVAR, COLRv1 Var* paints) ----------------------------------------
/// OpenType tag from four characters: fontTag("wght").
constexpr std::uint32_t fontTag(const char (&s)[5]) {
    return (std::uint32_t(std::uint8_t(s[0])) << 24) | (std::uint32_t(std::uint8_t(s[1])) << 16) |
           (std::uint32_t(std::uint8_t(s[2])) << 8) | std::uint32_t(std::uint8_t(s[3]));
}
struct VariationAxis {
    std::uint32_t tag = 0;
    float minValue = 0.f, defaultValue = 0.f, maxValue = 0.f;
    bool hidden = false;
    std::string name; ///< from the name table ("Weight")
};
struct NamedInstance {
    std::string name;          ///< subfamily name ("Semibold Display")
    std::vector<float> coords; ///< design value per axis (Font::axes order)
};
/// One axis setting in design units ({fontTag("wght"), 650}).
struct FontVariation {
    std::uint32_t tag = 0;
    float value = 0.f;
};
/// One OpenType feature setting over the whole text ({fontTag("tnum")}; value 0 turns a default feature such as liga
/// or kern off, n > 1 picks alternate n).
struct FontFeature {
    std::uint32_t tag = 0;
    std::uint32_t value = 1;
};

class ITextShaper;
namespace detail {
struct FontAccess;
}
struct TextLayoutDesc;
struct TextLayout;
struct ShapeCacheStats;

/// A TrueType / OpenType font (glyf or CFF outlines). Holds a copy of the file bytes; the parsed state points into it.
class Font {
public:
    /// `faceIndex` picks a face of a collection (.ttc / .otc); 0 for single-face files. COOKED builds accept only
    /// .fusefont packs (fromFile maps them instead of reading).
    static std::expected<Font, TextError> fromBytes(std::span<const std::uint8_t> bytes, std::uint32_t faceIndex = 0);
    /// `path` is UTF-8.
    static std::expected<Font, TextError> fromFile(const std::string& path, std::uint32_t faceIndex = 0);
    /// FUSE's bundled UI font (vendor/fonts/opensans) when the build knows its path; InvalidFont otherwise.
    static std::expected<Font, TextError> bundled();
    /// FUSE's bundled fallback faces in chain order: Noto Sans Hebrew, Arabic, Devanagari, Twemoji (COLRv0 colour
    /// emoji), Symbols 2, Thai and a Noto Sans CJK SC subset (kana, CJK punctuation, fullwidth forms, the ~1400 most
    /// frequent Han characters and Hangul: KS X 1001 syllables + jamo; CFF outlines). COOKED: their build-time packs. Fonts whose files are missing are skipped. Attach them with
    /// setFallbacks().
    static std::vector<Font> bundledFallbacks();

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
    /// Font units to pixels at `pixelSize`.
    float unitScale(float pixelSize) const;

    // ---- Fallback chain (Qt: QFont family substitution) ----
    /// Faces tried in order for code points this font has no glyph for (non-owning; they must outlive this font).
    /// Layout splits text into runs per face at grapheme-cluster granularity, so a base and its marks, emoji modifier
    /// and ZWJ sequences stay on one face.
    void setFallbacks(std::vector<const Font*> chain);
    /// 1 + the number of fallbacks; face 0 is this font.
    std::size_t faceCount() const;
    const Font& face(std::uint16_t i) const;
    /// First face (0 = this) whose cmap maps `cp`; 0 when no face has it (draws this font's .notdef).
    std::uint16_t faceFor(char32_t cp) const;
    /// Pixel size to use for face `i` so its em matches this font's em at `pixelSize`.
    float facePixelSize(std::uint16_t i, float pixelSize) const;

    // ---- Colour glyphs (COLR + CPAL palette 0, CBDT / CBLC, sbix) ----
    // COLRv0 layers are exact. With enableColorGlyphs(), COLRv1 paint graphs (glyphs without a v0 record) and bitmap
    // strikes are one `image` layer rendered by renderColor. Without it COLRv1 graphs are flattened to solid layers
    // (gradients become the mean colour of their stops, transforms are ignored) and bitmap glyphs have no layers.
    bool hasColorGlyphs() const;
    /// Layers of `glyph` when it is a colour glyph; empty for ordinary glyphs.
    std::span<const ColorLayer> colorLayers(std::uint32_t glyph) const;
    /// Renders an `image` colour glyph at `pixelSize`: the best bitmap strike scaled to the em size, or the COLRv1
    /// graph (`foreground`: text colour for palette index 0xFFFF, straight RGBA, R in the low byte). False when the
    /// glyph has no image or the rasteriser is not enabled.
    bool renderColor(std::uint32_t glyph, float pixelSize, std::uint32_t foreground, ColorImage& out) const;
    /// Multi-channel signed distance field of `glyph` at `pixelSize` (needs enableMsdf()): RGB per-channel distances,
    /// A the true distance, 255 * (0.5 + d / range) with d in pixels (inside > 127.5), padded by range / 2 + 1.
    /// False for glyphs without an outline.
    bool renderMsdf(std::uint32_t glyph, float pixelSize, float range, ColorImage& out) const;

    // ---- Variations ----
    // A variable font draws at its own coordinates (the default instance unless setVariation pinned others).
    // instance() returns another Font over the same file bytes at other coordinates: advances (HVAR, else gvar phantom
    // points), metrics (MVAR), outlines (MINIMAL: our glyf + gvar decoder; FULL: FreeType), shaping (FULL: HarfBuzz)
    // and COLRv1 Var* paints all follow them. Instances are owned by the font holding the bytes (they live as long as
    // it does), cached by normalised coordinates (at most 256; past that the nearest is returned) and found without
    // allocation. Needs enableVariableFonts(); not available in COOKED builds (packs are cooked at one instance:
    // fuse_text_cook --variation).
    bool isVariable() const;
    std::span<const VariationAxis> axes() const;
    std::span<const NamedInstance> namedInstances() const;
    /// Design coordinates of this font, one per axis (empty when not variable).
    std::span<const float> variationCoords() const;
    /// The instance with `settings` applied over this font's coordinates (unknown tags ignored, values clamped to the
    /// axis range). *this when they change nothing or the font is not variable.
    const Font& instance(std::span<const FontVariation> settings) const;
    /// Named instance `i` (namedInstances()); *this when out of range.
    const Font& namedInstance(std::size_t i) const;
    /// Pins this font's own coordinates (call before sharing it: caches keyed by id() are not invalidated). Used by
    /// FontCatalog::load for named-instance faces and by the cooker.
    void setVariation(std::span<const FontVariation> settings);

    // ---- OpenType features ----
    /// This font shaping with `features` (CSS font-feature-settings). FULL: HarfBuzz applies them (the run cache keys
    /// on them); MINIMAL / COOKED have no GSUB / GPOS and return *this. Variants are owned by this font (at most 64,
    /// past that the last one is reused), keyed by the first 8 settings and found without allocation. Called on a
    /// variant, the features replace its own.
    const Font& withFeatures(std::span<const FontFeature> features) const;

private:
    friend class HarfBuzzShaper;
    friend void layoutText(const Font&, const ITextShaper&, std::string_view, const TextLayoutDesc&, TextLayout&);
    friend ShapeCacheStats shapeCacheStats(const Font&);
    friend struct detail::FontAccess;
    struct State;
    static std::expected<Font, TextError> fromCooked(State* st, std::span<const std::uint8_t> pack); // COOKED only
    State* m_state = nullptr; // owned (pimpl); released in ~Font
    std::uint32_t m_id = 0;
};

// ---- Shaping -------------------------------------------------------------------------------------------------------
/// Glyph id given to line-break characters after layout: they break the line but are never drawn.
inline constexpr std::uint32_t kInvisibleGlyph = 0xFFFFFFFFu;

struct ShapedGlyph {
    std::uint32_t glyph = 0;
    std::uint32_t cluster = 0; ///< byte offset of the source code point
    float x = 0.f;             ///< pen position (pixels) relative to the run start
    float advance = 0.f;
    std::uint16_t face = 0;    ///< 0: the layout font; k: its fallback k (Font::face)
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

/// HarfBuzz shaping (FULL profile): ligatures, GPOS kerning, mark positioning, complex scripts. Glyphs come back in
/// visual order for one direction run; bidi reordering is not wired yet. Falls back to MinimalShaper when FULL is not
/// compiled in.
class HarfBuzzShaper final : public ITextShaper {
public:
    float shape(const Font& font, float pixelSize, std::string_view utf8, std::vector<ShapedGlyph>& out) const override;
};
/// The HarfBuzz shaper when FULL is compiled in, nullptr otherwise.
const ITextShaper* fullShaper();

/// The shaper of the compiled profile.
const ITextShaper& defaultShaper();

// ---- Line breaking and layout --------------------------------------------------------------------------------------
struct TextLine {
    std::uint32_t firstGlyph = 0, glyphCount = 0;
    std::uint32_t byteBegin = 0, byteEnd = 0;
    float width = 0.f;
    float baseline = 0.f; ///< y of the baseline from the top of the block
    bool rtl = false;     ///< paragraph base direction is right-to-left (FULL bidi); Start alignment then means right
};

/// Start / End follow the line direction (or TextLayoutDesc::rtl); Left / Right are physical.
enum class TextAlign : std::uint8_t { Start, Center, End, Left, Right };

struct TextLayoutDesc {
    float pixelSize = 14.f;
    float maxWidth = 0.f; ///< <= 0: no wrapping
    TextAlign align = TextAlign::Start;
    bool elide = false; ///< single line clipped to maxWidth with an ellipsis
    bool rtl = false;   ///< right-to-left base direction (UI `direction: rtl`): Start / End of every line follow it
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
/// FULL profile: bidi runs (UAX #9, SheenBidi) are shaped per direction, lines break at UAX #14 opportunities
/// (libunibreak) and each line is reordered visually (rule L2), so `glyphs` of a line are in visual order with
/// increasing x. Shaped runs go through a fixed-capacity per-font cache (ring pools, no steady-state allocation).
struct ShapeCacheStats {
    std::uint64_t hits = 0, misses = 0;
};
/// Run-cache counters of `font` (zeros in MINIMAL builds).
ShapeCacheStats shapeCacheStats(const Font& font);

/// Shapes one line with the default shaper and `font`'s fallback chain (glyph `face` set per run). Returns the width.
float shapeText(const Font& font, std::string_view utf8, float pixelSize, std::vector<ShapedGlyph>& out);

/// Calls fn(face, glyph, facePixelSize, layer) for every paint layer of a shaped glyph: once with layer == nullptr
/// (draw in the text colour) for an ordinary glyph, or once per colour layer bottom to top for a colour glyph.
template <class Fn> void forEachPaintGlyph(const Font& font, const ShapedGlyph& g, float pixelSize, Fn&& fn) {
    const Font& f = font.face(g.face);
    const float px = g.face ? font.facePixelSize(g.face, pixelSize) : pixelSize;
    const auto layers = f.colorLayers(g.glyph);
    if (layers.empty()) fn(f, g.glyph, px, static_cast<const ColorLayer*>(nullptr));
    else
        for (const ColorLayer& l : layers) fn(f, l.glyph, px, &l);
}

/// Width of a single line without wrapping (no allocation when `scratch` has capacity).
float measureText(const Font& font, std::string_view utf8, float pixelSize, std::vector<ShapedGlyph>& scratch);

// ---- System fonts --------------------------------------------------------------------------------------------------
/// Scripts the fallback chains know about (scriptOf maps a code point; Common for punctuation, digits, spaces).
enum class Script : std::uint8_t { Common, Latin, Greek, Cyrillic, Arabic, Hebrew, Devanagari, Thai, Han, Kana, Hangul, Emoji, Symbols };
Script scriptOf(char32_t cp);
const char* scriptName(Script s);

/// One face of an installed font file, read from its name / OS/2 / head tables (no outlines are loaded).
struct FontFaceInfo {
    std::string path;           ///< UTF-8 file path
    std::uint32_t index = 0;    ///< face index in a collection (.ttc / .otc)
    std::string family, style;  ///< typographic family / subfamily (name IDs 16 / 17, else 1 / 2)
    std::string fullName;       ///< name ID 4
    std::uint16_t weight = 400; ///< OS/2 usWeightClass
    std::uint16_t stretch = 5;  ///< OS/2 usWidthClass (5 = normal)
    bool italic = false;        ///< italic or oblique (OS/2 fsSelection, head macStyle)
    std::uint32_t unicodeRanges[4] = {}; ///< OS/2 ulUnicodeRange1..4 (script coverage hints)
    bool variable = false;        ///< the face has an fvar table (FontCatalog::load gives a Font that can instance)
    /// Variable faces are listed once at their default coordinates (namedInstance -1) and, with enableVariableFonts()
    /// called before the scan, once per named instance,
    /// named like DirectWrite's WWS families: STAT axis values other than weight / width / italic / slant join the
    /// family ("Segoe UI Variable Display"), the rest is the style ("Semibold"); weight / stretch / italic come from
    /// the instance's wght / wdth / ital / slnt coordinates.
    std::int32_t namedInstance = -1;
    bool covers(Script s) const;  ///< from unicodeRanges (Emoji / Symbols: by family name)
};

/// Font discovery without fontconfig (Qt: QFontDatabase). Windows: %WINDIR%\Fonts, the HKLM / HKCU
/// "...\Windows NT\CurrentVersion\Fonts" registry lists and %LOCALAPPDATA%\Microsoft\Windows\Fonts; Linux:
/// /usr/share/fonts, /usr/local/share/fonts, ~/.local/share/fonts, ~/.fonts; macOS: /System/Library/Fonts,
/// /Library/Fonts, ~/Library/Fonts. Only .ttf / .otf / .ttc / .otc files are read.
class FontCatalog {
public:
    /// The installed fonts, scanned once on first use (thread-safe).
    static const FontCatalog& system();
    /// Platform font directories in scan order.
    static std::vector<std::string> systemFontDirectories();

    /// Adds every face of every font file under `dir` (recursive). Returns the number of faces added.
    std::size_t scanDirectory(const std::string& dir);
    /// Adds the faces of one file (duplicates of an already listed path are ignored).
    std::size_t addFile(const std::string& path);

    std::span<const FontFaceInfo> faces() const { return m_faces; }
    /// Sorted, unique family names.
    std::vector<std::string> families() const;
    /// Best face of `family` (case-insensitive) for `weight` / `italic` (CSS-style: italic first, then the nearest
    /// weight, then stretch). Generic names (sans-serif, serif, monospace, system-ui) map to platform families.
    /// nullptr when no family matches.
    const FontFaceInfo* match(std::string_view family, std::uint16_t weight = 400, bool italic = false) const;
    /// Regular upright faces that cover `script`, best first (platform UI fonts for the script, then any other
    /// family whose OS/2 ranges claim it), one per family, at most `maxFaces`. Each face's cmap is checked for a letter
    /// of the script (the range bits alone are unreliable).
    std::vector<const FontFaceInfo*> systemFallbackChain(Script script, std::size_t maxFaces = 4) const;
    /// Loads a face (FontFaceInfo::path / index; a named instance is pinned with Font::setVariation).
    static std::expected<Font, TextError> load(const FontFaceInfo& face);

private:
    std::vector<FontFaceInfo> m_faces;
    std::vector<std::string> m_paths; // normalised paths already added
};

// ---- Glyph atlas ---------------------------------------------------------------------------------------------------
enum class AtlasKind : std::uint8_t {
    Coverage, ///< 8-bit coverage in the coverage page, tinted when drawn
    Color,    ///< premultiplied RGBA in the colour page
    Msdf,     ///< MSDF texels (RGB distances, A true distance) in the colour page, drawn scaled
};

struct AtlasGlyph {
    std::uint16_t u = 0, v = 0, w = 0, h = 0; ///< texels in the glyph's page
    std::int16_t x0 = 0, y0 = 0;              ///< offset from the pen on the baseline
    bool valid = false;
    AtlasKind kind = AtlasKind::Coverage;
};

/// Two pages in one buffer: an 8-bit coverage page (size x size) followed by an RGBA8 colour page
/// (size / 2 x size / 2, premultiplied colour glyphs and MSDF texels). Shelf packing per page and one open-addressed
/// lookup (no allocation on hit). When a page is full, both are reset and the generation bumps (callers re-request
/// their glyphs: LRU by page). pixels() is the whole buffer (coverage, then colour) as renderers upload it.
class GlyphAtlas {
public:
    /// MSDF glyphs are generated once at this pixel size with this distance range (pixels) and drawn scaled.
    static constexpr float kMsdfPixelSize = 32.f;
    static constexpr float kMsdfRange = 4.f;

    explicit GlyphAtlas(int size = 1024);
    /// Finds or rasterises `glyph` of `font` at `pixelSize` (sizes are bucketed to 1/4 px) as coverage.
    AtlasGlyph get(const Font& font, std::uint32_t glyph, float pixelSize);
    /// An `image` colour glyph (ColorLayer::image) as premultiplied RGBA (kind Color); w == 0 when it has no image.
    /// `foreground` (straight RGBA, R low) is the text colour COLRv1 graphs may use; it is part of their key.
    AtlasGlyph getColor(const Font& font, std::uint32_t glyph, float pixelSize, std::uint32_t foreground = 0xFF000000u);
    /// The MSDF of `glyph` at kMsdfPixelSize (kind Msdf; needs enableMsdf()). Draw it scaled by pixelSize /
    /// kMsdfPixelSize: x0 / y0 / w / h are texels at the MSDF size.
    AtlasGlyph getMsdf(const Font& font, std::uint32_t glyph);
    /// Caller-keyed coverage (UI icons). Keys must have bit 63 clear (font glyph keys set it) and be non-zero.
    bool lookup(std::uint64_t key, AtlasGlyph& out) const;
    /// Copies a w x h coverage bitmap (row stride `stride`) under `key`; x0 / y0 are stored as the glyph offset.
    AtlasGlyph insert(std::uint64_t key, const std::uint8_t* src, int w, int h, int stride, int x0, int y0);
    /// Copies a w x h premultiplied RGBA8 bitmap (row stride `stride` bytes) into the colour page under `key`
    /// (0: placed but not recorded).
    AtlasGlyph insertColor(std::uint64_t key, const std::uint8_t* rgba, int w, int h, int stride, int x0, int y0,
                           AtlasKind kind = AtlasKind::Color);
    int size() const { return m_size; }
    /// Side of the colour page (size() / 2); its texels start at pixels() + size() * size(), 4 bytes each.
    int colorSize() const { return m_size / 2; }
    const std::uint8_t* pixels() const { return m_pixels.data(); }
    const std::uint8_t* colorPixels() const { return m_pixels.data() + std::size_t(m_size) * std::size_t(m_size); }
    std::uint32_t generation() const { return m_generation; }
    std::uint32_t misses() const { return m_misses; }
    void clear();

private:
    struct Slot {
        std::uint64_t key = 0;
        std::uint32_t aux = 0;
        AtlasGlyph g;
    };
    struct Shelf {
        int x = 1, y = 1, h = 0;
    };
    bool find(std::uint64_t key, std::uint32_t aux, std::size_t& slot) const;
    static bool place(Shelf& s, int pageSize, int w, int h, int& u, int& v);
    AtlasGlyph store(std::uint64_t key, std::uint32_t aux, const AtlasGlyph& g);
    int m_size;
    std::vector<std::uint8_t> m_pixels;
    std::vector<Slot> m_slots;
    std::uint32_t m_used = 0;
    Shelf m_shelf, m_colorShelf;
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
