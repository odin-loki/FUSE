// Colour-management helpers (sRGB / P3 / Rec.2020, PQ, HLG) and SVG elliptical arcs (Path::arcTo, "A" commands).
#include "fuse/ui/color_space.hpp"
#include "fuse/ui/draw_list.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace fuse::ui;
namespace C = fuse::ui::color;

namespace {
bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::abs(a - b) <= eps; }
f32 area(const Path& p) { // shoelace over the flattened contours (signed; closing edges included)
    double s = 0;
    for (const PathEdge& e : p.edges()) s += double(e.x0) * e.y1 - double(e.x1) * e.y0;
    return f32(std::abs(s) * 0.5);
}
} // namespace

int main() {
    {   // Hdr10Packer matches toHdr10() to within 2 of 1023 on every channel, alpha in 2 bits
        const C::Hdr10Packer pk;
        int worst = 0;
        for (u32 v = 0; v < 256; v += 3)
            for (u32 ch = 0; ch < 3; ++ch) {
                const u32 px = (v << (8 * ch)) | (((v * 7) & 0xFFu) << (8 * ((ch + 1) % 3))) | 0xFF000000u;
                const u32 o = pk.pack(px);
                const Color ref = C::toHdr10({f32(px & 0xFF) / 255.f, f32((px >> 8) & 0xFF) / 255.f,
                                              f32((px >> 16) & 0xFF) / 255.f, 1.f});
                const f32 want[3] = {ref.r, ref.g, ref.b};
                for (int k = 0; k < 3; ++k)
                    worst = std::max(worst, std::abs(int((o >> (10 * k)) & 1023u) -
                                                     int(std::lround(std::clamp(want[k], 0.f, 1.f) * 1023.f))));
                UI_CHECK((o >> 30) == 3u);
            }
        UI_CHECK(worst <= 2);
        UI_CHECK(pk.pack(0xFF000000u) == 0xC0000000u);
    }
    // transfer curves round-trip and hit the reference points
    for (f32 v : {0.f, 0.02f, 0.2f, 0.5f, 0.8f, 1.f}) {
        UI_CHECK(near(C::linearToSrgb(C::srgbToLinear(v)), v, 1e-5f));
        UI_CHECK(near(C::linearToRec2020(C::rec2020ToLinear(v)), v, 1e-5f));
        UI_CHECK(near(C::hlgDecode(C::hlgEncode(v)), v, 1e-5f));
    }
    UI_CHECK(near(C::srgbToLinear(0.5f), 0.2140f));
    UI_CHECK(near(C::pqEncode(10000.f), 1.f, 1e-5f));
    UI_CHECK(near(C::pqEncode(100.f), 0.5081f));                 // ST 2084 table
    UI_CHECK(near(C::pqDecode(C::pqEncode(203.f)), 203.f, 0.05f));
    UI_CHECK(near(C::hlgEncode(1.f / 12.f), 0.5f, 1e-5f));
    // gamut: white stays white, sRGB red is inside P3 (~0.9175, 0.2003, 0.1386 encoded), and round-trips
    const Color w = C::convert({1, 1, 1, 1}, C::Space::Srgb, C::Space::DisplayP3);
    UI_CHECK(near(w.r, 1.f) && near(w.g, 1.f) && near(w.b, 1.f));
    const Color red = C::convert({1, 0, 0, 0.5f}, C::Space::Srgb, C::Space::DisplayP3);
    UI_CHECK(near(red.r, 0.9175f, 2e-3f) && near(red.g, 0.2003f, 2e-3f) && near(red.b, 0.1386f, 2e-3f));
    UI_CHECK(red.a == 0.5f);
    const Color back = C::convert(red, C::Space::DisplayP3, C::Space::Srgb);
    UI_CHECK(near(back.r, 1.f, 2e-3f) && near(back.g, 0.f, 2e-3f) && near(back.b, 0.f, 2e-3f));
    UI_CHECK(near(C::contrastRatio({0, 0, 0, 1}, {1, 1, 1, 1}), 21.f, 1e-3f));
    UI_CHECK(near(C::toHdr10({1, 1, 1, 1}).g, C::pqEncode(203.f), 1e-3f));

    // arcs: two half arcs make a circle of r = 10 (area ~ pi r^2), bounds 20 x 20
    Path circle = Path::fromSvg("M 0 10 A 10 10 0 0 1 20 10 A 10 10 0 0 1 0 10 Z");
    const Rect b = circle.bounds();
    UI_CHECK(near(b.x, 0.f, 0.01f) && near(b.w, 20.f, 0.05f) && near(b.y, 0.f, 0.05f) && near(b.h, 20.f, 0.05f));
    UI_CHECK(near(area(circle), 3.14159265f * 100.f, 4.f));
    // sweep=1 from (0,10) to (20,10) goes through the top (y-down screen space: y < 10)
    Path top = Path::fromSvg("M0 10 A10 10 0 0 1 20 10");
    UI_CHECK(near(top.bounds().y, 0.f, 0.05f) && top.bounds().bottom() <= 10.01f);
    // large-arc flag on a small radius: radii scaled up to reach (half circle), and large vs small arcs differ
    Path scaled = Path::fromSvg("M0 0 a1 1 0 0 0 40 0");
    UI_CHECK(near(scaled.bounds().h, 20.f, 0.06f));
    Path small = Path::fromSvg("M0 0 A30 30 0 0 1 40 0"), large = Path::fromSvg("M0 0 A30 30 0 1 1 40 0");
    UI_CHECK(large.bounds().h > small.bounds().h + 20.f);
    // rotated ellipse: rx 20, ry 5 rotated 90 degrees -> tall
    Path rot = Path::fromSvg("M0 0 A20 5 90 1 1 0 40 A20 5 90 1 1 0 0 Z");
    UI_CHECK(rot.bounds().h > 39.f && rot.bounds().w < 11.f);
    // zero radius = straight line; compact flags ("a1 1 0 01 ...") still parse
    {
        int strokeEdges = 0;
        for (const PathEdge& e : Path::fromSvg("M0 0 A0 5 0 0 1 10 0").edges()) strokeEdges += e.fillOnly == 0.f;
        UI_CHECK_EQ(strokeEdges, 1);
    }
    Path compact;
    UI_CHECK(compact.appendSvg("M0 10a10 10 0 0110 -10"));
    UI_CHECK(compact.edges().size() > 2u);
    std::printf("color + arc checks done\n");
    return fuse::ui::test::failures();
}
