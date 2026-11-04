// fuse_text_variable (UI3: variable fonts). data/variable_test.ttf comes from data/make_variable_test_font.py, whose
// docstring gives the analytic expectations used here (stem width per weight, advances, MVAR ascender, Var* paints).
//   axes      fvar axes / names / named instances, design coordinates of instances
//   metrics   MVAR ascender at several weights (pixel scale unchanged)
//   outlines  'A' stem from rendered coverage and HVAR advance at wght 100 .. 900 and wdth 50 / 150 (through the avar
//             map), gvar IUP-inferred midpoints, composite 'B' component offsets, the advance from gvar phantom points
//             (HVAR hidden) equal to HVAR's, instance cache identity; FULL: HarfBuzz shaping advances agree and FreeType
//             renders the instance
//   paints    U+E000 rendered with enableColorGlyphs at wght 100 / 400 / 650 / 900: PaintVarSolid alpha,
//             PaintVarTranslate and PaintVarLinearGradient positions
//   css       font-weight / font-stretch / font-variation-settings / font-feature-settings parsing and inheritance;
//             labels laid out with the instance; AnimProp::FontWeight tweens the width; instance lookups and re-layout
//             of seen text allocate nothing
//   system    Segoe UI Variable / Bahnschrift when installed (else that part is skipped): catalog named-instance faces
//             ("Segoe UI Variable Display" / "Semibold" / 600), our glyf decoder at zero deltas vs stb_truetype, HVAR vs
//             phantom advances, ink and width grow with wght / wdth; FULL: HarfBuzz advances match
//   cooked    COOKED: the pack cooked at wght 700 (fuse_text_cook --variation=wght:700) has that instance's advance,
//             ascender, stem coverage and Var* paint colour
#include "test_util.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <vector>

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
using fuse::ui::test::finish;

