// fuse_text_features: OpenType features through CSS font-feature-settings (FULL: HarfBuzz). data/features_test.ttf
// comes from data/make_features_test_font.py (liga f_i, tnum, smcp, ss01 and a kern pair, told apart by advances).
//   font      Font::withFeatures: defaults (liga, kern on), "liga" 0, "kern" 0, tnum, smcp, ss01 (and ss01 2 / 0),
//             variants cached per feature list, run cache entries kept apart; Open Sans tnum / pnum digits
//   css       a label with font-feature-settings: "tnum" lays out with the tabular digit, "liga" 0 keeps f i apart;
//             relaying out seen text with the variants allocates nothing
// MINIMAL / COOKED have no GSUB / GPOS: withFeatures returns the font itself, then the gate exits 77.
#include "test_util.hpp"

#include <array>
#include <atomic>
#include <cmath>
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
constexpr float kPx = 100.f; // ascent - descent = 1000 units: 0.1 px per unit

// Advances (font units) of `s` shaped with `f` and the default shaper.
std::vector<int> advances(const Font& f, std::string_view s) {
    std::vector<ShapedGlyph> run;
    defaultShaper().shape(f, kPx, s, run);
    std::vector<int> out;
    for (const ShapedGlyph& g : run) out.push_back(int(std::lround(g.advance * 10.f)));
    return out;
}

const Font& with(const Font& f, std::initializer_list<FontFeature> ft) { return f.withFeatures(std::span(ft.begin(), ft.size())); }
} // namespace

int main() {
    if (textProfile() == TextProfile::Cooked) { // packs hold pre-shaped advances only
        std::printf("fuse_text_features: skipped (COOKED profile: no OpenType shaping)\n");
        return 77;
    }
    auto font = Font::fromFile("data/features_test.ttf");
    if (!font) {
        std::fprintf(stderr, "data/features_test.ttf missing\n");
        return 1;
    }
    const Font& f = *font;
    if (&with(f, {{fontTag("tnum")}}) == &f) {
        std::printf("fuse_text_features: skipped (%s profile: no OpenType shaping)\n", textProfileName(textProfile()));
        return 77;
    }
    using V = std::vector<int>;
    UI_CHECK(advances(f, "fi") == V{500});                                        // liga on by default
    UI_CHECK(advances(with(f, {{fontTag("liga"), 0}}), "fi") == V({400, 300}));
    UI_CHECK(advances(f, "Aa") == V({500, 500}));                                 // kern A a -100 on by default
    UI_CHECK(advances(with(f, {{fontTag("kern"), 0}}), "Aa") == V({600, 500}));
    UI_CHECK(advances(f, "11") == V({300, 300}));
    UI_CHECK(advances(with(f, {{fontTag("tnum")}}), "11") == V({600, 600}));
    UI_CHECK(advances(with(f, {{fontTag("smcp")}}), "a") == V{450});
    UI_CHECK(advances(with(f, {{fontTag("ss01")}}), "a") == V{550});
    UI_CHECK(advances(with(f, {{fontTag("ss01"), 0}}), "a") == V{500});
    UI_CHECK(advances(with(f, {{fontTag("tnum")}, {fontTag("smcp")}, {fontTag("liga"), 0}}), "fi1a") == V({400, 300, 600, 450}));
    // one variant per feature list; a variant's features replace its own
    const Font& tn = with(f, {{fontTag("tnum")}});
    UI_CHECK(&with(f, {{fontTag("tnum")}}) == &tn && tn.id() != f.id());
    UI_CHECK(advances(with(tn, {{fontTag("smcp")}}), "1a") == V({300, 450}));
    // the default shaping is unchanged after the variants shared the run cache
    UI_CHECK(advances(f, "fi11a") == V({500, 300, 300, 500}));
    // Open Sans: tabular digits by default, pnum makes "1" narrower than "0"
    if (auto os = Font::bundled()) {
        const V def = advances(*os, "10"), pn = advances(with(*os, {{fontTag("pnum")}}), "10");
        UI_CHECK(def.size() == 2 && pn.size() == 2 && def[0] == def[1] && pn[0] < pn[1]);
        std::printf("  Open Sans \"10\" at 100 px: default %d / %d, pnum %d / %d (0.1 px)\n", def[0], def[1], pn[0], pn[1]);
    }

    // CSS: font-feature-settings reaches layout
    UiContext ui{{.viewport = {800, 400}, .font = &f}};
    const NodeHandle l = *ui.create("label", ui.root());
    ui.setCss(l, "font-size", "100px");
    ui.setText(l, "fi11");
    ui.update(0.f);
    auto width = [&] { return ui.textLayout(l) ? ui.textLayout(l)->width : -1.f; };
    UI_CHECK(std::abs(width() - 110.f) < 0.05f); // f_i 500 + 300 + 300
    ui.setCss(l, "font-feature-settings", "\"tnum\", \"liga\" off");
    ui.update(0.f);
    UI_CHECK(std::abs(width() - 190.f) < 0.05f); // 400 + 300 + 600 + 600
    ui.setCss(l, "font-feature-settings", "normal");
    ui.update(0.f);
    UI_CHECK(std::abs(width() - 110.f) < 0.05f);
    std::printf("  css: \"fi11\" at 100 px: 110 px, with \"tnum\", \"liga\" off: 190 px\n");

    // steady state: variant lookups and re-layout of seen text allocate nothing
    TextLayout tl;
    TextLayoutDesc d;
    d.pixelSize = 20.f;
    const std::array<FontFeature, 2> sets[] = {{FontFeature{fontTag("tnum")}, FontFeature{fontTag("liga"), 0}},
                                               {FontFeature{fontTag("smcp")}, FontFeature{fontTag("ss01")}}};
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            g_allocs = 0;
            g_counting = true;
        }
        for (const auto& s : sets) layoutText(f.withFeatures(s), defaultShaper(), "fi 11 a Aa", d, tl);
        g_counting = false;
    }
    UI_CHECK_EQ(g_allocs.load(), 0L);
    return finish("fuse_text_features");
}
