// fuse_text_font_styles: font families, weights and styles, synthetic bold / oblique, installed-font discovery, text
// decorations, letter spacing and line height (plan §4.8; Qt GUI fonts / text rows).
//   * face description: family / style / weight / slant read from the name, OS/2 and head tables
//   * synthesis, pixel level: a synthetic bold has more coverage, a wider box and a larger advance; a synthetic
//     oblique leans (the top of a stem is right of its foot) with the same advance; a variant with no style is the
//     base face byte for byte; the glyph atlas keeps regular / bold / oblique apart
//   * FontFamily: weight matching (CSS order), a real face is preferred when present, variants are synthesised from the
//     nearest face once and reused, fallback chains carry over
//   * system fonts: the scanner over a temp directory holding the vendored fonts (nested folders, a .ttc collection
//     built here, a corrupt file, non-font files); lookups by name / hash / weight; an empty and a missing directory
//   * CSS: font-weight / font-style / font-family (registered family, installed family, unknown -> fallback),
//     inheritance, text-decoration (rule geometry and pixels), letter-spacing, line-height, buttons (measureString)
//   * render scale 2: synthetic styles are rasterised at the device size
//   * golden: regular / bold / italic / bold italic / underline / strike-through / spacing / rich text / markdown
//     against tests/golden/text_styles[_full].png (FUSE_UI_UPDATE_GOLDEN=1 rewrites it; --out=<png> also writes it)
#include "test_util.hpp"

#include "fuse/text/font_family.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;
namespace ft = fuse::text;
namespace fs = std::filesystem;

namespace {
const std::string kFontDir = FUSE_UI_TEST_FONT_DIR;

struct Bitmap {
    int x0 = 0, y0 = 0, w = 0, h = 0;
    std::vector<std::uint8_t> px;
    double coverage() const {
        double s = 0;
        for (std::uint8_t v : px) s += v;
        return s;
    }
    /// Coverage-weighted mean x of rows [r0, r1).
    double centroidX(int r0, int r1) const {
        double sx = 0, sw = 0;
        for (int y = r0; y < r1; ++y)
            for (int x = 0; x < w; ++x) {
                const double v = px[std::size_t(y) * std::size_t(w) + std::size_t(x)];
                sx += v * (double(x0 + x) + 0.5);
                sw += v;
            }
        return sw > 0 ? sx / sw : 0.0;
    }
};
Bitmap rasterise(const ft::Font& f, char32_t cp, float px) {
    Bitmap b;
    int x1, y1;
    const std::uint32_t g = f.glyphIndex(cp);
    f.glyphBox(g, px, b.x0, b.y0, x1, y1);
    b.w = x1 - b.x0;
    b.h = y1 - b.y0;
    b.px.assign(std::size_t(b.w) * std::size_t(b.h), 0);
    if (b.w > 0 && b.h > 0) f.rasterise(g, px, b.px.data(), b.w, b.h, b.w);
    return b;
}
std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
    v[at] = std::uint8_t(x >> 24);
    v[at + 1] = std::uint8_t(x >> 16);
    v[at + 2] = std::uint8_t(x >> 8);
    v[at + 3] = std::uint8_t(x);
}
std::uint32_t get32(const std::vector<std::uint8_t>& v, std::size_t at) {
    return (std::uint32_t(v[at]) << 24) | (std::uint32_t(v[at + 1]) << 16) | (std::uint32_t(v[at + 2]) << 8) | v[at + 3];
}
/// A TrueType collection holding the given fonts whole, their table offsets rebased.
std::vector<std::uint8_t> makeTtc(const std::vector<std::vector<std::uint8_t>>& fonts) {
    std::vector<std::uint8_t> out(12 + fonts.size() * 4, 0);
    std::memcpy(out.data(), "ttcf", 4);
    put32(out, 4, 0x00010000u);
    put32(out, 8, std::uint32_t(fonts.size()));
    for (std::size_t k = 0; k < fonts.size(); ++k) {
        while (out.size() % 4) out.push_back(0);
        const std::size_t base = out.size();
        put32(out, 12 + k * 4, std::uint32_t(base));
        out.insert(out.end(), fonts[k].begin(), fonts[k].end());
        const std::size_t tables = (std::size_t(out[base + 4]) << 8) | out[base + 5];
        for (std::size_t t = 0; t < tables; ++t) {
            const std::size_t rec = base + 12 + t * 16;
            put32(out, rec + 8, get32(out, rec + 8) + std::uint32_t(base));
        }
    }
    return out;
}
ft::Font load(const std::string& rel, int index = 0) {
    auto f = ft::Font::fromFile(kFontDir + "/" + rel, index);
    UI_CHECK(f.has_value());
    return f ? std::move(*f) : ft::Font();
}
bool near(double a, double b, double eps) { return std::abs(a - b) <= eps; }

// ---- face description, synthesis ---------------------------------------------------------------------------------
void gateFaces() {
    const ft::Font& reg = font();
    UI_CHECK(reg.familyName() == "Open Sans");
    UI_CHECK_EQ(reg.weight(), 400);
    UI_CHECK(!reg.italic() && !reg.monospace());
    UI_CHECK(!reg.styleName().empty());
    UI_CHECK(reg.syntheticEmbolden() == 0.f && reg.syntheticSlant() == 0.f);
    const ft::Font heb = load("noto/NotoSansHebrew-Regular.ttf");
    UI_CHECK(heb.familyName() == "Noto Sans Hebrew");
    UI_CHECK(heb.styleName() == "Regular");
    UI_CHECK(!ft::Font::fromFile(kFontDir + "/opensans/OpenSans.ttf", 3).has_value()); // not a collection

    const auto dm = reg.decorationMetrics(32.f);
    UI_CHECK(dm.underlineOffset >= 1.f && dm.underlineOffset < 12.f);
    UI_CHECK(dm.underlineThickness >= 1.f && dm.underlineThickness <= 4.f);
    UI_CHECK(dm.strikeOffset < -4.f && dm.strikeOffset > -24.f); // inside the x-height
    UI_CHECK(dm.strikeThickness >= 1.f);
}

