// fuse_text_fallback (UI3): font fallback chains and COLRv0 colour emoji.
//   - Font::bundledFallbacks: Hebrew, Arabic, Devanagari, Twemoji (COLRv0 + CPAL), Symbols 2, Thai and CJK, in that order
//   - faceFor picks the first face whose cmap has the code point; runs split per face at cluster granularity
//     (marks, emoji modifiers and ZWJ sequences stay on one face), no .notdef in mixed text
//   - colour glyphs paint as palette-coloured layers through the ordinary Glyph op (CPU and GPU unchanged)
//   - layout with fallbacks still wraps inside maxWidth; a label renders coloured emoji pixels
// Optional --out=<dir>: text_fallback.png.
#include "test_util.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_write_impl.hpp"

#include <cstring>
#include <string>

using namespace fuse::ui;
using namespace fuse::text;
namespace w = fuse::ui::widgets;

namespace {
std::string utf8(std::u32string_view cps) {
    std::string s;
    for (char32_t c : cps) appendUtf8(s, c);
    return s;
}
// pixels whose channels differ a lot (text is grey on black; emoji are colourful)
int colourful(const Image& img, Rect r) {
    int n = 0;
    for (int y = int(r.y); y < int(r.y + r.h) && y < img.height; ++y)
        for (int x = int(r.x); x < int(r.x + r.w) && x < img.width; ++x) {
            const u32 p = img.at(x, y);
            const int R = int(p & 0xFF), G = int((p >> 8) & 0xFF), B = int((p >> 16) & 0xFF);
            if (std::max({R, G, B}) - std::min({R, G, B}) > 80) ++n;
        }
    return n;
}
} // namespace

