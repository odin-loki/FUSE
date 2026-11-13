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
//   colrv1           data/colrv1_test.ttf (make_colrv1_test_font.py): without enableColorGlyphs, COLRv1 graphs flattened
//                    to solid layers (gradients as mean colour, PaintColrGlyph through a transform, v0 records win);
//                    with it, exact RGBA rendering (linear / radial / sweep gradients, transforms, composite modes, clip
//                    boxes, foreground colour) against golden/colrv1_exact.png at max channel difference 0, analytic
//                    spot checks and colour glyph commands through the UI.
// Optional --out=<dir> writes the rendered images.
#include "test_env.hpp"
#include "test_util.hpp"

#if defined(__GNUC__) || defined(__clang__) // (clang-cl defines only __clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <array>
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
int systemFontsGate() {
    if (textProfile() == TextProfile::Cooked) {
        std::printf("fuse_text_system_fonts: COOKED loads only .fusefont packs, skipped\n");
        return 77;
    }
    const auto t0 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const std::vector<std::string> fams = systemFontFamilies();
    std::printf("system fonts: %zu faces, %zu families in %.0f ms\n", fuse::text::systemFonts().size(), fams.size(), ms);
    for (const std::string& d : systemFontDirectories()) std::printf("  dir %s\n", d.c_str());
    if (fuse::text::systemFonts().empty()) {
        std::printf("fuse_text_system_fonts: no fonts installed, skipped\n");
        return 77;
    }
    for (const SystemFontInfo& f : fuse::text::systemFonts()) {
        UI_CHECK(!f.family.empty() && f.weight >= 1 && f.weight <= 1000);
    }
    UI_CHECK(findSystemFont("No Such Family 123") == nullptr);
#if defined(_WIN32)
    // Windows' own fonts; Wine ships neither Segoe UI nor Arial (its catalogue holds the host's fonts)
    const bool winFonts = !fuse::ui::test::runningUnderWine();
    if (!winFonts) {
        std::printf("  under Wine: generic families only\n");
        UI_CHECK(findSystemFont("sans-serif") != nullptr && findSystemFont("monospace") != nullptr);
    } else {
        const SystemFontInfo* segoe = findSystemFont("Segoe UI");
        const SystemFontInfo* arial = findSystemFont("arial"); // case-insensitive
        UI_CHECK(segoe && arial);
        if (arial) {
            std::printf("  Arial -> %s (%s, %u)\n", arial->path.c_str(), arial->style.c_str(), arial->weight);
            UI_CHECK(arial->weight == 400 && !arial->italic);
            const SystemFontInfo* bold = findSystemFont("Arial", 700);
            const SystemFontInfo* italic = findSystemFont("Arial", 400, true);
            const SystemFontInfo* boldItalic = findSystemFont("Arial", 700, true);
            UI_CHECK(bold && bold->weight == 700 && !bold->italic);
            UI_CHECK(italic && italic->italic && italic->weight == 400);
            UI_CHECK(boldItalic && boldItalic->italic && boldItalic->weight == 700);
            auto f = loadSystemFont(*arial);
            UI_CHECK(f.has_value() && f->glyphIndex(U'A') != 0 && f->glyphIndex(U'\u0416') != 0);
        }
        if (segoe) {
            std::printf("  Segoe UI -> %s (%s, %u)\n", segoe->path.c_str(), segoe->style.c_str(), segoe->weight);
            UI_CHECK(segoe->weight == 400 && !segoe->italic);
            const SystemFontInfo* semi = findSystemFont("Segoe UI", 600);
            UI_CHECK(semi && semi->weight >= 550 && semi->weight <= 700);
        }
        const SystemFontInfo* sans = findSystemFont("sans-serif");
        const SystemFontInfo* mono = findSystemFont("monospace");
        UI_CHECK(sans && segoe && sans->family == segoe->family);
        UI_CHECK(mono != nullptr);
        // a collection face (.ttc) loads by index
        for (const SystemFontInfo& f : fuse::text::systemFonts())
            if (f.faceIndex > 0) {
                auto x = loadSystemFont(f);
                UI_CHECK(x.has_value());
                std::printf("  collection face %s #%u: %s\n", f.path.c_str(), f.faceIndex, x ? "loaded" : "FAILED");
                break;
            }
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
        const auto chain = systemFallbackChain(script);
        std::printf("  %-10s:", scriptName(script));
        for (const SystemFontInfo* f : chain) std::printf(" [%s]", f->family.c_str());
        std::printf("\n");
        if (chain.empty()) continue;
        ++chains;
        auto f = loadSystemFont(*chain.front());
        UI_CHECK(f.has_value());
        if (f && script != Script::Symbols) UI_CHECK(f->glyphIndex(cp) != 0);
    }
    UI_CHECK(!systemFallbackChain(Script::Latin).empty());
#if defined(_WIN32)
    if (winFonts)
        for (Script s : {Script::Arabic, Script::Hebrew, Script::Han, Script::Emoji}) UI_CHECK(!systemFallbackChain(s).empty());
#endif
    // generic CSS families resolve to an installed family; fontPicker (empty list) starts with the bundled family
    {
        const SystemFontInfo* sans = findSystemFont("sans-serif");
        const SystemFontInfo* mono = findSystemFont("monospace");
        std::printf("  sans-serif -> %s, monospace -> %s\n", sans ? sans->family.c_str() : "(none)", mono ? mono->family.c_str() : "(none)");
        if (sans) UI_CHECK(sans->covers(Script::Latin) && findSystemFont("system-ui") == sans && sans->stretch == 5);
        if (mono) UI_CHECK(mono->monospace || mono->family.find("Mono") != std::string::npos);
    }
    UiContext ui{{.viewport = {400, 300}, .font = &fuse::ui::test::font()}};
    w::install(ui);
    const NodeHandle fp = w::fontPicker(ui, ui.root(), {}, 14.f, "fonts");
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

// ---- fuse_text_colrv1 ----------------------------------------------------------------------------------------------
int colrv1() {
    auto loaded = Font::fromFile(fuse::ui::test::testFontPath("colrv1_test"));
    UI_CHECK(loaded.has_value());
    if (!loaded) return finish("fuse_text_colrv1");
    const Font& f = *loaded;
    UI_CHECK(f.hasColorGlyphs());
    const std::uint32_t box = f.glyphIndex(U'A');
    auto near = [](int v, int want) { return std::abs(v - want) <= 1; };
    auto layers = [&](char32_t cp) { return f.colorLayers(f.glyphIndex(cp)); };
    // two PaintGlyph + PaintSolid layers: red box, half-transparent white inner square
    const auto solid = layers(U'\uE000');
    UI_CHECK(solid.size() == 2);
    if (solid.size() == 2) {
        UI_CHECK(solid[0].glyph == box && solid[0].r == 255 && solid[0].g == 0 && solid[0].b == 0 && solid[0].a == 255);
        UI_CHECK(solid[1].glyph != box && solid[1].r == 255 && solid[1].g == 255 && near(solid[1].a, 128));
    }
    // linear gradient red -> blue: one layer in the mean colour
    const auto grad = layers(U'\uE001');
    UI_CHECK(grad.size() == 1 && grad[0].glyph == box && near(grad[0].r, 128) && grad[0].g == 0 && near(grad[0].b, 128));
    // PaintTranslate(PaintColrGlyph(solid)): the referenced glyph's layers
    const auto ref = layers(U'\uE002');
    UI_CHECK(ref.size() == 2 && ref[0].r == 255 && ref[1].g == 255);
    // a v0 record (green) wins over the v1 graph (red)
    const auto both = layers(U'\uE003');
    UI_CHECK(both.size() == 1 && both[0].g == 255 && both[0].r == 0);
    UI_CHECK(f.colorLayers(box).empty());
    // rendered through the ordinary glyph path
    UiContext ui{{.viewport = {200, 60}, .font = &f}};
    const NodeHandle l = w::label(ui, ui.root(), utf8(U"\uE000\uE001\uE002\uE003"));
    ui.setCss(l, "font-size", "32px");
    ui.update();
    Image img;
    fuse::ui::test::render(ui, img);
    int red = 0, green = 0, purple = 0;
    for (const u32 p : img.pixels) {
        const int R = int(p & 0xFF), G = int((p >> 8) & 0xFF), B = int((p >> 16) & 0xFF);
        red += R > 200 && G < 40 && B < 40;
        green += G > 200 && R < 40 && B < 40;
        purple += R > 100 && B > 100 && G < 40;
    }
    std::printf("colrv1: red %d, green %d, purple %d pixels\n", red, green, purple);
    UI_CHECK(red > 50 && green > 50 && purple > 50);
    if (!g_out.empty()) writePng(g_out + "/colrv1.png", img);

    // exact rendering (enableColorGlyphs): every v1 glyph is one RGBA image of its whole paint graph
    enableColorGlyphs();
    UI_CHECK(layers(U'').size() == 1 && layers(U'')[0].image);
    UI_CHECK(both.size() == 1 && layers(U'')[0].g == 255 && !layers(U'')[0].image); // v0 still wins
    const float px = 48.f, s = f.unitScale(px);
    const std::uint32_t fg = 0xFF0080FFu; // orange (255, 128, 0) for palette index 0xFFFF
    constexpr int kCell = 64, kCount = 13;
    Image sheet;
    sheet.resize(kCell * kCount, kCell, 0u);
    ColorImage ci;
    // straight RGBA of the texel under font-unit point (ux, uy) of glyph `cp` (pen at the origin)
    auto at = [&](char32_t cp, float ux, float uy) -> std::array<int, 4> {
        if (!f.renderColor(f.glyphIndex(cp), px, fg, ci)) return {-1, -1, -1, -1};
        const int x = int(std::floor(ux * s)) - ci.x0, y = int(std::floor(-uy * s)) - ci.y0;
        if (x < 0 || y < 0 || x >= ci.w || y >= ci.h) return {0, 0, 0, 0};
        const std::uint8_t* p = ci.rgba.data() + (std::size_t(y) * std::size_t(ci.w) + std::size_t(x)) * 4;
        return {p[0], p[1], p[2], p[3]};
    };
    for (int k = 0; k < kCount; ++k) {
        const std::uint32_t g = f.glyphIndex(char32_t(0xE000 + k));
        const bool has = f.renderColor(g, px, fg, ci);
        UI_CHECK(has == (k != 3)); // U+E003 has a v0 record: no image
        for (int y = 0; has && y < ci.h; ++y)
            for (int x = 0; x < ci.w; ++x) {
                const int dx = k * kCell + 4 + ci.x0 + x, dy = 52 + ci.y0 + y;
                if (dx < k * kCell || dx >= (k + 1) * kCell || dy < 0 || dy >= kCell) continue;
                const std::uint8_t* p = ci.rgba.data() + (std::size_t(y) * std::size_t(ci.w) + std::size_t(x)) * 4;
                sheet.pixels[std::size_t(dy) * std::size_t(sheet.width) + std::size_t(dx)] =
                    u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
            }
    }
    if (!g_out.empty()) writePng(g_out + "/colrv1_exact.png", sheet);
    // analytic spots: solid layers, gradient ends, multiply, foreground at half alpha, clip box
    auto eq = [](std::array<int, 4> a, std::array<int, 4> b) { return std::abs(a[0] - b[0]) <= 1 && std::abs(a[1] - b[1]) <= 1 && std::abs(a[2] - b[2]) <= 1 && std::abs(a[3] - b[3]) <= 1; };
    UI_CHECK(eq(at(U'', 500, 400), {255, 128, 128, 255}));  // white 50 % over red
    UI_CHECK(eq(at(U'', 150, 50), {255, 0, 0, 255}));
    const auto gl = at(U'', 120, 400), gr = at(U'', 880, 400);
    UI_CHECK(gl[0] > 240 && gl[2] < 15 && gr[2] > 240 && gr[0] < 15); // red -> blue, left to right
    UI_CHECK(eq(at(U'', 500, 400), {128, 64, 0, 128}));     // foreground orange, alpha 0.5, premultiplied
    UI_CHECK(f.renderColor(f.glyphIndex(U''), px, fg, ci) && ci.w <= int(std::ceil(400 * s)) + 2);
    compareGolden(sheet, "golden/colrv1_exact.png");
    // exactness: the same image twice, and the golden at max channel difference 0
    {
        int w = 0, h = 0, n = 0;
        if (unsigned char* ref = stbi_load("golden/colrv1_exact.png", &w, &h, &n, 4)) {
            int maxd = 0;
            for (int i = 0; w == sheet.width && h == sheet.height && i < w * h; ++i)
                for (int c = 0; c < 4; ++c)
                    maxd = std::max(maxd, std::abs(int(ref[i * 4 + c]) - int((sheet.pixels[std::size_t(i)] >> (8 * c)) & 0xFF)));
            std::printf("colrv1 exact: golden max channel difference %d\n", maxd);
            // COOKED rebuilds foreground-coloured glyphs from black / white renders: exact up to rounding (1 / 255)
            UI_CHECK(w == sheet.width && h == sheet.height && maxd <= (textProfile() == TextProfile::Cooked ? 1 : 0));
            stbi_image_free(ref);
        }
    }
    // through the UI: colour glyph commands sampling the atlas colour page
    UiContext ui2{{.viewport = {300, 60}, .font = &f}};
    const NodeHandle l2 = w::label(ui2, ui2.root(), utf8(U""));
    ui2.setCss(l2, "font-size", "32px");
    ui2.update();
    int colorCmds = 0;
    for (const DrawCmd& c : ui2.drawList().cmds()) colorCmds += c.op == DrawOp::Glyph && c.gradient == kGlyphColor;
    UI_CHECK(colorCmds == 4);
    fuse::ui::test::render(ui2, img);
    if (!g_out.empty()) writePng(g_out + "/colrv1_ui.png", img);
    return finish("fuse_text_colrv1");
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
    if (gate == "system_fonts") return systemFontsGate();
    if (gate == "cache_zero_alloc") return cacheZeroAlloc();
    if (gate == "colrv1") return colrv1();
    if (gate == "dpi_1") return dpi(1.f, "fuse_text_dpi_1");
    if (gate == "dpi_1_25") return dpi(1.25f, "fuse_text_dpi_1_25");
    if (gate == "dpi_1_5") return dpi(1.5f, "fuse_text_dpi_1_5");
    if (gate == "dpi_2") return dpi(2.f, "fuse_text_dpi_2");
    std::fprintf(stderr, "unknown gate\n");
    return 2;
}
