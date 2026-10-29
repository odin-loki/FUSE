// fuse_ui_text_full: the FULL text profile (HarfBuzz shaping + FreeType coverage). In MINIMAL builds it checks that
// fullShaper() is absent and HarfBuzzShaper falls back to the minimal shaper.
#include "test_util.hpp"

#include <cmath>

using namespace fuse::text;
using fuse::ui::test::font;

int main() {
    const Font& f = font();
    std::vector<ShapedGlyph> a, b;
#if FUSE_UI_TEST_TEXT_FULL
    UI_CHECK(textProfile() == TextProfile::Full);
    UI_CHECK(fullShaper() != nullptr);
    // HarfBuzz and the cmap+kern shaper agree on simple Latin within a pixel per glyph.
    const float wh = fullShaper()->shape(f, 16.f, "Hello", a);
    const float wm = MinimalShaper{}.shape(f, 16.f, "Hello", b);
    UI_CHECK(a.size() == 5 && std::abs(wh - wm) < 5.f);
    // Clusters are byte offsets and non-decreasing for an LTR run; multibyte input keeps byte clusters.
    fullShaper()->shape(f, 16.f, "a\xC3\xA9z", a);
    UI_CHECK(a.size() == 3 && a[0].cluster == 0 && a[1].cluster == 1 && a[2].cluster == 3);
    // Combining acute after 'e': the mark gets zero advance (GPOS / mark positioning) or composes into U+00E9.
    fullShaper()->shape(f, 16.f, "e\xCC\x81", a);
    UI_CHECK(a.size() == 1 || (a.size() == 2 && a[1].advance == 0.f));
    // FreeType coverage: a visible glyph has a non-empty box and some ink.
    const std::uint32_t g = f.glyphIndex('A');
    int x0, y0, x1, y1;
    f.glyphBox(g, 24.f, x0, y0, x1, y1);
    UI_CHECK(x1 > x0 && y1 > y0 && y0 < 0);
    std::vector<std::uint8_t> px(std::size_t((x1 - x0) * (y1 - y0)), 0);
    f.rasterise(g, 24.f, px.data(), x1 - x0, y1 - y0, x1 - x0);
    int ink = 0;
    for (auto v : px) ink += v > 128;
    UI_CHECK(ink > 10);
    // Steady state: reshaping into a buffer with capacity does not allocate (capacity kept).
    const auto cap = a.capacity();
    fullShaper()->shape(f, 16.f, "e", a);
    UI_CHECK(a.capacity() == cap);
#else
    UI_CHECK(textProfile() == TextProfile::Minimal);
    UI_CHECK(fullShaper() == nullptr);
    UI_CHECK(HarfBuzzShaper{}.shape(f, 16.f, "Hello", a) == MinimalShaper{}.shape(f, 16.f, "Hello", b));
#endif
    return fuse::ui::test::finish("fuse_ui_text_full");
}
