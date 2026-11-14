// fuse_text — fonts, shaping, line breaking, glyph cache and text editing (FUSE UI plan §4.8, phase UI3).
//
// Profiles (FUSE_UI_TEXT, decision D2 adopted 2026-10-29): FULL (HarfBuzz + FreeType + bidi + UAX #14/#29),
// MINIMAL (cmap + kerning shaping, stb_truetype grayscale rasterisation; Latin / Greek / Cyrillic / CJK, no complex
// scripts or bidi), COOKED (pre-baked .fusefont packs only, fuse/text/font_cook.hpp). MINIMAL is the default; FULL
// (FUSE_UI_TEXT=FULL, fetched HarfBuzz + FreeType) swaps in HarfBuzzShaper and FreeType coverage behind the same
// interfaces; COOKED reads advances, kerning and pre-rasterised glyphs from the pack (MINIMAL shaping, no font parsing).
// textProfile() reports what is compiled.
//
// Font families, synthetic bold / oblique, installed-font discovery, scripts and system fallback chains:
// fuse/text/font_family.hpp. Plural rules, number formatting and pseudo-localisation: fuse/text/i18n.hpp.
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
/// True when `utf8` holds a right-to-left character or an RLM / RLE / RLO mark (Hebrew, Arabic, Syriac, Thaana, NKo,
/// presentation forms): text that needs the bidi pass. Pure left-to-right text is shown in logical order.
bool hasRtlText(std::string_view utf8);
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
/// itself (UI font-stretch / font-variation-settings then do nothing). No-op in COOKED builds (packs are cooked at
/// one instance). Also makes systemFonts() (font_family.hpp) list fvar named instances: call it before the first scan
/// (or call refreshSystemFonts() after it).
void enableVariableFonts();
bool variableFontsEnabled();
/// Bumped whenever any font's fallback chain, bold face or pinned variation changes (what text shapes to may differ
/// although the Font object is the same): caches of laid-out or painted text key on it.
std::uint32_t fontChainGeneration();

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