void gateSynthesis() {
    const ft::Font& reg = font();
    const float px = 48.f;
    const ft::Font bold = ft::Font::synthesized(reg, ft::kBoldEmbolden, 0.f);
    const ft::Font obl = ft::Font::synthesized(reg, 0.f, ft::kObliqueSlant);
    const ft::Font both = ft::Font::synthesized(reg, ft::kBoldEmbolden, ft::kObliqueSlant);
    const ft::Font same = ft::Font::synthesized(reg, 0.f, 0.f);
    UI_CHECK(bold.valid() && obl.valid() && both.valid() && same.valid());
    UI_CHECK(bold.id() != reg.id() && obl.id() != reg.id() && bold.id() != obl.id());
    UI_CHECK(bold.syntheticEmbolden() > 0.f && bold.syntheticSlant() == 0.f);
    UI_CHECK(obl.syntheticSlant() == ft::kObliqueSlant && obl.syntheticEmbolden() == 0.f);
    UI_CHECK(bold.familyName() == "Open Sans" && bold.weight() == 400); // the description is the real face's
    UI_CHECK(!ft::Font::synthesized(ft::Font(), 0.1f, 0.2f).valid());

    // no style: the base face, byte for byte
    for (char32_t cp : {U'l', U'H', U'g', U'@'}) {
        const Bitmap a = rasterise(reg, cp, px), b = rasterise(same, cp, px);
        UI_CHECK(a.x0 == b.x0 && a.y0 == b.y0 && a.w == b.w && a.h == b.h && a.px == b.px);
    }

    // bold: more ink on every glyph, a wider box, the same height, a larger advance
    const std::uint32_t gl = reg.glyphIndex(U'l');
    const float grow = bold.advance(gl, px) - reg.advance(gl, px);
    UI_CHECK(grow > 1.f && grow < 5.f); // em / 16 at 48 px
    for (char32_t cp : {U'l', U'H', U'o', U'e', U'W'}) {
        const Bitmap a = rasterise(reg, cp, px), b = rasterise(bold, cp, px);
        UI_CHECK(b.coverage() > a.coverage() * 1.15);
        UI_CHECK(b.w == a.w + int(std::ceil(grow)) && b.h == a.h && b.x0 == a.x0 && b.y0 == a.y0);
    }
    {
        // the stem of 'l' is wider by the stem growth: coverage of the middle row, in pixels
        const Bitmap a = rasterise(reg, U'l', px), b = rasterise(bold, U'l', px);
        auto rowWidth = [](const Bitmap& m) {
            double s = 0;
            for (int x = 0; x < m.w; ++x) s += m.px[std::size_t(m.h / 2) * std::size_t(m.w) + std::size_t(x)];
            return s / 255.0;
        };
        UI_CHECK(near(rowWidth(b) - rowWidth(a), double(grow), 0.2));
    }
    UI_CHECK(reg.advance(reg.glyphIndex(U' '), px) > 0.f);
    UI_CHECK(bold.metrics(px).ascent == reg.metrics(px).ascent);

    // oblique: the stem of 'l' leans right going up by slant * height; the advance and the ink are the regular's
    {
        const Bitmap a = rasterise(reg, U'l', px), o = rasterise(obl, U'l', px);
        const int band = std::max(2, a.h / 6);
        const double upright = a.centroidX(0, band) - a.centroidX(a.h - band, a.h);
        const double lean = o.centroidX(0, band) - o.centroidX(o.h - band, o.h);
        const double expect = double(ft::kObliqueSlant) * double(a.h - band);
        UI_CHECK(std::abs(upright) < 0.5);
        UI_CHECK(near(lean, expect, 1.0));
        UI_CHECK(o.w > a.w && o.h == a.h);
        UI_CHECK(near(o.coverage(), a.coverage(), a.coverage() * 0.03)); // a shear keeps the area
        UI_CHECK(obl.advance(gl, px) == reg.advance(gl, px));
    }
    {
        const Bitmap b = rasterise(bold, U'l', px), bo = rasterise(both, U'l', px), o = rasterise(obl, U'l', px);
        UI_CHECK(bo.coverage() > o.coverage() * 1.15);
        const int band = std::max(2, b.h / 6);
        UI_CHECK(bo.centroidX(0, band) - bo.centroidX(bo.h - band, bo.h) > 4.0);
    }

    // shaping: a synthetic bold run is wider by the growth of each advancing glyph; an oblique run is not
    std::vector<ft::ShapedGlyph> sg;
    const float wr = ft::measureText(reg, "Hamburg", px, sg);
    const float wb = ft::measureText(bold, "Hamburg", px, sg);
    const float wo = ft::measureText(obl, "Hamburg", px, sg);
    UI_CHECK(near(wb - wr, 7.0 * double(grow), 0.05));
    UI_CHECK(near(wo, wr, 1e-3));

    // the atlas keys regular, bold and oblique apart
    ft::GlyphAtlas atlas(256);
    const auto ar = atlas.get(reg, gl, px), ab = atlas.get(bold, gl, px), ao = atlas.get(obl, gl, px);
    UI_CHECK_EQ(atlas.misses(), 3u);
    UI_CHECK(ar.valid && ab.valid && ao.valid);
    UI_CHECK(ab.w > ar.w && ao.w > ar.w);
    UI_CHECK(!(ar.u == ab.u && ar.v == ab.v) && !(ar.u == ao.u && ar.v == ao.v) && !(ab.u == ao.u && ab.v == ao.v));
    const auto again = atlas.get(reg, gl, px);
    UI_CHECK_EQ(atlas.misses(), 3u);
    UI_CHECK(again.u == ar.u && again.v == ar.v && again.w == ar.w);
    {
        // the atlas texels of the bold glyph are what Font::rasterise produces
        const Bitmap b = rasterise(bold, U'l', px);
        bool equal = b.w == ab.w && b.h == ab.h;
        for (int y = 0; equal && y < b.h; ++y)
            equal = std::memcmp(atlas.pixels() + std::size_t(ab.v + y) * std::size_t(atlas.size()) + ab.u,
                                b.px.data() + std::size_t(y) * std::size_t(b.w), std::size_t(b.w)) == 0;
        UI_CHECK(equal);
    }
}