namespace {

constexpr float kPx = 100.f; // the test font's hhea ascent - descent is 1000 units: 0.1 px per unit at 100 px

// Normalised wght of the test font (fvar 100 / 400 / 900, avar 0.4 -> 0.6) and the stem / ascender it gives.
float normWeight(float w) {
    float n = w < 400.f ? (w - 400.f) / 300.f : (w - 400.f) / 500.f;
    if (n > 0.f) n = n <= 0.4f ? n * 1.5f : 0.6f + (n - 0.4f) * (0.4f / 0.6f);
    return n;
}
float stemOf(float w) {
    const float n = normWeight(w);
    return n < 0.f ? 100.f + 60.f * n : 100.f + 200.f * n;
}

bool near(float a, float b, float tol) { return std::abs(a - b) <= tol; }

std::vector<std::uint8_t> readAll(const std::string& path) {
    std::vector<std::uint8_t> out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        out.resize(std::size_t(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
        std::fclose(f);
    }
    return out;
}

// A copy of a font file with the named tables hidden (the case of their tag's first letter flipped).
std::vector<std::uint8_t> hideTables(std::vector<std::uint8_t> d, std::initializer_list<const char*> tags) {
    const std::size_t n = std::size_t((d[4] << 8) | d[5]);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint8_t* e = d.data() + 12 + i * 16;
        for (const char* t : tags)
            if (std::memcmp(e, t, 4) == 0) e[0] = std::uint8_t(e[0] ^ 0x20);
    }
    return d;
}

struct Raster {
    int x0 = 0, y0 = 0, w = 0, h = 0;
    std::vector<std::uint8_t> px;
    long ink() const {
        long s = 0;
        for (const std::uint8_t v : px) s += v;
        return s;
    }
    float rowCoverage(int y) const { // pixels covered in row y (y relative to the baseline, down)
        const int r = y - y0;
        if (r < 0 || r >= h) return 0.f;
        float s = 0.f;
        for (int x = 0; x < w; ++x) s += float(px[std::size_t(r) * std::size_t(w) + std::size_t(x)]) / 255.f;
        return s;
    }
    int lastColumn() const { // last column with ink, relative to the pen
        for (int x = w - 1; x >= 0; --x)
            for (int y = 0; y < h; ++y)
                if (px[std::size_t(y) * std::size_t(w) + std::size_t(x)] > 127) return x0 + x;
        return x0;
    }
};

Raster render(const Font& f, std::uint32_t g, float px) {
    Raster r;
    int x1 = 0, y1 = 0;
    f.glyphBox(g, px, r.x0, r.y0, x1, y1);
    r.w = std::max(0, x1 - r.x0);
    r.h = std::max(0, y1 - r.y0);
    r.px.assign(std::size_t(r.w) * std::size_t(r.h), 0);
    if (r.w && r.h) f.rasterise(g, px, r.px.data(), r.w, r.h, r.w);
    return r;
}

float advanceUnits(const Font& f, std::uint32_t g) { return f.advance(g, kPx) / f.unitScale(kPx); }

const Font& at(const Font& f, float wght, float wdth = 100.f) {
    const FontVariation v[] = {{fontTag("wght"), wght}, {fontTag("wdth"), wdth}};
    return f.instance(v);
}

// ---- axes ------------------------------------------------------------------------------------------------------
void axes(const Font& font) {
    UI_CHECK(font.isVariable());
    const auto ax = font.axes();
    UI_CHECK_EQ(ax.size(), std::size_t(2));
    if (ax.size() == 2) {
        UI_CHECK(ax[0].tag == fontTag("wght") && ax[0].minValue == 100.f && ax[0].defaultValue == 400.f && ax[0].maxValue == 900.f);
        UI_CHECK(ax[1].tag == fontTag("wdth") && ax[1].minValue == 50.f && ax[1].defaultValue == 100.f && ax[1].maxValue == 150.f);
        UI_CHECK_EQ(ax[0].name, std::string("Weight"));
        UI_CHECK_EQ(ax[1].name, std::string("Width"));
    }
    const auto named = font.namedInstances();
    const char* names[] = {"Thin", "Regular", "SemiBold", "Black", "Condensed", "Bold Wide"};
    UI_CHECK_EQ(named.size(), std::size_t(6));
    for (std::size_t k = 0; k < named.size() && k < 6; ++k) UI_CHECK_EQ(named[k].name, std::string(names[k]));
    const Font& black = font.namedInstance(3);
    UI_CHECK(&black != &font && black.variationCoords().size() == 2 && black.variationCoords()[0] == 900.f);
    UI_CHECK(&font.namedInstance(1) == &font); // Regular is the default
    UI_CHECK(&at(font, 900.f) == &black);       // one instance per coordinates
    UI_CHECK(at(font, 2000.f).variationCoords()[0] == 900.f); // clamped
    const Font& wide = font.namedInstance(5);
    UI_CHECK(wide.variationCoords()[0] == 700.f && wide.variationCoords()[1] == 150.f);
    const FontVariation narrowOnly[] = {{fontTag("wdth"), 50.f}};
    UI_CHECK(black.instance(narrowOnly).variationCoords()[0] == 900.f); // settings apply over the instance's own
    std::printf("  axes: wght %g..%g, wdth %g..%g, %zu named instances\n", double(ax[0].minValue), double(ax[0].maxValue),
                double(ax[1].minValue), double(ax[1].maxValue), named.size());
}

// ---- metrics ---------------------------------------------------------------------------------------------------
void metrics(const Font& font) {
    for (const float w : {100.f, 400.f, 650.f, 900.f}) {
        const FontMetrics m = at(font, w).metrics(kPx);
        const float n = std::max(0.f, normWeight(w));
        UI_CHECK(near(m.ascent, (800.f + 100.f * n) * 0.1f, 0.05f));
        UI_CHECK(near(m.descent, -20.f, 0.01f));
        UI_CHECK(near(at(font, w).unitScale(kPx), 0.1f, 1e-6f));
        if (w == 650.f) std::printf("  metrics: ascent at wght 650 = %.3f px (MVAR hasc)\n", double(m.ascent));
    }
}

// ---- outlines --------------------------------------------------------------------------------------------------
void outlines(const Font& font, const std::vector<std::uint8_t>& bytes) {
    const std::uint32_t A = font.glyphIndex(U'A'), B = font.glyphIndex(U'B');
    auto noHvar = Font::fromBytes(hideTables(bytes, {"HVAR"}));
    UI_CHECK(noHvar.has_value());
    for (const float w : {100.f, 250.f, 400.f, 600.f, 650.f, 700.f, 900.f}) {
        const Font& f = at(font, w);
        const float stem = stemOf(w);
        UI_CHECK(near(advanceUnits(f, A), 300.f + stem, 0.1f));
        UI_CHECK(near(advanceUnits(f, B), 600.f + stem, 0.1f));
        if (noHvar) UI_CHECK(near(advanceUnits(at(*noHvar, w), A), advanceUnits(f, A), 0.5f)); // gvar phantom points
        const Raster r = render(f, A, kPx);
        UI_CHECK(near(r.rowCoverage(-35), stem * 0.1f, 0.06f)); // a row through the stem: its width in pixels
        UI_CHECK(r.x0 == 10);
        // composite: 'C' placed at s + 150, its right edge at s + 150 + 100 + s / 2
        const Raster rb = render(f, B, kPx);
        UI_CHECK(std::abs(rb.lastColumn() - int(std::floor((stem + 250.f + stem * 0.5f) * 0.1f - 0.5f))) <= 1);
        std::printf("  wght %3.0f: stem %.2f px (want %.2f), advance A %.2f units\n", double(w), double(r.rowCoverage(-35)),
                    double(stem * 0.1f), double(advanceUnits(f, A)));
    }
    for (const float d : {50.f, 150.f}) { // wdth scales x: stem 100 * d / 100, starting at 100 * d / 100
        const Font& f = at(font, 400.f, d);
        const Raster r = render(f, A, kPx);
        UI_CHECK(near(r.rowCoverage(-35), 10.f * d / 100.f, 0.06f));
        UI_CHECK(near(advanceUnits(f, A), 400.f * d / 100.f, 0.1f));
    }
    { // both axes: deltas add (x0 100 - 50, x1 200 + 200 - 100)
        const Raster r = render(at(font, 900.f, 50.f), A, kPx);
        UI_CHECK(near(r.rowCoverage(-35), 25.f, 0.06f) && r.x0 == 5);
    }
    // ink grows with weight
    long prev = 0;
    for (const float w : {100.f, 300.f, 400.f, 500.f, 700.f, 900.f}) {
        const long ink = render(at(font, w), A, 24.f).ink();
        UI_CHECK(ink > prev);
        prev = ink;
    }
#if FUSE_UI_TEST_TEXT_FULL
    { // HarfBuzz shapes the instance with the same advances (hb_font_set_var_coords_normalized)
        std::vector<ShapedGlyph> run;
        for (const float w : {100.f, 650.f, 900.f}) {
            const Font& f = at(font, w);
            const float width = HarfBuzzShaper{}.shape(f, kPx, "AB", run);
            UI_CHECK(near(width, (900.f + 2.f * stemOf(w)) * 0.1f, 0.1f));
        }
    }
#endif
}

// ---- paints ----------------------------------------------------------------------------------------------------
struct Rgba {
    int r, g, b, a;
};
Rgba sample(const ColorImage& img, float fx, float fy) {
    const int col = int(std::floor(fx * 0.1f)) - img.x0, row = int(std::floor(-fy * 0.1f)) - img.y0;
    if (col < 0 || row < 0 || col >= img.w || row >= img.h) return {-1, -1, -1, -1};
    const std::uint8_t* p = img.rgba.data() + (std::size_t(row) * std::size_t(img.w) + std::size_t(col)) * 4;
    return {p[0], p[1], p[2], p[3]};
}
bool close(Rgba c, Rgba want, int tol = 3) {
    return std::abs(c.r - want.r) <= tol && std::abs(c.g - want.g) <= tol && std::abs(c.b - want.b) <= tol &&
           std::abs(c.a - want.a) <= tol;
}
void paints(const Font& font) {
    const std::uint32_t g = font.glyphIndex(0xE000);
    struct Case {
        float w;
        float fx;
        Rgba want;
    };
    const float n650 = normWeight(650.f);
    const int red650 = int(std::lround((0.6f + 0.4f * n650) * 255.f));
    const float dx650 = 50.f + 50.f * n650, x1650 = 600.f + 300.f * n650; // gradient from 100 + dx to x1 + dx
    const float mid650 = (std::floor((100.f + dx650 + (x1650 - 100.f) * 0.5f) * 0.1f) + 0.5f) * 10.f; // a pixel centre
    const float t650 = (mid650 - 100.f - dx650) / (x1650 - 100.f);
    const int g650 = int(std::lround(t650 * 255.f));
    const Case cases[] = {
        {400.f, 125.f, {153, 0, 0, 153}},        // default instance: red strip (dx 50) at alpha 0.6
        {900.f, 150.f, {255, 0, 0, 255}},        // red at alpha 1.0, strip 100 .. 200
        {100.f, 145.f, {0, 57, 198, 255}},       // no strip: the gradient 100 -> 300, 22.5 % of the way
        {650.f, 125.f, {red650, 0, 0, red650}},  // interpolated alpha
        {650.f, mid650, {0, g650, 255 - g650, 255}}, // interpolated translate + gradient end
    };
    ColorImage img;
    for (const Case& c : cases) {
        const Font& f = at(font, c.w);
        UI_CHECK(f.colorLayers(g).size() == 1 && f.colorLayers(g)[0].image);
        UI_CHECK(f.renderColor(g, kPx, 0xFF000000u, img));
        const Rgba got = sample(img, c.fx, 400.f);
        if (!close(got, c.want))
            std::fprintf(stderr, "  wght %g at x %g: (%d %d %d %d), want (%d %d %d %d)\n", double(c.w), double(c.fx), got.r,
                         got.g, got.b, got.a, c.want.r, c.want.g, c.want.b, c.want.a);
        UI_CHECK(close(got, c.want));
    }
    std::printf("  paints: Var* solid / translate / gradient at 4 weights OK\n");
}

// ---- css ---------------------------------------------------------------------------------------------------------
void css(const Font& font) {
    std::vector<StyleDiagnostic> diags;
    auto sheet = StyleSheet::parse(R"(
        .a { font-weight: bold; font-stretch: condensed; font-variation-settings: "wdth" 80, 'XHGT' 0.5;
             font-feature-settings: "tnum", "liga" off, "ss01" 2; }
        .b { font-weight: 650; font-stretch: 112.5%; }
        .bad { font-weight: 0; font-variation-settings: "wgh" 1; font-feature-settings: tnum; }
    )", &diags);
    UI_CHECK(sheet.has_value());
    UI_CHECK(diags.size() == 3);
    UiContext ui{{.viewport = {800, 400}, .font = &font}};
    UI_CHECK(ui.addStyleSheet(R"(
        .a { font-weight: bold; font-stretch: condensed; font-variation-settings: "wdth" 80, 'XHGT' 0.5;
             font-feature-settings: "tnum", "liga" off, "ss01" 2; }
        .b { font-weight: 650; font-stretch: 112.5%; }
        .big { font-size: 100px; }
    )").has_value());
    const NodeHandle a = *ui.create("label", ui.root());
    ui.addClass(a, "a");
    const NodeHandle child = *ui.create("label", a);
    const NodeHandle b = *ui.create("label", ui.root());
    ui.addClass(b, "b");
    ui.update(0.f);
    const ComputedStyle& sa = ui.computed(a);
    UI_CHECK(sa.fontWeight == 700.f && sa.fontStretch == 75.f);
    UI_CHECK(sa.fontVariationCount == 2 && sa.fontVariationTags[0] == fontTag("wdth") && sa.fontVariationValues[0] == 80.f &&
             sa.fontVariationTags[1] == fontTag("XHGT") && sa.fontVariationValues[1] == 0.5f);
    UI_CHECK(sa.fontFeatureCount == 3 && sa.fontFeatureTags[0] == fontTag("tnum") && sa.fontFeatureValues[0] == 1u &&
             sa.fontFeatureTags[1] == fontTag("liga") && sa.fontFeatureValues[1] == 0u &&
             sa.fontFeatureTags[2] == fontTag("ss01") && sa.fontFeatureValues[2] == 2u);
    UI_CHECK(ui.computed(child).fontWeight == 700.f && ui.computed(child).fontVariationCount == 2); // inherited
    UI_CHECK(ui.computed(b).fontWeight == 650.f && ui.computed(b).fontStretch == 112.5f);
    // the font a node draws with: wght from font-weight, wdth from font-variation-settings (over font-stretch)
    UI_CHECK(ui.textFont(a).variationCoords()[0] == 700.f && ui.textFont(a).variationCoords()[1] == 80.f);
    UI_CHECK(ui.textFont(b).variationCoords()[0] == 650.f && ui.textFont(b).variationCoords()[1] == 112.5f);

    // label widths follow the instance (4 x 'A' at 100 px: 4 x (300 + stem) x 0.1)
    const NodeHandle l = *ui.create("label", ui.root());
    ui.addClass(l, "big");
    ui.setText(l, "AAAA");
    ui.update(0.f);
    auto width = [&] { return ui.textLayout(l) ? ui.textLayout(l)->width : -1.f; };
    UI_CHECK(near(width(), 160.f, 0.05f));
    ui.setCss(l, "font-weight", "900");
    ui.update(0.f);
    UI_CHECK(near(width(), 240.f, 0.05f));
    ui.setCss(l, "font-variation-settings", "\"wght\" 100");
    ui.update(0.f);
    UI_CHECK(near(width(), 136.f, 0.05f));
    ui.setCss(l, "font-variation-settings", "normal");
    ui.setCss(l, "font-weight", "normal");
    ui.setCss(l, "font-stretch", "50%");
    ui.update(0.f);
    UI_CHECK(near(width(), 80.f, 0.05f));
    ui.setCss(l, "font-stretch", "normal");
    ui.update(0.f);
    UI_CHECK(near(width(), 160.f, 0.05f));
    // animated weight: monotonic over the tween, ends at the target
    ui.animate(l, UiContext::AnimProp::FontWeight, 900.f, 1.f, UiContext::Ease::Linear);
    float last = width();
    for (int k = 0; k < 10; ++k) {
        ui.update(0.1f);
        UI_CHECK(width() >= last);
        last = width();
    }
    ui.update(0.1f);
    UI_CHECK(near(width(), 240.f, 0.05f));
    std::printf("  css: AAAA at 100 px: 160 (400) -> 240 (900), tweened\n");

    // steady state: instance lookups and re-layout of seen text at seen weights allocate nothing
    TextLayout tl;
    TextLayoutDesc d;
    d.pixelSize = 24.f;
    d.maxWidth = 200.f;
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            g_allocs = 0;
            g_counting = true;
        }
        for (const float w : {100.f, 300.f, 500.f, 700.f, 900.f})
            layoutText(at(font, w), defaultShaper(), "Variable AB fonts CAB", d, tl);
        g_counting = false;
    }
    UI_CHECK_EQ(g_allocs.load(), 0L);
}