/// Where the bundled fonts come from on platforms without a build-tree font directory (Android assets, an iOS /
/// macOS app bundle): `read` fills `out` with the bytes of `name` ("opensans/OpenSans.ttf", "noto/...") and returns
/// true. Font::bundled() / bundledFallbacks() ask the source first, then the build's font directory, then (default
/// face only) the copy embedded with FUSE_TEXT_EMBED_FONT. Set it before the first font is loaded.
struct FontDataSource {
    void* user = nullptr;
    bool (*read)(void* user, std::string_view name, std::vector<std::uint8_t>& out) = nullptr;
};
void setFontDataSource(const FontDataSource& source);

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
    /// `faceIndex` selects a face of a TrueType collection (.ttc); 0 for plain .ttf / .otf files. COOKED builds accept
    /// only .fusefont packs (fuse/text/font_cook.hpp; fromFile maps them instead of reading; one face per pack).
    static std::expected<Font, TextError> fromBytes(std::span<const std::uint8_t> bytes, int faceIndex = 0);
    static std::expected<Font, TextError> fromFile(const std::string& path, int faceIndex = 0);
    /// An installed font by family name, nearest weight and slant (fuse/text/font_family.hpp: systemFonts()).
    /// InvalidFont when the family is not installed. The face is the real file: nothing is synthesised here.
    static std::expected<Font, TextError> fromSystem(std::string_view family, int weight = 400, bool italic = false);
    /// A synthetic variant of `base` sharing its file bytes: `embolden` widens every stem by that fraction of the em
    /// (the rasterised coverage is smeared sideways; advances grow by the same amount) and `slant` shears the outlines
    /// before rasterisation (x += slant * height above the baseline; 0.2 is the usual oblique). The variant has its own id(), so glyph atlases keep it apart from `base`.
    /// It keeps `base`'s fallback chain and variation coordinates. Invalid when `base` is. COOKED packs hold no
    /// outlines: `slant` is ignored there (emboldening works: it is done on the coverage).
    static Font synthesized(const Font& base, float embolden, float slant);
    /// FUSE's bundled UI font (vendor/fonts/opensans): from the FontDataSource, the build's font path or the embedded
    /// copy (FUSE_TEXT_EMBED_FONT, the default on Android / iOS); InvalidFont when none has it.
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
    // ---- Face description (name / OS/2 / head / post tables) ----
    /// Family ("Open Sans"; typographic family when the font has one), style ("Regular", "Bold Italic"), CSS weight
    /// 1..1000 (the wght coordinate of a variable font when enableVariableFonts() was called, else usWeightClass; 400
    /// when absent) and slant of the real face. Synthetic variants report their base's. COOKED packs: empty names, 400.
    std::string_view familyName() const;
    std::string_view styleName() const;
    int weight() const;
    bool italic() const;
    bool monospace() const;
    /// Synthetic style of this object (both 0 for a real face).
    float syntheticEmbolden() const;
    float syntheticSlant() const;
    /// Underline / strike-through geometry in pixels at `pixelSize`: offsets are the top edge relative to the
    /// baseline (y down: underline positive, strike-through negative). From post / OS/2, with defaults when absent.
    struct DecorationMetrics {
        float underlineOffset = 0.f, underlineThickness = 1.f, strikeOffset = 0.f, strikeThickness = 1.f;
    };
    DecorationMetrics decorationMetrics(float pixelSize) const;
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
    /// Fallbacks follow this font's weight: when weight() >= 600 (a real bold face or a variable instance; a
    /// synthesized() variant reports its base's weight and keeps its fallbacks as they are), face(i) is the fallback's
    /// bold form: the face given to it with setBoldFace (e.g. a findSystemFont(family, 700) face), else a variable
    /// fallback's instance at this weight, else the fallback emboldened(). Colour fonts are never emboldened.
    /// The face to use for this font when it is the fallback of a bold primary (non-owning; nullptr: none).
    void setBoldFace(const Font* bold);
    const Font* boldFace() const;
    /// This font with synthetic bold (owned by this font; same rasterisation as synthesized(*this, 1 / 24, 0), FreeType's
    /// FT_GlyphSlot_Embolden strength; MSDF: distances biased outward). *this for colour fonts, fonts already synthetic
    /// or at weight >= 600, and in COOKED builds.
    const Font& emboldened() const;
    /// The synthetic bold strength in pixels at `pixelSize` (syntheticEmbolden() of the em; 0 for a real face).
    float syntheticBold(float pixelSize) const;

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
    // fuse_text_cook --variation). FontFamily (font_family.hpp) uses a variable face's wght axis for its weights.
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
    /// loadSystemFont (font_family.hpp) for named-instance faces and by the cooker.
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
    void rawGlyphBox(std::uint32_t glyph, float pixelSize, int& x0, int& y0, int& x1, int& y1) const;
    void rawRasterise(std::uint32_t glyph, float pixelSize, std::uint8_t* dst, int w, int h, int stride) const;
    float baseAdvance(std::uint32_t glyph, float pixelSize) const;
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
    float letterSpacing = 0.f; ///< extra pixels after every glyph that advances (CSS letter-spacing)
    float lineHeight = 0.f;    ///< line box height in pixels; <= 0: the font's natural line height. Extra (or
                               ///< missing) space is split above and below the text (CSS half-leading).
    bool rtl = false;   ///< right-to-left base direction (UI `direction: rtl`): Start / End of every line follow it
    /// Paragraph base direction for bidi (FULL): Auto = the first strong character (P2 / P3), Ltr / Rtl = given (the UI
    /// passes the `direction` property when it is set). MINIMAL has no bidi and ignores it.
    BaseDirection base = BaseDirection::Auto;
};

/// A shaped, line-broken block. Reuse one object per text node: relayout keeps every buffer's capacity.
struct TextLayout {
    std::vector<ShapedGlyph> glyphs; ///< x already includes line alignment; y comes from the line
    std::vector<TextLine> lines;
    float width = 0.f, height = 0.f;
    FontMetrics metrics;
    const Font* font = nullptr; ///< the font this block was laid out with (non-owning; painters use it)
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
    /// Puts the cursor at byte `pos` (clamped to the text; widgets moving the caret in visual order over bidi text).
    void setCursor(std::size_t pos, bool select);
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