// ---- families ------------------------------------------------------------------------------------------------------
void gateFamily() {
    // CSS weight matching
    UI_CHECK(ft::fontMatchScore(400, false, 400, false) == 0);
    UI_CHECK(ft::fontMatchScore(400, false, 500, false) < ft::fontMatchScore(400, false, 300, false));
    UI_CHECK(ft::fontMatchScore(400, false, 300, false) < ft::fontMatchScore(400, false, 600, false));
    UI_CHECK(ft::fontMatchScore(700, false, 900, false) < ft::fontMatchScore(700, false, 600, false));
    UI_CHECK(ft::fontMatchScore(300, false, 200, false) < ft::fontMatchScore(300, false, 400, false));
    UI_CHECK(ft::fontMatchScore(700, true, 400, true) < ft::fontMatchScore(700, true, 700, false)); // slant first
    UI_CHECK(ft::familyHash("Open Sans") == ft::familyHash("  \"open sans\" "));
    UI_CHECK(ft::familyHash("Open Sans") != ft::familyHash("OpenSans"));
    UI_CHECK(ft::familyHash("") != 0u && ft::familyHash("Open Sans") <= 0xFFFFFFu);

    // one regular face: everything else is synthesised from it, once
    ft::FontFamily fam = ft::FontFamily::bundled();
    UI_CHECK(fam.name() == "Open Sans");
    UI_CHECK_EQ(fam.realFaceCount(), std::size_t(1));
    UI_CHECK(fam.hasReal(400, false) && !fam.hasReal(700, false) && !fam.hasReal(400, true));
    const ft::Font* regular = fam.nearestReal(400, false);
    const auto mr = fam.match(400, false);
    UI_CHECK(mr.font == regular && !mr.syntheticBold && !mr.syntheticOblique);
    const auto mb = fam.match(700, false);
    UI_CHECK(mb.font && mb.font != regular && mb.syntheticBold && !mb.syntheticOblique);
    UI_CHECK(mb.font->syntheticEmbolden() == ft::kBoldEmbolden && mb.font->syntheticSlant() == 0.f);
    UI_CHECK(fam.match(700, false).font == mb.font); // cached
    UI_CHECK(fam.resolve(700, false) == mb.font);
    const auto mi = fam.match(400, true);
    UI_CHECK(mi.font != regular && mi.font != mb.font && mi.syntheticOblique && !mi.syntheticBold);
    UI_CHECK(mi.font->syntheticSlant() == ft::kObliqueSlant && mi.font->syntheticEmbolden() == 0.f);
    const auto mbi = fam.match(700, true);
    UI_CHECK(mbi.syntheticBold && mbi.syntheticOblique && mbi.font != mb.font && mbi.font != mi.font);
    UI_CHECK(fam.match(500, false).font == regular); // medium is close enough to regular: no synthesis
    const auto m6 = fam.match(600, false), m9 = fam.match(900, false);
    UI_CHECK(m6.syntheticBold && m9.syntheticBold);
    UI_CHECK(m6.font->syntheticEmbolden() < mb.font->syntheticEmbolden());
    UI_CHECK(m9.font->syntheticEmbolden() > mb.font->syntheticEmbolden());
    UI_CHECK(fam.match(300, false).font == regular); // there is no synthetic light

    // a real bold face is preferred to a synthetic one, and a bold italic is sheared from it
    ft::FontFamily two("Two");
    const ft::Font* r0 = two.add(load("opensans/OpenSans.ttf"));
    const ft::Font* b0 = two.add(load("noto/NotoSansSymbols2-Regular.ttf"), 700, false); // stands in for a bold file
    UI_CHECK(r0 && b0 && two.realFaceCount() == 2);
    UI_CHECK(two.hasReal(700, false));
    const auto t7 = two.match(700, false);
    UI_CHECK(t7.font == b0 && !t7.syntheticBold && !t7.syntheticOblique);
    UI_CHECK(two.match(800, false).font == b0 && !two.match(800, false).syntheticBold);
    UI_CHECK(two.match(400, false).font == r0);
    const auto t7i = two.match(700, true);
    UI_CHECK(t7i.syntheticOblique && !t7i.syntheticBold);
    UI_CHECK(t7i.font != b0 && t7i.font->familyName() == b0->familyName() && t7i.font->syntheticEmbolden() == 0.f);
    const auto t4i = two.match(400, true);
    UI_CHECK(t4i.syntheticOblique && t4i.font->familyName() == "Open Sans");
    UI_CHECK(!two.add(ft::Font()));

    // fallback chains: variants of an external face copy its chain; the family's chain reaches owned faces and variants
    ft::Font base = load("opensans/OpenSans.ttf");
    const ft::Font heb = load("noto/NotoSansHebrew-Regular.ttf");
    base.setFallbacks({&heb});
    ft::FontFamily ext;
    ext.addExternal(&base, 400, false);
    UI_CHECK(ext.name() == "Open Sans");
    const ft::Font* eb = ext.resolve(700, false);
    UI_CHECK(eb != &base && eb->faceCount() == 2 && &eb->face(1) == &heb);
    UI_CHECK(eb->faceFor(0x05D0) == 1);
    ft::FontFamily owned = ft::FontFamily::bundled();
    const ft::Font* ob = owned.resolve(700, false);
    UI_CHECK(ob->faceCount() == 1);
    owned.setFallbacks({&heb});
    UI_CHECK(ob->faceCount() == 2 && owned.nearestReal(400, false)->faceCount() == 2);
    UI_CHECK(owned.resolve(400, true)->faceCount() == 2);

    ft::FontFamily none;
    UI_CHECK(none.empty() && none.match(700, true).font == nullptr && none.nearestReal(400, false) == nullptr);
}

// ---- installed fonts -----------------------------------------------------------------------------------------------
struct TempFonts {
    fs::path dir, empty;
    TempFonts() {
        std::error_code ec;
        dir = fs::temp_directory_path(ec) / "fuse_text_font_styles_gate";
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "vendored", ec);
        fs::copy(kFontDir, dir / "vendored", fs::copy_options::recursive, ec); // opensans/, noto/, twemoji/ + licences
        UI_CHECK(!ec);
        fs::create_directories(dir / "deep" / "er", ec);
        const auto ttc = makeTtc({readFile(kFontDir + "/noto/NotoSansHebrew-Regular.ttf"), readFile(kFontDir + "/opensans/OpenSans.ttf")});
        std::ofstream(dir / "deep" / "er" / "Pair.TTC", std::ios::binary).write(reinterpret_cast<const char*>(ttc.data()), std::streamsize(ttc.size()));
        std::ofstream(dir / "broken.ttf", std::ios::binary) << "this is not a font file, only its extension says so";
        auto cut = readFile(kFontDir + "/opensans/OpenSans.ttf");
        cut.resize(200); // a truncated download
        std::ofstream(dir / "truncated.otf", std::ios::binary).write(reinterpret_cast<const char*>(cut.data()), std::streamsize(cut.size()));
        std::ofstream(dir / "readme.txt") << "fonts live here";
        empty = dir / "none";
        fs::create_directories(empty, ec);
    }
    ~TempFonts() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