// ---- system fonts ----------------------------------------------------------------------------------------------
std::string windowsFont(const char* file) {
#if defined(_WIN32)
    const char* win = std::getenv("WINDIR");
    const std::string p = std::string(win ? win : "C:\\Windows") + "\\Fonts\\" + file;
    return std::filesystem::exists(p) ? p : std::string();
#else
    (void)file;
    return {};
#endif
}

void systemFont(const std::string& path, const char* family, float bold) {
    const auto bytes = readAll(path);
    auto font = Font::fromBytes(bytes);
    UI_CHECK(font && font->isVariable());
    if (!font) return;
    // zero deltas (gvar / HVAR / MVAR hidden) through the variation path == stb_truetype's default outlines
    auto flat = Font::fromBytes(hideTables(bytes, {"gvar", "HVAR", "MVAR"}));
    UI_CHECK(flat.has_value());
    const std::string sample = "Hamburgefonstiv 0123 &@ ÆØå ñ";
    if (flat) {
        const Font& varied = at(*flat, bold);
        UI_CHECK(&varied != &*flat);
        // The outlines are compared through MSDFs (same generator for both: equal outlines give equal texels); the
        // coverage goes through two different rasterisers (stb_truetype's, path_raster's), so only its ink is compared.
        int worstMsdf = 0, glyphs = 0;
        double inkRef = 0, inkGot = 0;
        ColorImage ma, mb;
        for (std::size_t i = 0; i < sample.size();) {
            const char32_t cp = decodeUtf8(sample, i);
            const std::uint32_t g = flat->glyphIndex(cp);
            const Raster ref = render(*flat, g, 31.f), got = render(varied, g, 31.f);
            UI_CHECK(ref.x0 == got.x0 && ref.y0 == got.y0 && ref.w == got.w && ref.h == got.h);
            inkRef += double(ref.ink());
            inkGot += double(got.ink());
            UI_CHECK(near(advanceUnits(varied, g), advanceUnits(*flat, g), 0.01f));
            const bool hasA = flat->renderMsdf(g, 32.f, 4.f, ma), hasB = varied.renderMsdf(g, 32.f, 4.f, mb);
            UI_CHECK(hasA == hasB);
            if (hasA && hasB) {
                UI_CHECK(ma.w == mb.w && ma.h == mb.h && ma.x0 == mb.x0 && ma.y0 == mb.y0);
                if (ma.rgba.size() == mb.rgba.size())
                    for (std::size_t k = 0; k < ma.rgba.size(); ++k) worstMsdf = std::max(worstMsdf, std::abs(int(ma.rgba[k]) - int(mb.rgba[k])));
            }
            ++glyphs;
        }
        UI_CHECK(worstMsdf <= 1);
        UI_CHECK(std::abs(inkGot - inkRef) <= 0.005 * inkRef);
        std::printf("  %s: decoder vs stb_truetype outlines over %d glyphs: MSDF max difference %d, ink %+.3f %%\n", family,
                    glyphs, worstMsdf, 100.0 * (inkGot - inkRef) / inkRef);
    }
    // HVAR vs the gvar phantom points
    auto noHvar = Font::fromBytes(hideTables(bytes, {"HVAR"}));
    if (noHvar) {
        float worst = 0.f;
        for (std::size_t i = 0; i < sample.size();) {
            const std::uint32_t g = font->glyphIndex(decodeUtf8(sample, i));
            worst = std::max(worst, std::abs(advanceUnits(at(*font, bold), g) - advanceUnits(at(*noHvar, bold), g)));
        }
        UI_CHECK(worst <= 1.f);
        std::printf("  %s: HVAR vs phantom advances at wght %g: max difference %.2f units\n", family, double(bold), double(worst));
    }
    // heavier instances have more ink
    long prev = 0;
    for (const float w : {300.f, 400.f, bold}) {
        long ink = 0;
        for (std::size_t i = 0; i < sample.size();) ink += render(at(*font, w), font->glyphIndex(decodeUtf8(sample, i)), 24.f).ink();
        UI_CHECK(ink > prev);
        prev = ink;
    }
#if FUSE_UI_TEST_TEXT_FULL
    { // HarfBuzz advances at the instance == HVAR advances (one letter per run: no GPOS kerning; HarfBuzz rounds the
      // delta to whole units)
        std::vector<ShapedGlyph> run;
        const Font& f = at(*font, bold);
        float worst = 0.f;
        for (const char c : std::string_view("Hamburgefonstiv")) {
            HarfBuzzShaper{}.shape(f, 40.f, std::string_view(&c, 1), run);
            if (run.size() == 1) worst = std::max(worst, std::abs(run[0].advance - f.advance(run[0].glyph, 40.f)) / f.unitScale(40.f));
        }
        UI_CHECK(worst <= 0.5f);
        std::printf("  %s: HarfBuzz vs HVAR advances: max difference %.2f units\n", family, double(worst));
    }
#endif
}

