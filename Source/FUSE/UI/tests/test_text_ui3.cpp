// UI3 text gates (one binary per gate, FUSE_TEXT_GATE picks it):
//   shaping_scripts  FULL: Latin, Cyrillic, Greek, Arabic, Hebrew, Devanagari, Thai, CJK and emoji lines through the
//                    bundled fallback chain: shaping checks per script (ligatures, Arabic joining, RTL order, Indic
//                    reordering, Thai marks, CJK em advances, ZWJ emoji) and golden/shaping_scripts_full.png
//                    (regenerate with FUSE_UI_UPDATE_GOLDEN=1). MINIMAL skips with 77 (no complex shaping).
//   system_fonts     FontCatalog::system(): Segoe UI / Arial on Windows, weight / italic matching, generic families,
//                    per-script fallback chains, loading a face, fontPicker("@system"). 77 when no fonts are installed.
//   cache_zero_alloc steady-state shaping, layout (wrapping, alignment, eliding, fallback runs, RTL), measuring,
//                    atlas lookups and segmentation of already-seen strings perform no heap allocation.
//   dpi_<scale>      bundled font + fallback chain at 14 px x scale: metrics, advances and ink scale with the DPR,
//                    line breaks are scale-invariant, no missing glyphs (twins of fuse_editor_qt_font_*).
// Optional --out=<dir> writes the rendered images.
#include "test_util.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>
#include <string>

namespace {
std::atomic<long> g_allocs{0};
bool g_counting = false;
} // namespace