void gateSystemFonts(const TempFonts& tmp) {
    // the platform list is never empty, and scanning it (whatever this machine has, possibly nothing) is harmless
    UI_CHECK(!ft::systemFontDirectories().empty());
    {
        std::vector<ft::SystemFontInfo> real;
        for (const std::string& d : ft::systemFontDirectories()) ft::scanFontDirectory(d, real);
        std::printf("this machine: %zu installed faces in %zu directories\n", real.size(), ft::systemFontDirectories().size());
        for (const auto& f : real) UI_CHECK(!f.family.empty() && !f.path.empty() && f.weight >= 1 && f.weight <= 1000);
    }

    // a directory that does not exist, an empty one, a corrupt file
    std::vector<ft::SystemFontInfo> out;
    ft::scanFontDirectory((tmp.dir / "missing").string(), out);
    ft::scanFontDirectory(tmp.empty.string(), out);
    UI_CHECK(out.empty());
    UI_CHECK(!ft::describeFontFile((tmp.dir / "broken.ttf").string(), out));
    UI_CHECK(!ft::describeFontFile((tmp.dir / "truncated.otf").string(), out));
    UI_CHECK(!ft::describeFontFile((tmp.dir / "nope.ttf").string(), out));
    UI_CHECK(out.empty());

    // one file, then the collection
    UI_CHECK(ft::describeFontFile((tmp.dir / "vendored" / "opensans" / "OpenSans.ttf").string(), out));
    UI_CHECK_EQ(out.size(), std::size_t(1));
    UI_CHECK(out[0].family == "Open Sans" && out[0].weight == 400 && !out[0].italic && out[0].faceIndex == 0);
    out.clear();
    const std::string ttc = (tmp.dir / "deep" / "er" / "Pair.TTC").string();
    UI_CHECK(ft::describeFontFile(ttc, out));
    UI_CHECK_EQ(out.size(), std::size_t(2));
    UI_CHECK(out[0].family == "Noto Sans Hebrew" && out[0].faceIndex == 0);
    UI_CHECK(out[1].family == "Open Sans" && out[1].faceIndex == 1);
    {
        // both faces of the collection load and are the right fonts
        auto f0 = ft::Font::fromFile(ttc, 0), f1 = ft::Font::fromFile(ttc, 1);
        UI_CHECK(f0 && f1);
        UI_CHECK(f0 && f0->familyName() == "Noto Sans Hebrew" && f0->glyphIndex(0x05D0) != 0 && f0->glyphIndex(U'A') == 0);
        UI_CHECK(f1 && f1->familyName() == "Open Sans" && f1->glyphIndex(U'A') != 0);
        UI_CHECK(!ft::Font::fromFile(ttc, 2));
        if (f1) { // the collection's Open Sans draws the same 'A' as the plain file
            const Bitmap a = rasterise(font(), U'A', 32.f), b = rasterise(*f1, U'A', 32.f);
            UI_CHECK(a.w == b.w && a.h == b.h && a.px == b.px);
        }
    }

    // the whole tree: 6 vendored files + 2 collection faces; the corrupt and non-font files are skipped
    out.clear();
    ft::scanFontDirectory(tmp.dir.string(), out);
    UI_CHECK_EQ(out.size(), std::size_t(8));
    int openSans = 0;
    for (const auto& f : out) openSans += f.family == "Open Sans";
    UI_CHECK_EQ(openSans, 2);

    // the cached system list over that tree
    ft::setSystemFontDirectories({tmp.empty.string(), tmp.dir.string(), (tmp.dir / "missing").string()});
    const auto fonts = ft::systemFonts();
    UI_CHECK_EQ(fonts.size(), std::size_t(8));
    UI_CHECK(ft::systemFonts().data() == fonts.data()); // cached
    for (std::size_t k = 1; k < fonts.size(); ++k) { // sorted by family, case-insensitively
        std::string a = fonts[k - 1].family, b = fonts[k].family;
        for (char& c : a) c = char(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : b) c = char(std::tolower(static_cast<unsigned char>(c)));
        UI_CHECK(a <= b);
    }
    const auto families = ft::systemFontFamilies();
    UI_CHECK_EQ(families.size(), std::size_t(6));
    UI_CHECK(std::find(families.begin(), families.end(), "Open Sans") != families.end());
    UI_CHECK(std::find(families.begin(), families.end(), "Noto Sans Hebrew") != families.end());
    UI_CHECK(std::find(families.begin(), families.end(), "Noto Sans Arabic") != families.end());
    UI_CHECK(std::find(families.begin(), families.end(), "Noto Sans Devanagari") != families.end());

    const ft::SystemFontInfo* os = ft::findSystemFont("open sans");
    UI_CHECK(os && os->family == "Open Sans" && os->weight == 400);
    UI_CHECK(ft::findSystemFont(" 'Open Sans' ", 900, true) != nullptr); // nearest face of the family
    UI_CHECK(ft::findSystemFont("Open") == nullptr && ft::findSystemFont("No Such Family") == nullptr);
    UI_CHECK(ft::findSystemFontByHash(ft::familyHash("NOTO SANS HEBREW")) != nullptr);
    UI_CHECK(ft::findSystemFontByHash(ft::familyHash("nothing")) == nullptr);

    auto sys = ft::Font::fromSystem("Noto Sans Hebrew", 700, true);
    UI_CHECK(sys.has_value() && sys->familyName() == "Noto Sans Hebrew" && sys->syntheticEmbolden() == 0.f);
    UI_CHECK(!ft::Font::fromSystem("No Such Family").has_value());
    auto fam = ft::FontFamily::fromSystem("OPEN SANS");
    UI_CHECK(fam.has_value() && fam->name() == "Open Sans" && fam->realFaceCount() == 1); // the duplicate is one style
    UI_CHECK(fam && fam->match(700, true).syntheticBold && fam->match(700, true).syntheticOblique);
    UI_CHECK(!ft::FontFamily::fromSystem("No Such Family").has_value());

    // a machine with no fonts at all
    ft::setSystemFontDirectories({tmp.empty.string()});
    UI_CHECK(ft::systemFonts().empty() && ft::systemFontFamilies().empty());
    UI_CHECK(ft::findSystemFont("Open Sans") == nullptr);
    UI_CHECK(!ft::Font::fromSystem("Open Sans").has_value());
    UI_CHECK(fonts.size() == 8 && fonts[0].family.size() > 0); // a span taken before the rescan is still readable
    ft::refreshSystemFonts();
    UI_CHECK(ft::systemFonts().empty());
}

// ---- layout: letter spacing, line height ---------------------------------------------------------------------------
void gateLayout() {
    const ft::Font& f = font();
    ft::TextLayout a, b;
    ft::TextLayoutDesc d;
    d.pixelSize = 20.f;
    ft::layoutText(f, ft::defaultShaper(), "spacing", d, a);
    UI_CHECK(a.font == &f);
    d.letterSpacing = 3.f;
    ft::layoutText(f, ft::defaultShaper(), "spacing", d, b);
    UI_CHECK(near(b.width, a.width + 7.f * 3.f, 1e-2));
    UI_CHECK(near(b.glyphs[3].x, a.glyphs[3].x + 9.f, 1e-2));
    d.maxWidth = a.width + 2.f; // the wider text no longer fits on one line: spacing takes part in wrapping
    ft::layoutText(f, ft::defaultShaper(), "spac ing", d, b);
    UI_CHECK(b.lines.size() == 2);

    ft::TextLayoutDesc h;
    h.pixelSize = 20.f;
    ft::layoutText(f, ft::defaultShaper(), "one\ntwo\nthree", h, a);
    const float natural = std::ceil(a.metrics.lineHeight());
    UI_CHECK(a.height == 3.f * natural && a.lines[1].baseline - a.lines[0].baseline == natural);
    h.lineHeight = 40.f;
    ft::layoutText(f, ft::defaultShaper(), "one\ntwo\nthree", h, b);
    UI_CHECK(b.height == 120.f && b.lines[1].baseline - b.lines[0].baseline == 40.f);
    UI_CHECK(b.lines[0].baseline == a.lines[0].baseline + std::floor((40.f - natural) * 0.5f)); // half-leading
    h.lineHeight = natural;
    ft::layoutText(f, ft::defaultShaper(), "one\ntwo\nthree", h, b);
    UI_CHECK(b.height == a.height && b.lines[2].baseline == a.lines[2].baseline);
}