void system() {
    const std::string seg = windowsFont("SegUIVar.ttf"), bahn = windowsFont("bahnschrift.ttf");
    if (seg.empty() && bahn.empty()) {
        std::printf("  system: no Segoe UI Variable / Bahnschrift (skipped)\n");
        return;
    }
    if (!seg.empty()) {
        systemFont(seg, "Segoe UI Variable", 700.f);
        const FontCatalog& cat = FontCatalog::system();
        const FontFaceInfo* sb = cat.match("Segoe UI Variable Display", 600);
        UI_CHECK(sb && sb->namedInstance >= 0 && sb->style == "Semibold" && sb->weight == 600 && sb->variable);
        if (sb) {
            UI_CHECK_EQ(sb->fullName, std::string("Segoe UI Variable Display Semibold"));
            auto f = FontCatalog::load(*sb);
            UI_CHECK(f && f->variationCoords().size() == 2);
            if (f && f->variationCoords().size() == 2) {
                const auto axes = f->axes();
                for (std::size_t k = 0; k < axes.size(); ++k) {
                    if (axes[k].tag == fontTag("wght")) UI_CHECK(f->variationCoords()[k] == 600.f);
                    if (axes[k].tag == fontTag("opsz")) UI_CHECK(f->variationCoords()[k] == 36.f);
                }
                // pinned at Semibold: wider than the default instance
                auto def = Font::fromFile(sb->path);
                const std::uint32_t H = f->glyphIndex(U'H');
                UI_CHECK(def && f->advance(H, 20.f) > def->advance(H, 20.f));
            }
        }
        const FontFaceInfo* text = cat.match("Segoe UI Variable Text", 400);
        UI_CHECK(text && text->style == "Regular" && text->namedInstance >= 0);
        UI_CHECK(cat.match("Segoe UI Variable Small", 300) && cat.match("Segoe UI Variable Small", 300)->style == "Light");
        std::size_t named = 0;
        for (const FontFaceInfo& fi : cat.faces()) named += fi.variable && fi.namedInstance >= 0;
        std::printf("  catalog: %zu named-instance faces; Segoe UI Variable Display Semibold -> wght 600, opsz 36\n", named);
    }
    if (!bahn.empty()) {
        systemFont(bahn, "Bahnschrift", 700.f);
        auto f = Font::fromFile(bahn);
        if (f) { // narrower with wdth
            const std::uint32_t o = f->glyphIndex(U'o');
            UI_CHECK(at(*f, 400.f, 75.f).advance(o, 20.f) < f->advance(o, 20.f));
        }
        const FontFaceInfo* c = FontCatalog::system().match("Bahnschrift", 600);
        UI_CHECK(c != nullptr);
        bool condensed = false;
        for (const FontFaceInfo& fi : FontCatalog::system().faces())
            condensed |= fi.family == "Bahnschrift" && fi.style == "Bold Condensed" && fi.stretch == 3 && fi.weight == 700;
        UI_CHECK(condensed);
    }
}

} // namespace

