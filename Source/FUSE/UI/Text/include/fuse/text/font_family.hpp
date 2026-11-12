// fuse_text — font families, synthetic bold / oblique and installed-font discovery (FUSE UI plan §4.8; Qt: QFont,
// QFontDatabase).
//
// A FontFamily groups the real faces of one family (regular, bold, italic, ...) and answers "the font for weight w,
// italic i": the nearest real face when there is one, otherwise a synthetic variant of the nearest face
// (Font::synthesized: coverage emboldening and a shear, done where glyphs are rasterised, so the CPU rasteriser and the
// GPU glyph path, which samples the same atlas, both get it). Variants are created on first use and then reused, so a
// steady-state frame does not allocate.
//
// Installed fonts are found by scanning the platform font directories and reading each file's name / OS/2 / head /
// post tables directly: no fontconfig, CoreText or DirectWrite dependency. .ttf, .otf (glyf and CFF outlines) and
// .ttc / .otc collections are listed; bitmap-only and CFF2 (variable .otf) faces are skipped because the rasterisers
// cannot draw them. A variable .ttf is listed as its default instance.
#pragma once

#include "fuse/text/text.hpp"

#include <memory>

namespace fuse::text {

/// CSS weights.
inline constexpr int kWeightNormal = 400;
inline constexpr int kWeightBold = 700;
/// Shear of a synthetic oblique (about 11 degrees) and stem growth, in em, of a synthetic bold made from a regular.
inline constexpr float kObliqueSlant = 0.2f;
inline constexpr float kBoldEmbolden = 1.f / 16.f;

/// Stable 24-bit hash of a family name (ASCII case-insensitive, surrounding blanks and quotes ignored; never 0).
/// This is what the style engine stores for `font-family`.
std::uint32_t familyHash(std::string_view name);

/// Ranks a candidate face against a request (lower is better): slant first, then CSS weight matching (for 400..500
/// the range up to 500 first, lighter requests prefer lighter faces, bolder requests bolder ones).
int fontMatchScore(int wantWeight, bool wantItalic, int faceWeight, bool faceItalic);

class FontFamily {
public:
    FontFamily() = default;
    explicit FontFamily(std::string name) : m_name(std::move(name)) {}
    FontFamily(FontFamily&&) noexcept = default;
    FontFamily& operator=(FontFamily&&) noexcept = default;

    const std::string& name() const { return m_name; }
    void setName(std::string name) { m_name = std::move(name); }

    /// Adds a real face the family owns; its weight and slant come from the font (OS/2, head). Returns it (stable
    /// address for the family's lifetime), or nullptr when `face` is invalid.
    const Font* add(Font face);
    /// Same with the weight / slant given (a font whose tables are wrong, or a deliberate alias).
    const Font* add(Font face, int weight, bool italic);
    /// Adds a face owned by the caller (it must outlive the family).
    void addExternal(const Font* face);
    void addExternal(const Font* face, int weight, bool italic);

    bool empty() const { return m_members.empty(); }
    std::size_t realFaceCount() const { return m_members.size(); }
    /// True when a real face has exactly this weight and slant.
    bool hasReal(int weight, bool italic) const;
    /// Nearest real face (fontMatchScore); nullptr when the family is empty.
    const Font* nearestReal(int weight, bool italic) const;

    struct Match {
        const Font* font = nullptr; ///< nullptr only when the family is empty
        bool syntheticBold = false, syntheticOblique = false;
    };
    /// The font to draw (weight, italic) with. A real face when it is close enough; otherwise the nearest real face
    /// emboldened (requests of 600 and more from a face at least 150 lighter) and / or sheared (italic asked from
    /// an upright face). Synthetic variants are created on the first request and cached.
    Match match(int weight, bool italic);
    const Font* resolve(int weight, bool italic) { return match(weight, italic).font; }

    /// Fallback chain for every owned face and synthetic variant (Font::setFallbacks; the faces must outlive the
    /// family). Faces added with addExternal keep their own chain, which their variants copy.
    void setFallbacks(std::vector<const Font*> chain);

    /// The bundled UI family: Open Sans regular (bold and italic are synthesised). Empty when the font is missing.
    static FontFamily bundled();
    /// Every installed face of `family` (systemFonts()). InvalidFont when the family is not installed.
    static std::expected<FontFamily, TextError> fromSystem(std::string_view family);

private:
    struct Member {
        const Font* font = nullptr;
        std::unique_ptr<Font> owned;
        int weight = 400;
        bool italic = false;
    };
    struct Variant {
        const Font* base = nullptr;
        int boldSteps = 0; ///< weight difference emboldened, in units of 50
        bool oblique = false;
        std::unique_ptr<Font> font;
    };
    const Member* nearestMember(int weight, bool italic) const;
    std::string m_name;
    std::vector<Member> m_members;
    std::vector<Variant> m_variants;
    std::vector<const Font*> m_fallbacks;
    bool m_hasFallbacks = false;
};

// ---- Installed fonts -----------------------------------------------------------------------------------------------
struct SystemFontInfo {
    std::string family; ///< "DejaVu Sans" (typographic family when the font has one)
    std::string style;  ///< "Bold Oblique"
    std::string path;
    int faceIndex = 0;  ///< face inside a .ttc collection
    int weight = 400;   ///< CSS weight (OS/2 usWeightClass)
    bool italic = false, monospace = false;
};

/// The platform's font directories, most specific (per-user) first: Linux / BSD: $XDG_DATA_HOME/fonts, ~/.fonts,
/// $XDG_DATA_DIRS/fonts, /usr/local/share/fonts, /usr/share/fonts, X11 font directories; macOS / iOS: ~/Library/Fonts,
/// /Library/Fonts, /System/Library/Fonts; Android: /system/fonts, /product/fonts, /data/fonts; Windows:
/// %LOCALAPPDATA%\Microsoft\Windows\Fonts, %WINDIR%\Fonts. Directories that do not exist are still listed.
std::vector<std::string> systemFontDirectories();
/// Appends every face of the font file at `path` to `out`; false when it is not a usable font.
bool describeFontFile(const std::string& path, std::vector<SystemFontInfo>& out);
/// Appends every face found under `dir` (recursive, bounded depth; unreadable entries are skipped).
void scanFontDirectory(const std::string& dir, std::vector<SystemFontInfo>& out);

/// Every installed face, sorted by family (case-insensitive), weight, slant. Scanned on first use and cached; a
/// machine without fonts yields an empty list. Thread-safe. The span stays valid until the list is rebuilt
/// (setSystemFontDirectories / refreshSystemFonts).
std::span<const SystemFontInfo> systemFonts();
/// Family names of systemFonts(), unique, in order.
std::vector<std::string> systemFontFamilies();
/// Best face of `family` (case-insensitive) for the weight and slant; nullptr when the family is not installed.
const SystemFontInfo* findSystemFont(std::string_view family, int weight = 400, bool italic = false);
/// Same lookup by familyHash().
const SystemFontInfo* findSystemFontByHash(std::uint32_t familyHash, int weight = 400, bool italic = false);
/// Replaces the scanned directories (tests, apps shipping a font folder) and drops the cache; an empty list restores
/// systemFontDirectories().
void setSystemFontDirectories(std::vector<std::string> dirs);
/// Drops the cache: the next systemFonts() scans again (fonts installed while running).
void refreshSystemFonts();

} // namespace fuse::text