// ---- CSS -----------------------------------------------------------------------------------------------------------
int countRects(const DrawList& dl, u32 node) {
    int n = 0;
    for (const DrawCmd& c : dl.cmds()) n += c.op == DrawOp::Rect && c.node == node;
    return n;
}
double glyphArea(const DrawList& dl, u32 node) {
    double a = 0;
    for (const DrawCmd& c : dl.cmds())
        if (c.op == DrawOp::Glyph && c.node == node) a += double(c.tw) * double(c.th);
    return a;
}
/// Sum of atlas coverage over a node's glyph commands: the ink the node's text puts on screen.
double glyphInk(UiContext& ui, u32 node) {
    double s = 0;
    for (const DrawCmd& c : ui.drawList().cmds()) {
        if (c.op != DrawOp::Glyph || c.node != node) continue;
        for (int y = 0; y < c.th; ++y)
            for (int x = 0; x < c.tw; ++x) s += ui.atlas().pixels()[std::size_t(c.v + y) * std::size_t(ui.atlas().size()) + c.u + x];
    }
    return s;
}

void gateCss(const TempFonts& tmp) {
    // parsing
    PropValue v;
    UI_CHECK(parsePropValue(Prop::FontWeight, "bold", v) && v.num == 700.f);
    UI_CHECK(parsePropValue(Prop::FontWeight, "normal", v) && v.num == 400.f);
    UI_CHECK(parsePropValue(Prop::FontWeight, "650", v) && v.num == 650.f);
    UI_CHECK(!parsePropValue(Prop::FontWeight, "heavy", v) && !parsePropValue(Prop::FontWeight, "0", v) &&
             !parsePropValue(Prop::FontWeight, "1200", v));
    UI_CHECK(parsePropValue(Prop::FontStyle, "italic", v) && v.e == 1);
    UI_CHECK(parsePropValue(Prop::FontStyle, "oblique", v) && v.e == 1);
    UI_CHECK(parsePropValue(Prop::FontStyle, "normal", v) && v.e == 0);
    UI_CHECK(!parsePropValue(Prop::FontStyle, "slanted", v));
    UI_CHECK(parsePropValue(Prop::TextDecoration, "underline", v) && v.e == kTextUnderline);
    UI_CHECK(parsePropValue(Prop::TextDecoration, "underline  line-through", v) && v.e == (kTextUnderline | kTextLineThrough));
    UI_CHECK(parsePropValue(Prop::TextDecoration, "none", v) && v.e == 0);
    UI_CHECK(!parsePropValue(Prop::TextDecoration, "wavy", v));
    UI_CHECK(parsePropValue(Prop::LineHeight, "1.5", v) && v.len == Length::pct(150.f));
    UI_CHECK(parsePropValue(Prop::LineHeight, "24px", v) && v.len == Length::px(24.f));
    UI_CHECK(parsePropValue(Prop::LineHeight, "normal", v) && v.len.isAuto());
    UI_CHECK(parsePropValue(Prop::LetterSpacing, "2px", v) && v.len == Length::px(2.f));
    UI_CHECK(parsePropValue(Prop::FontFamily, "\"Segoe UI\", Arial , sans-serif", v) && v.trackCount == 3);
    UI_CHECK(u32(v.tracks[0].value) == ft::familyHash("segoe ui") && u32(v.tracks[1].value) == ft::familyHash("Arial"));
    UI_CHECK(!parsePropValue(Prop::FontFamily, " , ", v));
    Prop p;
    UI_CHECK(findProp("font-weight", p) && p == Prop::FontWeight && propInfo(p).inherited);
    UI_CHECK(findProp("text-decoration", p) && !propInfo(p).inherited);
    std::vector<StyleDiagnostic> diags;
    auto sheet = StyleSheet::parse(".strong { font-weight: bold; font-style: italic; font-family: \"Test Hebrew\", sans-serif; "
                                   "text-decoration: underline; letter-spacing: 1px; line-height: 2; }", &diags);
    UI_CHECK(sheet.has_value() && diags.empty());

    ft::setSystemFontDirectories({tmp.dir.string()});
    UiContext ui({.viewport = {640, 480}, .font = &font()});
    w::install(ui);
    if (sheet) ui.styles().addSheet(std::move(*sheet));
    ui.setCss(ui.root(), "align-items", "flex-start");
    const char* text = "Hamburgefonstiv";
    const NodeHandle reg = w::label(ui, ui.root(), text, "reg");
    const NodeHandle bold = w::label(ui, ui.root(), text, "bold");
    const NodeHandle ital = w::label(ui, ui.root(), text, "ital");
    const NodeHandle deco = w::label(ui, ui.root(), text, "deco");
    const NodeHandle wide = w::label(ui, ui.root(), text, "wide");
    const NodeHandle tall = w::label(ui, ui.root(), "two\nlines", "tall");
    const NodeHandle box = w::panel(ui, ui.root(), "box");
    const NodeHandle child = w::label(ui, box, text, "child");
    const NodeHandle b1 = w::button(ui, ui.root(), "Apply", "b1");
    const NodeHandle b2 = w::button(ui, ui.root(), "Apply", "b2");
    ui.setCss(bold, "font-weight", "bold");
    ui.setCss(ital, "font-style", "italic");
    ui.setCss(deco, "text-decoration", "underline line-through");
    ui.setCss(deco, "color", "#ffffff");
    ui.setCss(wide, "letter-spacing", "2px");
    ui.setCss(tall, "line-height", "30px");
    ui.setCss(box, "font-weight", "700");
    ui.setCss(box, "font-style", "italic");
    ui.setCss(b2, "font-weight", "bold");
    ui.update();

    // weight and slant reach the layout font; regular text is the context's font itself
    UI_CHECK(ui.textLayout(reg)->font == &font());
    UI_CHECK(ui.nodeFont(reg) == &font());
    const ft::Font* bf = ui.textLayout(bold)->font;
    UI_CHECK(bf != &font() && bf->syntheticEmbolden() == ft::kBoldEmbolden && bf->syntheticSlant() == 0.f);
    UI_CHECK(ui.nodeFont(bold) == bf && ui.fontFor("", 700, false) == bf);
    UI_CHECK(ui.computed(bold).fontWeight == 700 && !ui.computed(bold).fontItalic);
    UI_CHECK(ui.rect(bold).w > ui.rect(reg).w + 4.f); // 15 advancing glyphs, each wider
    const ft::Font* itf = ui.textLayout(ital)->font;
    UI_CHECK(itf->syntheticSlant() == ft::kObliqueSlant && itf->syntheticEmbolden() == 0.f);
    UI_CHECK(ui.rect(ital).w == ui.rect(reg).w);
    UI_CHECK(glyphInk(ui, bold.index) > glyphInk(ui, reg.index) * 1.15);
    UI_CHECK(glyphArea(ui.drawList(), ital.index) > glyphArea(ui.drawList(), reg.index));
    // inherited from the panel
    UI_CHECK(ui.computed(child).fontWeight == 700 && ui.computed(child).fontItalic);
    const ft::Font* cf = ui.textLayout(child)->font;
    UI_CHECK(cf->syntheticEmbolden() > 0.f && cf->syntheticSlant() > 0.f);
    // widgets that measure and paint their own strings (buttons) follow the node's font
    UI_CHECK(ui.rect(b2).w > ui.rect(b1).w);
    UI_CHECK(glyphInk(ui, b2.index) > glyphInk(ui, b1.index) * 1.15);

    // decorations: two rules across the text, below the baseline and through the x-height
    UI_CHECK_EQ(countRects(ui.drawList(), reg.index), 0);
    UI_CHECK_EQ(countRects(ui.drawList(), deco.index), 2);
    {
        const auto* tl = ui.textLayout(deco);
        const Rect r = ui.contentRect(deco);
        const f32 baseline = r.y + std::floor(std::max(0.f, r.h - tl->height) * 0.5f) + tl->lines[0].baseline;
        const auto dm = font().decorationMetrics(ui.computed(deco).fontSize);
        int seen = 0;
        for (const DrawCmd& c : ui.drawList().cmds()) {
            if (c.op != DrawOp::Rect || c.node != deco.index) continue;
            UI_CHECK(c.rect.x == std::round(r.x) && c.rect.w == std::ceil(tl->width));
            if (seen == 0) UI_CHECK(c.rect.y == std::round(baseline) + dm.underlineOffset && c.rect.h == dm.underlineThickness);
            else UI_CHECK(c.rect.y == std::round(baseline) + dm.strikeOffset && c.rect.h == dm.strikeThickness);
            ++seen;
        }
        Image img;
        render(ui, img);
        const int uy = int(std::round(baseline) + dm.underlineOffset), x0 = int(std::round(r.x)), x1 = x0 + int(std::ceil(tl->width));
        int lit = 0;
        for (int x = x0; x < x1; ++x) lit += (img.at(x, uy) & 0xFFu) > 200u;
        UI_CHECK_EQ(lit, x1 - x0); // an unbroken white rule
        UI_CHECK((img.at(x0 - 2, uy) & 0xFFu) < 100u && (img.at(x1 + 2, uy) & 0xFFu) < 100u);
    }
    // letter spacing and line height
    UI_CHECK(near(ui.rect(wide).w, ui.rect(reg).w + 15.f * 2.f, 1.01));
    UI_CHECK(ui.textLayout(tall)->height == 60.f && ui.rect(tall).h >= 60.f);
    UI_CHECK(ui.computed(tall).lineHeightPx() == 30.f);
    ui.setCss(tall, "line-height", "2");
    ui.update();
    UI_CHECK(ui.computed(tall).lineHeightPx() == 2.f * ui.computed(tall).fontSize);
    UI_CHECK(ui.textLayout(tall)->height == 2.f * std::round(2.f * ui.computed(tall).fontSize));

    // switching the weight back relays out with the regular face
    ui.setCss(bold, "font-weight", "normal");
    ui.update();
    UI_CHECK(ui.textLayout(bold)->font == &font() && ui.rect(bold).w == ui.rect(reg).w);
    ui.setCss(bold, "font-weight", "bold");
    ui.update();
    UI_CHECK(ui.textLayout(bold)->font == bf);

    // families: registered, installed, unknown
    ft::FontFamily hebrew("Test Hebrew");
    const ft::Font* hreal = hebrew.add(load("noto/NotoSansHebrew-Regular.ttf"));
    ui.addFontFamily(hebrew);
    const char* shalom = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";
    const NodeHandle h1 = w::label(ui, ui.root(), shalom, "h1");
    const NodeHandle h2 = w::label(ui, ui.root(), shalom, "h2");
    const NodeHandle h3 = w::label(ui, ui.root(), text, "h3");
    const NodeHandle h4 = w::label(ui, ui.root(), "\xD0\x96 \xE0\xA4\x95", "h4"); // Cyrillic Zhe, Devanagari Ka
    const NodeHandle h5 = w::label(ui, ui.root(), text, "h5");
    ui.setCss(h1, "font-family", "\"Test Hebrew\", sans-serif");
    ui.addClass(h2, "strong"); // the sheet: Test Hebrew, bold italic, underline, spacing, line-height 2
    ui.setCss(h3, "font-family", "No Such Family, 'Another Missing One'");
    ui.setCss(h4, "font-family", "noto sans devanagari");
    ui.setCss(h5, "font-family", "Missing, sans-serif");
    ui.update();
    UI_CHECK(ui.textLayout(h1)->font == hreal);
    const ft::Font* h2f = ui.textLayout(h2)->font;
    UI_CHECK(h2f != hreal && h2f->familyName() == "Noto Sans Hebrew" && h2f->syntheticEmbolden() > 0.f && h2f->syntheticSlant() > 0.f);
    UI_CHECK(ui.computed(h2).textDecoration == kTextUnderline && ui.computed(h2).letterSpacing == 1.f);
    UI_CHECK_EQ(countRects(ui.drawList(), h2.index), 1);
    UI_CHECK(ui.textLayout(h2)->height == std::round(2.f * ui.computed(h2).fontSize));
    UI_CHECK(ui.textLayout(h3)->font == &font()); // nothing matches: the context's font
    UI_CHECK(ui.textLayout(h5)->font == &font());
    const ft::Font* dev = ui.textLayout(h4)->font; // found among the installed fonts (the temp directory)
    UI_CHECK(dev != &font() && dev->familyName() == "Noto Sans Devanagari");
    UI_CHECK(ui.fontFor("Noto Sans Devanagari", 400, false) == dev);
    UI_CHECK(dev->faceCount() >= 2 && &dev->face(1) == &font()); // what it lacks falls back to the context's font
    UI_CHECK(dev->glyphIndex(0x0416) == 0 && font().glyphIndex(0x0416) != 0);
    {
        bool cyrillicFallback = false, nativeDeva = false;
        for (const auto& g : ui.textLayout(h4)->glyphs) {
            cyrillicFallback |= g.cluster == 0 && g.face == 1;
            nativeDeva |= g.cluster == 3 && g.face == 0;
        }
        UI_CHECK(cyrillicFallback && nativeDeva);
    }
    // installed fonts off: the same name falls back
    UiContext off({.viewport = {320, 200}, .font = &font()});
    off.setSystemFontLookup(false);
    UI_CHECK(off.fontFor("Noto Sans Devanagari", 400, false) == &font());
    UI_CHECK(off.fontFor("", 400, false) == &font());
    // a context without a font
    UiContext bare({.viewport = {320, 200}});
    UI_CHECK(bare.fontFor("", 700, true) == nullptr);
    const NodeHandle bl = w::label(bare, bare.root(), "x");
    bare.setCss(bl, "font-weight", "bold");
    bare.setCss(bl, "text-decoration", "underline");
    bare.update();
    UI_CHECK(bare.drawList().size() == 0 || countRects(bare.drawList(), bl.index) == 0);

    // font picker: the installed families follow the bundled / generic ones; the preview takes the chosen face
    {
        const NodeHandle fp = w::fontPicker(ui, ui.root(), {}, 16.f, "fonts");
        ui.update();
        const NodeHandle list = ui.firstChild(fp);
        const NodeHandle preview = ui.nextSibling(ui.nextSibling(list));
        UI_CHECK(w::listItem(ui, list, 0) == "Open Sans" && w::listItem(ui, list, 4) == "System UI");
        u32 devRow = 0, rows = 0;
        for (; rows < 64 && !w::listItem(ui, list, rows).empty(); ++rows)
            if (w::listItem(ui, list, rows) == "Noto Sans Devanagari") devRow = rows;
        UI_CHECK_EQ(rows, 5u + 5u); // 5 built in + the 6 scanned families less Open Sans
        UI_CHECK(devRow >= 5);
        UI_CHECK(ui.textLayout(preview)->font == &font());
        UI_CHECK(!w::fontPickerBold(ui, fp) && !w::fontPickerItalic(ui, fp));
        std::string fam;
        int emitted = 0;
        ui.connect(fp, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) { fam = a.text; ++emitted; });
        ui.setValue(list, f32(devRow));
        ui.emit(list, SignalKind::ValueChanged, {.value = f32(devRow)});
        ui.update();
        UI_CHECK(fam == "Noto Sans Devanagari" && w::fontPickerFamily(ui, fp) == fam);
        UI_CHECK(ui.textLayout(preview)->font == dev);
        w::fontPickerSetStyle(ui, fp, true, true);
        ui.update();
        UI_CHECK(emitted == 2 && w::fontPickerBold(ui, fp) && w::fontPickerItalic(ui, fp));
        const ft::Font* pf = ui.textLayout(preview)->font;
        UI_CHECK(pf != dev && pf->familyName() == "Noto Sans Devanagari" && pf->syntheticEmbolden() > 0.f && pf->syntheticSlant() > 0.f);
        // an explicit list is used as given
        const NodeHandle fixed = w::fontPicker(ui, ui.root(), "Alpha\nBeta", 12.f, "fixed");
        UI_CHECK(w::listItem(ui, ui.firstChild(fixed), 1) == "Beta" && w::listItem(ui, ui.firstChild(fixed), 2).empty());
    }
    ft::setSystemFontDirectories({tmp.empty.string()});
}