void* operator new(std::size_t n) {
    if (g_counting) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace fuse::ui;
using namespace fuse::text;
namespace w = fuse::ui::widgets;
using fuse::ui::test::finish;

namespace {
std::string g_out;

std::string utf8(std::u32string_view cps) {
    std::string s;
    for (char32_t c : cps) appendUtf8(s, c);
    return s;
}

/// The bundled font with its whole fallback chain attached.
struct ChainFont {
    Font font;
    std::vector<Font> fallbacks;
    ChainFont() {
        auto f = Font::bundled();
        if (!f) {
            std::fprintf(stderr, "bundled font missing\n");
            std::exit(2);
        }
        font = std::move(*f);
        fallbacks = Font::bundledFallbacks();
        std::vector<const Font*> chain;
        for (const Font& x : fallbacks) chain.push_back(&x);
        font.setFallbacks(chain);
    }
};

int countNotdef(const TextLayout& tl) {
    int n = 0;
    for (const ShapedGlyph& g : tl.glyphs) n += g.glyph == 0;
    return n;
}

bool writePng(const std::string& path, const Image& img) {
    return stbi_write_png(path.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4) != 0;
}

/// Compares `img` with a PNG golden (<= 0.1 % of pixels may differ by more than 3 per channel).
void compareGolden(const Image& img, const std::string& golden) {
    if (std::getenv("FUSE_UI_UPDATE_GOLDEN")) {
        UI_CHECK(writePng(golden, img));
        std::printf("golden written: %s\n", golden.c_str());
        return;
    }
    int w = 0, h = 0, n = 0;
    unsigned char* ref = stbi_load(golden.c_str(), &w, &h, &n, 4);
    UI_CHECK(ref != nullptr);
    if (!ref) return;
    UI_CHECK(w == img.width && h == img.height);
    if (w == img.width && h == img.height) {
        int bad = 0, maxd = 0;
        for (int i = 0; i < w * h; ++i)
            for (int c = 0; c < 4; ++c) {
                const int d = std::abs(int(ref[i * 4 + c]) - int((img.pixels[std::size_t(i)] >> (8 * c)) & 0xFF));
                maxd = std::max(maxd, d);
                if (d > 3) { ++bad; break; }
            }
        std::printf("golden %s: %d pixels > 3, max channel diff %d\n", golden.c_str(), bad, maxd);
        UI_CHECK(bad <= w * h / 1000);
    }
    stbi_image_free(ref);
}

// ---- fuse_text_shaping_scripts -------------------------------------------------------------------------------------
struct ScriptLine {
    const char* name;
    std::u32string text;
    std::uint16_t face; // face the script's letters must come from (bundled chain order)
};

int shapingScripts() {
    if (textProfile() != TextProfile::Full) {
        std::printf("fuse_text_shaping_scripts: MINIMAL has no complex shaping, skipped\n");
        return 77;
    }
    ChainFont cf;
    const Font& font = cf.font;
    UI_CHECK_EQ(font.faceCount(), std::size_t(8));
    const ScriptLine lines[] = {
        {"Latin", U"Office affinity: AVA Wave, fluffy \u00E9t\u00E9 \u2014 caf\u00E9", 0},
        {"Cyrillic", U"\u0421\u044A\u0435\u0448\u044C \u0436\u0435 \u0435\u0449\u0451 \u044D\u0442\u0438\u0445 \u043C\u044F\u0433\u043A\u0438\u0445 \u0431\u0443\u043B\u043E\u043A", 0},
        {"Greek", U"\u039E\u03B5\u03C3\u03BA\u03B5\u03C0\u03AC\u03B6\u03C9 \u03C4\u03B7\u03BD \u03C8\u03C5\u03C7\u03BF\u03C6\u03B8\u03CC\u03C1\u03B1", 0},
        {"Arabic", U"\u0633\u0644\u0627\u0645 \u0639\u0644\u064A\u0643\u0645 \u0648\u0631\u062D\u0645\u0629 \u0627\u0644\u0644\u0647", 2},
        {"Hebrew", U"\u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD, \u05D3\u05D2 \u05E1\u05E7\u05E8\u05DF", 1},
        {"Devanagari", U"\u0928\u092E\u0938\u094D\u0924\u0947 \u0915\u093F \u0915\u094D\u0937\u0924\u094D\u0930\u093F\u092F \u0936\u094D\u0930\u0940", 3},
        {"Thai", U"\u0E40\u0E1B\u0E47\u0E19\u0E21\u0E19\u0E38\u0E29\u0E22\u0E4C\u0E2A\u0E38\u0E14\u0E1B\u0E23\u0E30\u0E40\u0E2A\u0E23\u0E34\u0E10", 6},
        {"CJK", U"\u4E2D\u6587\u5B57\u4F53\uFF0C\u65E5\u672C\u306E\u3072\u3089\u304C\u306A\u3068\u30AB\u30BF\u30AB\u30CA\u3002", 7},
        {"Emoji", U"\U0001F600 \U0001F44D\U0001F3FD \U0001F468\u200D\U0001F469\u200D\U0001F467 \U0001F1EF\U0001F1F5 \u2764\uFE0F", 4},
    };
    const float px = 20.f;
    TextLayout tl;
    TextLayoutDesc d;
    d.pixelSize = px;
    for (const ScriptLine& ln : lines) {
        const std::string s = utf8(ln.text);
        layoutText(font, defaultShaper(), s, d, tl);
        const int notdef = countNotdef(tl);
        if (notdef) std::printf("  %s: %d .notdef glyphs\n", ln.name, notdef);
        UI_CHECK_EQ(notdef, 0);
        UI_CHECK_EQ(tl.lines.size(), std::size_t(1));
        // the script's first letter comes from the expected face
        std::size_t i = 0;
        const char32_t first = decodeUtf8(s, i);
        bool faceOk = false;
        for (const ShapedGlyph& g : tl.glyphs)
            if (g.cluster == 0) faceOk = g.face == ln.face;
        if (!faceOk) std::printf("  %s: U+%04X not on face %u\n", ln.name, unsigned(first), ln.face);
        UI_CHECK(faceOk);
        // visual order: x increases along the line
        // visual order: spacing glyphs advance left to right (marks may sit on their base)
        float lastBase = -1e9f;
        bool monotonic = true;
        for (const ShapedGlyph& x : tl.glyphs) {
            if (x.advance == 0.f) continue;
            if (x.x < lastBase - 1.f) {
                std::printf("  %s: glyph at x %.2f (cluster %u) left of the previous base %.2f\n", ln.name, double(x.x),
                            x.cluster, double(lastBase));
                monotonic = false;
            }
            lastBase = x.x;
        }
        UI_CHECK(monotonic);
    }
    std::vector<ShapedGlyph> g;
    // Latin: "ffi" / "fi" / "fl" ligatures (Open Sans liga) and GPOS kerning (AV closer than A + V advances)
    shapeText(font, "office", px, g);
    UI_CHECK(g.size() < 6);
    shapeText(font, "AV", px, g);
    UI_CHECK(g.size() == 2 && g[1].x < font.advance(font.glyphIndex('A'), px) - 0.1f);
    // Arabic: joining forms differ from the cmap (isolated) glyphs; the line is RTL
    {
        const std::string s = utf8(U"\u0633\u0644\u0627\u0645");
        layoutText(font, defaultShaper(), s, d, tl);
        const Font& ar = font.face(2);
        int joined = 0;
        std::size_t at = 0;
        while (at < s.size()) {
            const std::size_t c = at;
            const std::uint32_t iso = ar.glyphIndex(decodeUtf8(s, at));
            for (const ShapedGlyph& x : tl.glyphs)
                if (x.cluster == c && x.glyph != iso) ++joined;
        }
        UI_CHECK(joined >= 2);
        UI_CHECK(tl.lines[0].rtl);
        // first logical letter (seen) is right-most
        float seenX = -1.f, maxX = -1.f;
        for (const ShapedGlyph& x : tl.glyphs) {
            if (x.cluster == 0) seenX = x.x;
            maxX = std::max(maxX, x.x);
        }
        UI_CHECK(seenX == maxX);
    }
    // Hebrew: RTL, first letter right-most
    {
        layoutText(font, defaultShaper(), utf8(U"\u05E9\u05DC\u05D5\u05DD"), d, tl);
        UI_CHECK(tl.lines[0].rtl && tl.glyphs.size() == 4 && tl.glyphs.back().cluster == 0);
    }
    // Devanagari: the i-matra (U+093F) is drawn before KA though it follows it logically
    {
        layoutText(font, defaultShaper(), utf8(U"\u0915\u093F"), d, tl);
        const Font& dv = font.face(3);
        UI_CHECK(tl.glyphs.size() == 2 && tl.glyphs[0].glyph != dv.glyphIndex(U'\u0915') &&
                 tl.glyphs[1].glyph == dv.glyphIndex(U'\u0915'));
        // KSSA conjunct: KA + VIRAMA + SSA becomes fewer glyphs than code points
        layoutText(font, defaultShaper(), utf8(U"\u0915\u094D\u0937"), d, tl);
        UI_CHECK(tl.glyphs.size() < 3);
    }
    // Thai: above vowels and tone marks are zero-advance marks
    {
        layoutText(font, defaultShaper(), utf8(U"\u0E19\u0E35\u0E49"), d, tl);
        int marks = 0;
        for (const ShapedGlyph& x : tl.glyphs) marks += x.advance == 0.f;
        UI_CHECK(tl.glyphs.size() == 3 && marks == 2);
    }
    // CJK: ideographs advance one em
    {
        layoutText(font, defaultShaper(), utf8(U"\u4E2D\u6587"), d, tl);
        // pixel sizes are ascent-to-descent, so one em is a bit less than px (Open Sans: about 0.73 px)
        bool emAdv = tl.glyphs.size() == 2 && tl.glyphs[0].advance == tl.glyphs[1].advance;
        for (const ShapedGlyph& x : tl.glyphs) emAdv &= x.face == 7 && x.advance > 0.6f * px && x.advance < 1.0f * px;
        UI_CHECK(emAdv);
    }
    // Emoji: a ZWJ family is one colour glyph; skin tones stay on the emoji face
    {
        layoutText(font, defaultShaper(), utf8(U"\U0001F468\u200D\U0001F469\u200D\U0001F467"), d, tl);
        UI_CHECK(tl.glyphs.size() == 1 && !font.face(4).colorLayers(tl.glyphs[0].glyph).empty());
    }

    // golden: one label per script
    UiContext ui{{.viewport = {560, 228}, .font = &font}};
    ui.setCss(ui.root(), "padding", "10px");
    ui.setCss(ui.root(), "gap", "4px");
    for (const ScriptLine& ln : lines) {
        const NodeHandle l = w::label(ui, ui.root(), std::string(ln.name) + ": " + utf8(ln.text));
        ui.setCss(l, "font-size", "18px");
    }
    ui.update();
    Image img;
    fuse::ui::test::render(ui, img);
    if (!g_out.empty()) writePng(g_out + "/shaping_scripts.png", img);
    compareGolden(img, "golden/shaping_scripts_full.png");
    return finish("fuse_text_shaping_scripts");
}

// ---- fuse_text_system_fonts ----------------------------------------------------------------------------------------
int systemFonts() {
    const auto t0 = std::chrono::steady_clock::now();
    const FontCatalog& cat = FontCatalog::system();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const std::vector<std::string> fams = cat.families();
    std::printf("system fonts: %zu faces, %zu families in %.0f ms\n", cat.faces().size(), fams.size(), ms);
    for (const std::string& d : FontCatalog::systemFontDirectories()) std::printf("  dir %s\n", d.c_str());
    if (cat.faces().empty()) {
        std::printf("fuse_text_system_fonts: no fonts installed, skipped\n");
        return 77;
    }
    for (const FontFaceInfo& f : cat.faces()) {
        UI_CHECK(!f.family.empty() && f.weight >= 1 && f.weight <= 1000);
    }
    UI_CHECK(cat.match("No Such Family 123") == nullptr);
#if defined(_WIN32)
    const FontFaceInfo* segoe = cat.match("Segoe UI");
    const FontFaceInfo* arial = cat.match("arial"); // case-insensitive
    UI_CHECK(segoe && arial);
    if (arial) {
        std::printf("  Arial -> %s (%s, %u)\n", arial->path.c_str(), arial->style.c_str(), arial->weight);
        UI_CHECK(arial->weight == 400 && !arial->italic);
        const FontFaceInfo* bold = cat.match("Arial", 700);
        const FontFaceInfo* italic = cat.match("Arial", 400, true);
        const FontFaceInfo* boldItalic = cat.match("Arial", 700, true);
        UI_CHECK(bold && bold->weight == 700 && !bold->italic);
        UI_CHECK(italic && italic->italic && italic->weight == 400);
        UI_CHECK(boldItalic && boldItalic->italic && boldItalic->weight == 700);
        auto f = FontCatalog::load(*arial);
        UI_CHECK(f.has_value() && f->glyphIndex(U'A') != 0 && f->glyphIndex(U'\u0416') != 0);
    }
    if (segoe) {
        std::printf("  Segoe UI -> %s (%s, %u)\n", segoe->path.c_str(), segoe->style.c_str(), segoe->weight);
        UI_CHECK(segoe->weight == 400 && !segoe->italic);
        const FontFaceInfo* semi = cat.match("Segoe UI", 600);
        UI_CHECK(semi && semi->weight >= 550 && semi->weight <= 700);
    }
    const FontFaceInfo* sans = cat.match("sans-serif");
    const FontFaceInfo* mono = cat.match("monospace");
    UI_CHECK(sans && segoe && sans->family == segoe->family);
    UI_CHECK(mono != nullptr);
    // a collection face (.ttc) loads by index
    for (const FontFaceInfo& f : cat.faces())
        if (f.index > 0) {
            auto x = FontCatalog::load(f);
            UI_CHECK(x.has_value());
            std::printf("  collection face %s #%u: %s\n", f.path.c_str(), f.index, x ? "loaded" : "FAILED");
            break;
        }
#endif
    // per-script chains: the first face of a non-empty chain really has a glyph for the script
    const std::pair<Script, char32_t> probes[] = {
        {Script::Latin, U'A'},          {Script::Greek, U'\u03A9'},  {Script::Cyrillic, U'\u0416'}, {Script::Arabic, U'\u0628'},
        {Script::Hebrew, U'\u05D0'},    {Script::Devanagari, U'\u0915'}, {Script::Thai, U'\u0E01'}, {Script::Han, U'\u4E2D'},
        {Script::Kana, U'\u3042'},      {Script::Hangul, U'\uD55C'}, {Script::Emoji, U'\U0001F600'}, {Script::Symbols, U'\u2B95'},
    };
    int chains = 0;
    for (const auto& [script, cp] : probes) {
        UI_CHECK(scriptOf(cp) == script);
        const auto chain = cat.systemFallbackChain(script);
        std::printf("  %-10s:", scriptName(script));
        for (const FontFaceInfo* f : chain) std::printf(" [%s]", f->family.c_str());
        std::printf("\n");
        if (chain.empty()) continue;
        ++chains;
        auto f = FontCatalog::load(*chain.front());
        UI_CHECK(f.has_value());
        if (f && script != Script::Symbols) UI_CHECK(f->glyphIndex(cp) != 0);
    }
    UI_CHECK(!cat.systemFallbackChain(Script::Latin).empty());
#if defined(_WIN32)
    for (Script s : {Script::Arabic, Script::Hebrew, Script::Han, Script::Emoji}) UI_CHECK(!cat.systemFallbackChain(s).empty());
#endif
    // fontPicker lists the installed families
    const std::string list = w::systemFontFamilies();
    UI_CHECK(list.starts_with("Open Sans\n"));
#if defined(_WIN32)
    UI_CHECK(list.find("\nSegoe UI\n") != std::string::npos && list.find("\nArial\n") != std::string::npos);
#endif
    UiContext ui{{.viewport = {400, 300}, .font = &fuse::ui::test::font()}};
    w::install(ui);
    const NodeHandle fp = w::fontPicker(ui, ui.root(), "@system", 14.f, "fonts");
    ui.update();
    UI_CHECK_EQ(w::fontPickerFamily(ui, fp), std::string("Open Sans"));
    std::printf("  %d script chains\n", chains);
    return finish("fuse_text_system_fonts");
}

// ---- fuse_text_cache_zero_alloc ------------------------------------------------------------------------------------
int cacheZeroAlloc() {
    ChainFont cf;
    const Font& font = cf.font;
    const std::string strings[] = {
        "Settings",
        "The quick brown fox jumps over the lazy dog, again and again, until the line has to wrap twice.",
        utf8(U"Caf\u00E9 \u2014 \u0421\u044A\u0435\u0448\u044C \u0436\u0435 \u03A9 \U0001F600 \U0001F44D\U0001F3FD \u4E2D\u6587 \u0E2A\u0E27\u0E31\u0E2A\u0E14\u0E35"),
        utf8(U"Shalom \u05E9\u05DC\u05D5\u05DD and salam \u0633\u0644\u0627\u0645 \u0646\u0645\u0627\u0632, mixed RTL"),
        "Line one\nLine two\n\nLine four",
    };
    TextLayout tl;
    std::vector<ShapedGlyph> scratch;
    std::vector<LineBreak> breaks;
    GlyphAtlas atlas(1024);
    std::size_t sink = 0;
    auto pass = [&] {
        for (const std::string& s : strings) {
            for (float maxW : {0.f, 160.f, 320.f})
                for (TextAlign a : {TextAlign::Start, TextAlign::Center, TextAlign::End}) {
                    TextLayoutDesc d;
                    d.pixelSize = 15.f;
                    d.maxWidth = maxW;
                    d.align = a;
                    layoutText(font, defaultShaper(), s, d, tl);
                    sink += tl.glyphs.size();
                }
            TextLayoutDesc el;
            el.pixelSize = 15.f;
            el.maxWidth = 90.f;
            el.elide = true;
            layoutText(font, defaultShaper(), s, el, tl);
            for (const ShapedGlyph& g : tl.glyphs)
                if (g.glyph != kInvisibleGlyph)
                    forEachPaintGlyph(font, g, 15.f, [&](const Font& f, std::uint32_t glyph, float px, const ColorLayer*) {
                        sink += atlas.get(f, glyph, px).w;
                    });
            sink += std::size_t(measureText(font, s, 15.f, scratch));
            sink += graphemeCount(s) + nextBoundary(s, 0) + prevBoundary(s, s.size());
            lineBreaks(s, breaks);
            sink += breaks.size();
        }
    };
    for (int i = 0; i < 3; ++i) pass(); // warm-up: run cache, scratch capacity, atlas
    const ShapeCacheStats before = shapeCacheStats(font);
    const std::uint32_t atlasMisses = atlas.misses();
    g_allocs = 0;
    g_counting = true;
    for (int i = 0; i < 20; ++i) pass();
    g_counting = false;
    const ShapeCacheStats after = shapeCacheStats(font);
    std::printf("allocations in 20 steady passes: %ld (run cache hits +%llu, misses +%llu; atlas misses +%u; sink %zu)\n",
                g_allocs.load(), (unsigned long long)(after.hits - before.hits),
                (unsigned long long)(after.misses - before.misses), atlas.misses() - atlasMisses, sink);
    UI_CHECK_EQ(g_allocs.load(), 0L);
    UI_CHECK_EQ(atlas.misses(), atlasMisses);
    if (textProfile() == TextProfile::Full) {
        UI_CHECK(after.misses == before.misses);
        UI_CHECK(after.hits > before.hits);
    }
    return finish("fuse_text_cache_zero_alloc");
}

// ---- fuse_text_dpi_<scale> -----------------------------------------------------------------------------------------
float ink(const Font& font, const TextLayout& tl, float px, GlyphAtlas& atlas) {
    double sum = 0.0;
    for (const ShapedGlyph& g : tl.glyphs) {
        if (g.glyph == kInvisibleGlyph) continue;
        forEachPaintGlyph(font, g, px, [&](const Font& f, std::uint32_t glyph, float fpx, const ColorLayer*) {
            const AtlasGlyph a = atlas.get(f, glyph, fpx);
            for (int y = 0; y < a.h; ++y)
                for (int x = 0; x < a.w; ++x) sum += atlas.pixels()[std::size_t(a.v + y) * std::size_t(atlas.size()) + a.u + x];
        });
    }
    return float(sum / 255.0);
}

int dpi(float scale, const char* name) {
    ChainFont cf;
    const Font& font = cf.font;
    const float base = 14.f, px = base * scale;
    const bool full = textProfile() == TextProfile::Full;
    const std::string sample = full ? utf8(U"Settings \u2014 \u0421\u043F\u0430\u0441\u0438\u0431\u043E \u03A9\u03BC\u03B5\u03B3\u03B1 "
                                           U"\u05E9\u05DC\u05D5\u05DD \u0633\u0644\u0627\u0645 \u0928\u092E\u0938\u094D\u0924\u0947 "
                                           U"\u0E2A\u0E27\u0E31\u0E2A\u0E14\u0E35 \u4E2D\u6587 \U0001F600 \u2B95")
                                    : utf8(U"Settings \u2014 \u0421\u043F\u0430\u0441\u0438\u0431\u043E \u03A9\u03BC\u03B5\u03B3\u03B1 "
                                           U"\u4E2D\u6587 \U0001F600 \u2B95"); // MINIMAL: no complex scripts
    // metrics scale linearly
    const FontMetrics m1 = font.metrics(base), ms = font.metrics(px);
    UI_CHECK(std::abs(ms.ascent - m1.ascent * scale) < 0.01f * scale);
    UI_CHECK(std::abs(ms.descent - m1.descent * scale) < 0.01f * scale);
    UI_CHECK(std::abs(ms.lineHeight() - m1.lineHeight() * scale) < 0.02f * scale);
    std::printf("%s: %.2fx -> %.2f px, line height %.2f (1x %.2f)\n", name, double(scale), double(px), double(ms.lineHeight()),
                double(m1.lineHeight()));
    // layout: no missing glyphs, widths scale, same line breaks as at 1x
    TextLayout a, b;
    TextLayoutDesc d1, ds;
    d1.pixelSize = base;
    ds.pixelSize = px;
    layoutText(font, defaultShaper(), sample, d1, a);
    layoutText(font, defaultShaper(), sample, ds, b);
    UI_CHECK_EQ(countNotdef(b), 0);
    const float w1 = a.width * scale, ws = b.width;
    std::printf("  width %.2f (1x scaled %.2f), %zu glyphs\n", double(ws), double(w1), b.glyphs.size());
    UI_CHECK(std::abs(ws - w1) <= 0.01f * w1 + 1.f);
    UI_CHECK(b.height == float(b.lines.size()) * std::ceil(ms.lineHeight()));
    d1.maxWidth = 150.f;
    ds.maxWidth = 150.f * scale;
    layoutText(font, defaultShaper(), sample, d1, a);
    layoutText(font, defaultShaper(), sample, ds, b);
    bool sameBreaks = a.lines.size() == b.lines.size();
    for (std::size_t k = 0; sameBreaks && k < a.lines.size(); ++k) sameBreaks &= a.lines[k].byteEnd == b.lines[k].byteEnd;
    UI_CHECK(sameBreaks);
    for (const TextLine& ln : b.lines) UI_CHECK(ln.width <= ds.maxWidth + 1.f);
    // rasterisation: glyph boxes and ink grow with the scale
    int x0, y0, x1, y1, X0, Y0, X1, Y1;
    const std::uint32_t H = font.glyphIndex('H');
    font.glyphBox(H, base, x0, y0, x1, y1);
    font.glyphBox(H, px, X0, Y0, X1, Y1);
    std::printf("  'H' box %dx%d (1x %dx%d)\n", X1 - X0, Y1 - Y0, x1 - x0, y1 - y0);
    UI_CHECK(std::abs(float(Y1 - Y0) - float(y1 - y0) * scale) <= 2.f);
    UI_CHECK(std::abs(float(X1 - X0) - float(x1 - x0) * scale) <= 2.f);
    GlyphAtlas atlas(2048);
    d1.maxWidth = ds.maxWidth = 0.f;
    layoutText(font, defaultShaper(), sample, d1, a);
    layoutText(font, defaultShaper(), sample, ds, b);
    const float i1 = ink(font, a, base, atlas), is = ink(font, b, px, atlas);
    const float ratio = is / (i1 * scale * scale);
    std::printf("  ink %.0f (1x %.0f), ratio to scale^2 %.3f\n", double(is), double(i1), double(ratio));
    UI_CHECK(ratio > 0.92f && ratio < 1.08f);
    // the UI renders the sample at the scaled size without missing glyphs
    UiContext ui{{.viewport = {std::ceil(420.f * scale), std::ceil(40.f * scale)}, .font = &font}};
    const NodeHandle l = w::label(ui, ui.root(), sample);
    char fs[32];
    std::snprintf(fs, sizeof fs, "%gpx", double(px));
    ui.setCss(l, "font-size", fs);
    ui.update();
    Image img;
    fuse::ui::test::render(ui, img);
    int lit = 0;
    for (const u32 p : img.pixels) lit += ((p >> 24) & 0xFF) > 0 && (p & 0xFFFFFF) != 0;
    UI_CHECK(lit > int(100 * scale * scale));
    if (!g_out.empty()) writePng(g_out + "/" + name + ".png", img);
    return finish(name);
}
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--out=", 6) == 0) g_out = argv[i] + 6;
    const std::string_view gate = FUSE_TEXT_GATE;
    if (gate == "shaping_scripts") return shapingScripts();
    if (gate == "system_fonts") return systemFonts();
    if (gate == "cache_zero_alloc") return cacheZeroAlloc();
    if (gate == "dpi_1") return dpi(1.f, "fuse_text_dpi_1");
    if (gate == "dpi_1_25") return dpi(1.25f, "fuse_text_dpi_1_25");
    if (gate == "dpi_1_5") return dpi(1.5f, "fuse_text_dpi_1_5");
    if (gate == "dpi_2") return dpi(2.f, "fuse_text_dpi_2");
    std::fprintf(stderr, "unknown gate\n");
    return 2;
}