int main() {
#if FUSE_UI_TEST_TEXT_COOKED
    // packs hold one instance: the one fuse_text_cook --variation=wght:700 baked
    enableColorGlyphs();
    auto pack = Font::fromFile(fuse::ui::test::testFontPath("variable_test"));
    UI_CHECK(pack.has_value());
    if (pack) {
        UI_CHECK(!pack->isVariable() && &pack->instance(std::array{FontVariation{fontTag("wght"), 900.f}}) == &*pack);
        const std::uint32_t A = pack->glyphIndex(U'A');
        UI_CHECK(near(advanceUnits(*pack, A), std::round(300.f + stemOf(700.f)), 0.01f));
        UI_CHECK(near(pack->metrics(kPx).ascent, std::round(800.f + 100.f * normWeight(700.f)) * 0.1f, 0.01f));
        UI_CHECK(near(render(*pack, A, kPx).rowCoverage(-35), stemOf(700.f) * 0.1f, 0.06f));
        ColorImage img;
        const std::uint32_t g = pack->glyphIndex(0xE000);
        UI_CHECK(pack->renderColor(g, kPx, 0xFF000000u, img));
        const int red = int(std::lround((0.6f + 0.4f * normWeight(700.f)) * 255.f));
        UI_CHECK(close(sample(img, 125.f, 400.f), {red, 0, 0, red}));
        std::printf("  cooked at wght 700: advance A %.1f units, ascent %.2f px\n", double(advanceUnits(*pack, A)),
                    double(pack->metrics(kPx).ascent));
    }
    return finish("fuse_text_variable");
#else
    enableVariableFonts();
    enableColorGlyphs();
    enableMsdf();
    const auto bytes = readAll("data/variable_test.ttf");
    auto font = Font::fromBytes(bytes);
    if (!font) {
        std::fprintf(stderr, "data/variable_test.ttf missing\n");
        return 1;
    }
    axes(*font);
    metrics(*font);
    outlines(*font, bytes);
    paints(*font);
    css(*font);
    system();
    return finish("fuse_text_variable");
#endif
}