// ---- render scale --------------------------------------------------------------------------------------------------
void gateRenderScale() {
    auto build = [](UiContext& ui, NodeHandle& reg, NodeHandle& bold, NodeHandle& ital) {
        w::install(ui);
        ui.setCss(ui.root(), "align-items", "flex-start");
        reg = w::label(ui, ui.root(), "Hill", "reg");
        bold = w::label(ui, ui.root(), "Hill", "bold");
        ital = w::label(ui, ui.root(), "Hill", "ital");
        ui.setCss(bold, "font-weight", "bold");
        ui.setCss(bold, "text-decoration", "underline");
        ui.setCss(ital, "font-style", "italic");
        for (NodeHandle n : {reg, bold, ital}) ui.setCss(n, "font-size", "24px");
    };
    UiContext one({.viewport = {200, 120}, .font = &font()}), two({.viewport = {200, 120}, .font = &font()});
    NodeHandle r1, b1, i1, r2, b2, i2;
    build(one, r1, b1, i1);
    build(two, r2, b2, i2);
    two.setRenderScale(2.f);
    one.update();
    two.update();
    // logical layout is the same; the glyph quads are the device-size ones
    UI_CHECK(one.rect(b1).w == two.rect(b2).w && one.rect(i1).x == two.rect(i2).x);
    UI_CHECK(glyphArea(two.drawList(), b2.index) > glyphArea(one.drawList(), b1.index) * 3.0);
    UI_CHECK(glyphArea(two.drawList(), i2.index) > glyphArea(one.drawList(), i1.index) * 3.0);
    // synthetic styles at the device size: the bold has more ink than the regular, about 4x its scale-1 ink
    const double inkR2 = glyphInk(two, r2.index), inkB2 = glyphInk(two, b2.index), inkB1 = glyphInk(one, b1.index);
    UI_CHECK(inkB2 > inkR2 * 1.15);
    UI_CHECK(inkB2 > inkB1 * 3.4 && inkB2 < inkB1 * 4.6);
    // the oblique at scale 2 leans twice as many pixels: its quads are wider than the regular's by more than at scale 1
    auto widthSum = [](const DrawList& dl, u32 node) {
        double s = 0;
        for (const DrawCmd& c : dl.cmds())
            if (c.op == DrawOp::Glyph && c.node == node) s += c.tw;
        return s;
    };
    const double lean1 = widthSum(one.drawList(), i1.index) - widthSum(one.drawList(), r1.index);
    const double lean2 = widthSum(two.drawList(), i2.index) - widthSum(two.drawList(), r2.index);
    UI_CHECK(lean1 > 4.0 && lean2 > lean1 + 3.0);
    // the underline is a device-pixel rect twice as wide
    f32 w1 = 0.f, w2 = 0.f;
    for (const DrawCmd& c : one.drawList().cmds()) if (c.op == DrawOp::Rect && c.node == b1.index) w1 = c.rect.w;
    for (const DrawCmd& c : two.drawList().cmds()) if (c.op == DrawOp::Rect && c.node == b2.index) w2 = c.rect.w;
    UI_CHECK(w1 > 0.f && w2 == 2.f * w1);
    // and the frame rasterises: the bold line has ink in the 2x image
    Image img;
    img.resize(400, 240, 0xFF000000u);
    CpuRasterizer r;
    r.setGlyphAtlas(two.atlas().pixels(), two.atlas().size());
    r.render(two.drawList(), img);
    const Rect br = two.rect(b2);
    int lit = 0;
    for (int y = int(br.y * 2); y < int(br.bottom() * 2); ++y)
        for (int x = int(br.x * 2); x < int(br.right() * 2); ++x) lit += (img.at(x, y) & 0xFFu) > 128u;
    UI_CHECK(lit > 200);
}