int main(int argc, char** argv) {
    std::string out;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--out=", 6) == 0) out = argv[i] + 6;

    auto primary = Font::bundled();
    UI_CHECK(primary.has_value());
    if (!primary) return fuse::ui::test::finish("fuse_text_fallback");
    Font& font = *primary;
    std::vector<Font> chainFonts = Font::bundledFallbacks();
    UI_CHECK_EQ(chainFonts.size(), std::size_t(7));
    if (chainFonts.size() != 7) return fuse::ui::test::finish("fuse_text_fallback");
    UI_CHECK(!font.hasColorGlyphs() && chainFonts[3].hasColorGlyphs());

    std::vector<ShapedGlyph> g;
    const float plainW = shapeText(font, utf8(U"Hi \U0001F600"), 20.f, g);
    UI_CHECK(g.back().glyph == 0); // no chain yet: .notdef
    std::vector<const Font*> chain;
    for (const Font& f : chainFonts) chain.push_back(&f);
    font.setFallbacks(chain);
    UI_CHECK_EQ(font.faceCount(), std::size_t(8));

    UI_CHECK_EQ(font.faceFor(U'A'), 0);
    UI_CHECK_EQ(font.faceFor(U'\u05D0'), 1);  // Hebrew alef
    UI_CHECK_EQ(font.faceFor(U'\u0628'), 2);  // Arabic beh
    UI_CHECK_EQ(font.faceFor(U'\u0915'), 3);  // Devanagari ka
    UI_CHECK_EQ(font.faceFor(U'\U0001F600'), 4);
    UI_CHECK_EQ(font.faceFor(U'\u2764'), 4);  // emoji presentation wins over Symbols 2
    UI_CHECK_EQ(font.faceFor(U'\u2B95'), 5);  // only Symbols 2 has it
    UI_CHECK_EQ(font.faceFor(U'\u0E01'), 6);  // Thai ko kai
    UI_CHECK_EQ(font.faceFor(U'\u4E2D'), 7);  // CJK subset
    UI_CHECK_EQ(font.faceFor(U'\u3042'), 7);  // hiragana a
    UI_CHECK_EQ(font.faceFor(U'\U0010FFFD'), 0);

    // runs and faces
    const float emojiW = shapeText(font, utf8(U"Hi \U0001F600"), 20.f, g);
    UI_CHECK(g.size() == 4 && g[0].face == 0 && g[2].face == 0 && g[3].face == 4 && g[3].glyph != 0);
    UI_CHECK(g[3].x > g[2].x && emojiW > plainW * 0.8f);
    const auto layers = font.face(4).colorLayers(g[3].glyph);
    UI_CHECK(layers.size() >= 2);
    bool palette = false;
    for (const auto& l : layers) palette |= !l.foreground && (l.r != l.g || l.g != l.b);
    UI_CHECK(palette);
    UI_CHECK(font.colorLayers(font.glyphIndex(U'A')).empty());
    // em sizes match: the emoji is about one em wide
    UI_CHECK(g[3].advance > 14.f && g[3].advance < 30.f);

    shapeText(font, utf8(U"\U0001F44D\U0001F3FD"), 20.f, g); // thumbs up + skin tone: one face
    bool allEmoji = !g.empty();
    for (const auto& x : g) allEmoji &= x.face == 4 && x.glyph != 0;
    UI_CHECK(allEmoji);
    shapeText(font, utf8(U"e\u0301"), 20.f, g); // primary has both
    UI_CHECK(!g.empty() && g.size() <= 2 && g[0].face == 0 && g.back().face == 0); // FULL composes to U+00E9
    shapeText(font, utf8(U"\u0915\u093F"), 20.f, g); // mark follows its base's face
    bool deva = !g.empty();
    for (const auto& x : g) deva &= x.face == 3 && x.glyph != 0;
    UI_CHECK(deva);
#if FUSE_UI_TEST_TEXT_FULL
    shapeText(font, utf8(U"\U0001F468\u200D\U0001F469\u200D\U0001F467"), 20.f, g); // GSUB ligature in FULL
    UI_CHECK_EQ(g.size(), std::size_t(1));
#endif

    // mixed text: no .notdef anywhere
    const std::string mixed = utf8(U"Shalom \u05E9\u05DC\u05D5\u05DD, salam \u0633\u0644\u0627\u0645, namaste \u0928\u092E\u0938\u094D\u0924\u0947 \U0001F44B\U0001F3FD \u2764\uFE0F \u2B95 ok");
    TextLayout tl;
    TextLayoutDesc d;
    d.pixelSize = 20.f;
    layoutText(font, defaultShaper(), mixed, d, tl);
    int notdef = 0, faces = 0;
    bool seen[8] = {};
    for (const auto& x : tl.glyphs) {
        notdef += x.glyph == 0;
        if (!seen[x.face]) { seen[x.face] = true; ++faces; }
    }
    if (notdef) std::printf("  %d .notdef glyphs in the mixed line\n", notdef);
    UI_CHECK_EQ(notdef, 0);
    UI_CHECK_EQ(faces, 6);
    d.maxWidth = 260.f;
    layoutText(font, defaultShaper(), mixed, d, tl);
    bool fits = tl.lines.size() >= 2;
    for (const auto& ln : tl.lines) {
        if (ln.width > d.maxWidth + 1.f) std::printf("  line %u..%u width %.1f\n", ln.byteBegin, ln.byteEnd, ln.width);
        fits &= ln.width <= d.maxWidth + 1.f;
    }
    UI_CHECK(fits);

#if FUSE_UI_TEST_TEXT_FULL
    // fallback runs are shaped once, by their own face, inside the bidi / line-breaking pass: a new string costs the
    // primary one cache miss per Latin run and the Hebrew face one; a relayout costs neither anything
    {
        const std::string fresh = utf8(U"zqx \u05D0\u05D1\u05D2\u05D3 xqz");
        const ShapeCacheStats p0 = shapeCacheStats(font), h0 = shapeCacheStats(chainFonts[0]);
        TextLayout fl;
        TextLayoutDesc fd;
        fd.pixelSize = 20.f;
        layoutText(font, defaultShaper(), fresh, fd, fl);
        const ShapeCacheStats p1 = shapeCacheStats(font), h1 = shapeCacheStats(chainFonts[0]);
        std::printf("  FULL fallback shaping: primary misses %llu, Hebrew misses %llu\n",
                    (unsigned long long)(p1.misses - p0.misses), (unsigned long long)(h1.misses - h0.misses));
        UI_CHECK_EQ(p1.misses - p0.misses, std::uint64_t(2));
        UI_CHECK_EQ(h1.misses - h0.misses, std::uint64_t(1));
        bool hebFace = true;
        for (const auto& x : fl.glyphs) hebFace &= (x.cluster >= 4 && x.cluster < 12) == (x.face == 1);
        UI_CHECK(hebFace);
        layoutText(font, defaultShaper(), fresh, fd, fl);
        UI_CHECK(shapeCacheStats(font).misses == p1.misses && shapeCacheStats(chainFonts[0]).misses == h1.misses);
    }
#endif

#if !FUSE_UI_TEST_TEXT_COOKED
    // fallbacks follow the primary's weight
    {
        enableVariableFonts();
        const Font& bold = font.emboldened(); // the bundled Open Sans is static: the UI synthesises its bold
        UI_CHECK(font.weight() == 400 && bold.weight() == 700 && &bold != &font && &bold.emboldened() == &bold);
        UI_CHECK(&font.face(1) == &chainFonts[0]); // regular primary: the fallbacks as they are
        const Font& hb = bold.face(1);             // static Noto Sans Hebrew: synthesised bold
        UI_CHECK(&hb == &chainFonts[0].emboldened() && &hb != &chainFonts[0] && hb.weight() >= 600);
        UI_CHECK(&bold.face(4) == &chainFonts[3]); // colour emoji are never emboldened
        const float px = 40.f, e = hb.syntheticBold(px);
        UI_CHECK(e > 1.f && e < 3.f); // em / 24
        const std::uint32_t alef = chainFonts[0].glyphIndex(U'\u05D0');
        UI_CHECK(std::abs(hb.advance(alef, px) - chainFonts[0].advance(alef, px) - e) < 1e-3f);
        auto ink = [&](const Font& f, int& w) {
            int x0, y0, x1, y1;
            f.glyphBox(alef, px, x0, y0, x1, y1);
            w = x1 - x0;
            std::vector<std::uint8_t> px8(std::size_t(std::max(0, w) * std::max(0, y1 - y0)), 0);
            if (!px8.empty()) f.rasterise(alef, px, px8.data(), w, y1 - y0, w);
            long s = 0;
            for (const std::uint8_t v : px8) s += v;
            return s;
        };
        int wr = 0, wb = 0;
        const long inkR = ink(chainFonts[0], wr), inkB = ink(hb, wb);
        UI_CHECK(wb == wr + int(std::ceil(e)) && inkB > inkR * 11 / 10);
        // layout of the bold primary: Hebrew drawn by the bold face, every letter wider by the strength
        const std::string heb = utf8(U"\u05E9\u05DC\u05D5\u05DD");
        TextLayout lr, lb;
        TextLayoutDesc bd;
        bd.pixelSize = px;
        layoutText(font, defaultShaper(), heb, bd, lr);
        layoutText(bold, defaultShaper(), heb, bd, lb);
        bool boldFace = !lb.glyphs.empty();
        for (const auto& x : lb.glyphs) boldFace &= x.face == 1 && x.glyph != 0;
        UI_CHECK(boldFace);
        UI_CHECK(std::abs(lb.width - lr.width - 4.f * hb.syntheticBold(bold.facePixelSize(1, px))) < 0.05f);
        // a bold face given with setBoldFace wins (here: a second copy of the Hebrew face stands in for a real Bold)
        const std::vector<Font> second = Font::bundledFallbacks();
        const Font* boldCut = second.empty() ? nullptr : &second[0];
        UI_CHECK(boldCut != nullptr);
        if (boldCut) {
            chainFonts[0].setBoldFace(boldCut);
            UI_CHECK(&bold.face(1) == boldCut && &font.face(1) == &chainFonts[0]);
            chainFonts[0].setBoldFace(nullptr);
            UI_CHECK(&bold.face(1) == &hb);
        }
        // a variable fallback is instanced at the primary's exact weight
        auto varFace = Font::fromFile("data/variable_test.ttf"); // U+E000 is not in Open Sans
        auto heavy = Font::bundled();
        UI_CHECK(varFace.has_value() && heavy.has_value());
        if (varFace && heavy) {
            heavy->setFallbacks({&*varFace});
            const Font& hv = heavy->emboldened();
            UI_CHECK(hv.weight() == 700 && hv.faceFor(0xE000) == 1);
            const Font& vf = hv.face(1);
            UI_CHECK(&vf != &*varFace && vf.variationCoords().size() == 2 && vf.variationCoords()[0] == 700.f);
        }
        // the UI: font-weight 700 on a label makes its Hebrew bold too
        UiContext bui{{.viewport = {400, 80}, .font = &font}};
        const NodeHandle reg = w::label(bui, bui.root(), heb, "reg");
        const NodeHandle bl = w::label(bui, bui.root(), heb, "bold");
        bui.setCss(bl, "font-weight", "700");
        bui.update();
        // main's UI bold of a static family is FontFamily's synthesized() variant: it reports its base's weight and keeps
        // the fallback chain as it is (Font::synthesized); only real bold faces, variable instances and emboldened()
        // fonts restyle their fallbacks
        const Font* uiBold = bui.nodeFont(bl);
        UI_CHECK(uiBold && uiBold != &font && uiBold->syntheticEmbolden() > 0.f && uiBold->weight() == 400);
        UI_CHECK(uiBold && &uiBold->face(1) == &chainFonts[0] && bui.nodeFont(reg) == &font);
        std::printf("  bold fallback: synthetic strength %.2f px at 40 px, ink +%.0f%%\n", double(e),
                    double(100.f * float(inkB - inkR) / float(inkR)));
    }
#endif

    // render: emoji pixels are coloured, the rest is grey
    UiContext ui{{.viewport = {640, 150}, .font = &font}};
    ui.setCss(ui.root(), "padding", "10px");
    ui.setCss(ui.root(), "gap", "8px");
    const NodeHandle a = w::label(ui, ui.root(), "Plain text, no emoji", "plain");
    const NodeHandle b = w::label(ui, ui.root(), utf8(U"Emoji \U0001F600 \U0001F680 \U0001F44D\U0001F3FD \u2764\uFE0F"), "emoji");
    const NodeHandle c = w::label(ui, ui.root(), mixed, "mixed");
    (void)c;
    ui.update();
    Image img;
    fuse::ui::test::render(ui, img);
    const int greyRow = colourful(img, ui.rect(a)), emojiRow = colourful(img, ui.rect(b));
    std::printf("fallback: colourful pixels plain %d, emoji %d; %zu lines at 260 px\n", greyRow, emojiRow, tl.lines.size());
    UI_CHECK(greyRow == 0);
    UI_CHECK(emojiRow > 100);
    if (!out.empty()) {
        const std::string path = out + "/text_fallback.png";
        stbi_write_png(path.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("wrote %s\n", path.c_str());
    }
    return fuse::ui::test::finish("fuse_text_fallback");
}