// ---- golden --------------------------------------------------------------------------------------------------------
u8 ch(u32 p, int c) { return u8((p >> (8 * c)) & 0xFF); }
void gateGolden(const char* out) {
    UiContext ui({.viewport = {560, 430}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "14px");
    ui.setCss(ui.root(), "gap", "6px");
    ui.setCss(ui.root(), "align-items", "flex-start");
    auto line = [&](const char* text, std::initializer_list<std::pair<const char*, const char*>> css) {
        const NodeHandle l = w::label(ui, ui.root(), text);
        ui.setCss(l, "font-size", "18px");
        for (const auto& [k, v] : css) ui.setCss(l, k, v);
        return l;
    };
    line("Regular: The quick brown fox jumps over the lazy dog", {});
    line("Bold: The quick brown fox jumps over the lazy dog", {{"font-weight", "bold"}});
    line("Italic: The quick brown fox jumps over the lazy dog", {{"font-style", "italic"}});
    line("Bold italic: The quick brown fox jumps over the lazy dog", {{"font-weight", "bold"}, {"font-style", "italic"}});
    line("Underline: The quick brown fox jumps over the lazy dog", {{"text-decoration", "underline"}});
    line("Strike-through: The quick brown fox jumps over the lazy dog", {{"text-decoration", "line-through"}});
    line("Both, bold italic, orange", {{"text-decoration", "underline line-through"}, {"font-weight", "bold"}, {"font-style", "italic"}, {"color", "#ff9a3c"}});
    const NodeHandle row = w::panel(ui, ui.root());
    ui.addClass(row, "row");
    for (const char* wgt : {"300", "400", "600", "700", "900"}) {
        const NodeHandle l = w::label(ui, row, (std::string("Weight ") + wgt).c_str());
        ui.setCss(l, "font-weight", wgt);
    }
    line("Letter spacing 3px", {{"letter-spacing", "3px"}, {"font-size", "14px"}});
    const NodeHandle two = line("Line height 2:\nsecond line", {{"line-height", "2"}, {"font-size", "14px"}});
    ui.setCss(two, "background", "#2a2c36");
    const NodeHandle btns = w::panel(ui, ui.root());
    ui.addClass(btns, "row");
    w::button(ui, btns, "Regular");
    ui.setCss(w::button(ui, btns, "Bold"), "font-weight", "bold");
    ui.setCss(w::button(ui, btns, "Italic"), "font-style", "italic");
    ui.setCss(w::button(ui, btns, "Underlined"), "text-decoration", "underline");
    const NodeHandle rich = w::richText(ui, ui.root(), "Rich text: <b>bold</b>, <i>italic</i>, <b><i>both</i></b>, <u>underlined</u>, <s>struck</s>, "
                                                       "<size=20><b>big bold</b></size>.");
    ui.setCss(rich, "width", "520px");
    const NodeHandle md = w::markdownView(ui, ui.root(), "## Markdown heading\nA paragraph with **bold**, *italic* and __more bold__ words, "
                                                         "and snake_case_names left alone.\n- an item with *emphasis*");
    ui.setCss(md, "width", "520px");
    ui.update();
    Image img;
    render(ui, img);
    if (out) stbi_write_png(out, img.width, img.height, 4, img.pixels.data(), img.width * 4);

    // the markdown blocks: heading label (bold), rich paragraph, rich list item; node text is the plain text
    const NodeHandle mh = ui.firstChild(md), mp = ui.nextSibling(mh), ml = ui.nextSibling(mp);
    UI_CHECK(ui.hasClass(mh, "md-h2") && ui.computed(mh).fontWeight == 700);
    UI_CHECK(ui.textLayout(mh)->font->syntheticEmbolden() > 0.f);
    UI_CHECK(ui.hasClass(mp, "md-p") && ui.hasClass(ml, "md-li"));
    UI_CHECK(ui.text(mp) == "A paragraph with bold, italic and more bold words, and snake_case_names left alone.");
    UI_CHECK(w::richTextPlain(ui, mp) == ui.text(mp));
    UI_CHECK(w::richTextPlain(ui, rich) == "Rich text: bold, italic, both, underlined, struck, big bold.");

#if FUSE_UI_TEST_TEXT_FULL
    const char* golden = "golden/text_styles_full.png"; // FreeType coverage differs from stb_truetype
#else
    const char* golden = "golden/text_styles.png";
#endif
    if (std::getenv("FUSE_UI_UPDATE_GOLDEN")) {
        stbi_write_png(golden, img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("golden written: %s\n", golden);
        return;
    }
    int gw = 0, gh = 0, n = 0;
    unsigned char* ref = stbi_load(golden, &gw, &gh, &n, 4);
    UI_CHECK(ref != nullptr);
    if (!ref) {
        std::fprintf(stderr, "missing golden %s (run from the tests directory; FUSE_UI_UPDATE_GOLDEN=1 writes it)\n", golden);
        return;
    }
    UI_CHECK_EQ(gw, img.width);
    UI_CHECK_EQ(gh, img.height);
    if (gw == img.width && gh == img.height) {
        int bad = 0, maxd = 0;
        for (int i = 0; i < gw * gh; ++i)
            for (int c = 0; c < 4; ++c) {
                const int d = std::abs(int(ref[i * 4 + c]) - int(ch(img.pixels[std::size_t(i)], c)));
                maxd = std::max(maxd, d);
                if (d > 3) { ++bad; break; }
            }
        std::printf("text_styles golden diff: %d pixels > 3, max channel diff %d\n", bad, maxd);
        UI_CHECK(bad <= gw * gh / 1000); // <= 0.1% of pixels may differ (compiler / libm rounding)
    }
    stbi_image_free(ref);
}
} // namespace

int main(int argc, char** argv) {
    const char* out = nullptr;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--out=", 6) == 0) out = argv[i] + 6;
    const TempFonts tmp;
    gateFaces();
    gateSynthesis();
    gateFamily();
    gateSystemFonts(tmp);
    gateLayout();
    gateCss(tmp);
    gateRenderScale();
    gateGolden(out);
    return finish("fuse_text_font_styles");
}
