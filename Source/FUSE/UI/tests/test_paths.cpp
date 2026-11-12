// Gate fuse_ui_paths: vector paths in the draw list and the CPU reference rasteriser (plan §4.9).
//   * Path building, SVG path data (valid and malformed), flattening
//   * fill coverage of known shapes (circle = pi r^2, rect, rotated / scaled shapes), non-zero vs even-odd on a star
//   * strokes: width, butt / square / round caps, miter / bevel / round joins, miter limit, dashes (+ offset)
//   * N-stop linear / radial / conic gradients (pad / repeat / reflect, premultiplied interpolation), gradientRect
//   * clip to path, per-corner rounded clips, opacity layers
//   * 9-slice and sub-rect images
//   * DrawList::translate / scale carry every vector op: a list scaled 2x renders exactly what the same shapes
//     recorded at double size render; a scissored render equals the full render inside the scissor
//   * the coverage cache: a shape redrawn at another whole-pixel position is a hit and renders identically
//   * robustness: NaN / huge coordinates, empty paths, unbalanced groups
#include "test_util.hpp"

#include <cmath>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
constexpr f32 kPi = 3.14159265358979f;
u8 ch(u32 p, int c) { return u8((p >> (8 * c)) & 0xFF); }
const Color kWhite = Color::rgb(0xFFFFFF);

Image render(const DrawList& dl, int w, int h, u32 clear = 0, Rect scissor = {}, const Image* img0 = nullptr) {
    Image img;
    img.resize(w, h, clear);
    CpuRasterizer r;
    if (img0) r.addImage(img0);
    r.render(dl, img, scissor);
    return img;
}
/// Sum of alpha / 255: the covered area in pixels when drawn opaque on transparent.
double area(const Image& img) {
    double a = 0.0;
    for (u32 p : img.pixels) a += double(p >> 24) / 255.0;
    return a;
}
double fillArea(const Path& p, FillRule rule = FillRule::NonZero, const Affine& xf = {}, int w = 96, int h = 96) {
    DrawList dl;
    dl.fillPath(p, kWhite, rule, xf);
    return area(render(dl, w, h));
}
double strokeArea(const Path& p, const StrokeStyle& st, int w = 128, int h = 96) {
    DrawList dl;
    dl.strokePath(p, st, kWhite);
    return area(render(dl, w, h));
}
bool nearRel(double v, double want, double rel) { return std::abs(v - want) <= rel * std::abs(want); }
#define CHECK_NEAR(v, want, rel)                                                                                        \
    do {                                                                                                                \
        const double v_ = (v), w_ = (want);                                                                             \
        if (!nearRel(v_, w_, (rel))) {                                                                                  \
            std::fprintf(stderr, "%s:%d: %s = %.4f, want %.4f (+-%.2f%%)\n", __FILE__, __LINE__, #v, v_, w_, (rel) * 100.0); \
            ++failures();                                                                                               \
        }                                                                                                               \
    } while (0)

Path star(Vec2 c, f32 r) { // pentagram: every second vertex of a regular pentagon
    Path p;
    for (int i = 0; i < 5; ++i) {
        const f32 a = -kPi / 2.f + f32(i * 2) * 2.f * kPi / 5.f;
        const Vec2 v{c.x + r * std::cos(a), c.y + r * std::sin(a)};
        if (i == 0) p.moveTo(v);
        else p.lineTo(v);
    }
    return p.close();
}
int maxDiff(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height) return 999;
    int m = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i)
        for (int c = 0; c < 4; ++c) m = std::max(m, std::abs(int(ch(a.pixels[i], c)) - int(ch(b.pixels[i], c))));
    return m;
}
} // namespace

int main() {
    // ---- Affine ----
    {
        const Affine t = Affine::translation(10, 5) * Affine::scaling(2, 3);
        UI_CHECK(t.apply({1, 1}) == (Vec2{12, 8}));
        Affine inv;
        UI_CHECK(t.invert(inv));
        const Vec2 back = inv.apply(t.apply({7, -3}));
        UI_CHECK(std::abs(back.x - 7.f) < 1e-4f && std::abs(back.y + 3.f) < 1e-4f);
        UI_CHECK(!Affine::scaling(0, 1).invert(inv));
        UI_CHECK(Affine::rotation(0.7f).isSimilarity() && !Affine::scaling(2, 1).isSimilarity());
        UI_CHECK(std::abs(Affine::scaling(2, 5).maxScale() - 5.f) < 1e-4f);
        const Vec2 r = Affine::rotation(kPi / 2.f).apply({1, 0}); // +x turns towards +y
        UI_CHECK(std::abs(r.x) < 1e-5f && std::abs(r.y - 1.f) < 1e-5f);
    }
    // ---- Path building + SVG path data ----
    {
        Path p;
        p.moveTo({1, 2}).lineTo({5, 2}).quadTo({7, 2}, {7, 4}).cubicTo({7, 6}, {5, 8}, {3, 8}).close();
        UI_CHECK_EQ(p.verbs().size(), std::size_t(5));
        UI_CHECK_EQ(p.points().size(), std::size_t(7));
        UI_CHECK_RECT(p.bounds(), 1, 2, 6, 6);
        p.lineTo({9, 9}); // after close(): continues from the sub-path start
        UI_CHECK(p.verbs()[5] == PathVerb::Move && p.points()[7] == (Vec2{1, 2}));

        auto a = Path::fromSvg("M10 10 L20 10 20 20 H5 V15 Z m 2 2 l 1 0 0 1 z");
        UI_CHECK(a.has_value());
        if (a) {
            UI_CHECK_EQ(a->verbs().size(), std::size_t(10));
            UI_CHECK(a->points()[3] == (Vec2{5, 20}) && a->points()[4] == (Vec2{5, 15}));
            UI_CHECK(a->points()[5] == (Vec2{12, 12})); // relative to the closed sub-path's start
        }
        auto b = Path::fromSvg("M.5.5-1-1"); // compact numbers: (0.5, 0.5) then an implicit lineto (-1, -1)
        UI_CHECK(b && b->points().size() == 2 && b->points()[1] == (Vec2{-1, -1}));
        auto c = Path::fromSvg("M0 0 C1 1 2 2 3 3 S5 5 6 6 Q7 7 8 8 T10 10");
        UI_CHECK(c && c->verbs().size() == 5);
        if (c) UI_CHECK(c->points()[4] == (Vec2{4, 4})); // S reflects the previous control point
        auto arc = Path::fromSvg("M0 10 A10 10 0 0 1 20 10"); // upper half circle (sweep = clockwise on screen)
        UI_CHECK(arc.has_value());
        if (arc) {
            const Rect bb = arc->bounds();
            UI_CHECK(std::abs(bb.y) < 0.01f && std::abs(bb.bottom() - 10.f) < 0.01f && std::abs(bb.w - 20.f) < 0.01f);
            UI_CHECK(arc->points().back() == (Vec2{20, 10}));
        }
        auto flags = Path::fromSvg("M0 0a5 5 0 1110 0"); // arc flags without separators
        UI_CHECK(flags && flags->points().back() == (Vec2{10, 0}));
        // malformed: fails with the offset, never crashes
        for (const char* bad : {"L10 10", "M10", "M10 10 L", "M10 10 X5", "M 1 2 L 3 abc", "M0 0 A1 1 0 2 0 5 5", "10 10", "M0 0 Z 5 5",
                                "M1e999 0", "M0 0 L1,,2"}) {
            auto r = Path::fromSvg(bad);
            if (r) std::fprintf(stderr, "accepted malformed path data: %s\n", bad);
            UI_CHECK(!r.has_value());
        }
        UI_CHECK(Path::fromSvg("").has_value() && Path::fromSvg("  ").value().empty());
        auto err = Path::fromSvg("M10 10 L20 20 Q");
        UI_CHECK(!err && err.error().offset == 15u);

        Polyline flat;
        Path circle;
        circle.addCircle({0, 0}, 100.f);
        flattenPath(circle, 0.1f, flat);
        UI_CHECK(flat.contours.size() == 1 && flat.contours[0].closed);
        f32 worst = 0.f; // every flattened vertex lies on the circle, every chord midpoint within the tolerance
        for (u32 i = 0; i < flat.contours[0].count; ++i) {
            const Vec2 p0 = flat.points[i], p1 = flat.points[(i + 1) % flat.contours[0].count];
            worst = std::max(worst, std::abs(std::hypot((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f) - 100.f));
        }
        UI_CHECK(worst < 0.15f);
        Polyline coarse;
        flattenPath(circle, 2.f, coarse);
        UI_CHECK(coarse.points.size() < flat.points.size()); // the tolerance drives the segment count
    }
    // ---- fill coverage ----
    {
        Path c;
        c.addCircle({48.3f, 47.6f}, 30.f);
        CHECK_NEAR(fillArea(c), kPi * 900.0, 0.003);
        Path e;
        e.addEllipse({48, 48}, 40.f, 12.5f);
        CHECK_NEAR(fillArea(e), kPi * 40.0 * 12.5, 0.004);
        Path r;
        r.addRect({10.25f, 20.5f, 30.5f, 17.25f});
        CHECK_NEAR(fillArea(r), 30.5 * 17.25, 0.0005);
        Path rr;
        rr.addRoundedRect({10, 10, 60, 40}, 10.f);
        CHECK_NEAR(fillArea(rr), 60.0 * 40.0 - (4.0 - kPi) * 100.0, 0.003);
        // exact partial coverage: a rect covering the left 0.25 of a pixel column
        DrawList dl;
        Path q;
        q.addRect({3.75f, 2.f, 1.25f, 2.f});
        dl.fillPath(q, kWhite);
        const Image img = render(dl, 8, 8);
        UI_CHECK_EQ(ch(img.at(3, 2), 3), u8(64)); // 0.25 * 255 = 63.75
        UI_CHECK_EQ(ch(img.at(4, 2), 3), u8(255));
        UI_CHECK_EQ(img.at(5, 2), 0u);
        UI_CHECK_EQ(img.at(4, 1), 0u);
        // transforms: rotation keeps the area, scale multiplies it
        Path sq;
        sq.addRect({-15, -15, 30, 30});
        CHECK_NEAR(fillArea(sq, FillRule::NonZero, Affine::translation(48, 48) * Affine::rotation(0.6f)), 900.0, 0.002);
        CHECK_NEAR(fillArea(c, FillRule::NonZero, Affine::scaling(0.5f, 0.25f)), kPi * 900.0 / 8.0, 0.005);
        // fill rules on a pentagram
        const Path st = star({48, 48}, 40.f);
        const Image nz = [&] { DrawList d; d.fillPath(st, kWhite, FillRule::NonZero); return render(d, 96, 96); }();
        const Image eo = [&] { DrawList d; d.fillPath(st, kWhite, FillRule::EvenOdd); return render(d, 96, 96); }();
        UI_CHECK_EQ(ch(nz.at(48, 48), 3), u8(255)); // the inner pentagon: winding 2
        UI_CHECK_EQ(ch(eo.at(48, 48), 3), u8(0));
        UI_CHECK_EQ(ch(eo.at(48, 16), 3), u8(255)); // a point of the star: winding 1 in both
        UI_CHECK_EQ(ch(nz.at(48, 16), 3), u8(255));
        const double inner = 40.0 * std::cos(2.0 * kPi / 5.0) / std::cos(kPi / 5.0); // inner pentagon circumradius
        CHECK_NEAR(area(nz) - area(eo), 2.5 * inner * inner * std::sin(2.0 * kPi / 5.0), 0.01);
        // a hole wound the other way is a hole under both rules
        Path ring;
        ring.addRect({10, 10, 60, 60});
        ring.moveTo({30, 30}).lineTo({30, 50}).lineTo({50, 50}).lineTo({50, 30}).close();
        CHECK_NEAR(fillArea(ring, FillRule::NonZero), 3600.0 - 400.0, 0.0005);
        CHECK_NEAR(fillArea(ring, FillRule::EvenOdd), 3600.0 - 400.0, 0.0005);
    }
    // ---- strokes: width, caps, joins, miter limit, dashes ----
    {
        Path line;
        line.moveTo({20, 40.5f}).lineTo({80, 40.5f});
        StrokeStyle st;
        st.width = 6.f;
        CHECK_NEAR(strokeArea(line, st), 60.0 * 6.0, 0.001);
        { // the cross-section is `width` pixels
            DrawList dl;
            dl.strokePath(line, st, kWhite);
            const Image img = render(dl, 128, 96);
            double col = 0.0;
            for (int y = 0; y < 96; ++y) col += double(ch(img.at(50, y), 3)) / 255.0;
            CHECK_NEAR(col, 6.0, 0.002);
            UI_CHECK_EQ(img.at(19, 40), 0u); // butt: nothing beyond the end points
            UI_CHECK_EQ(img.at(80, 40), 0u);
        }
        st.cap = LineCap::Square;
        CHECK_NEAR(strokeArea(line, st), 66.0 * 6.0, 0.001);
        st.cap = LineCap::Round;
        CHECK_NEAR(strokeArea(line, st), 60.0 * 6.0 + kPi * 9.0, 0.003);
        // a zero-length sub-path: a dot with round / square caps, nothing with butt caps
        Path dot;
        dot.moveTo({40, 40}).lineTo({40, 40});
        st.width = 10.f;
        CHECK_NEAR(strokeArea(dot, st), kPi * 25.0, 0.01);
        st.cap = LineCap::Square;
        CHECK_NEAR(strokeArea(dot, st), 100.0, 0.001);
        st.cap = LineCap::Butt;
        UI_CHECK(strokeArea(dot, st) == 0.0);

        // joins on a right angle, width 8: miter = the full corner square, bevel cuts half of it, round a quarter disc
        Path corner;
        corner.moveTo({20, 30}).lineTo({60, 30}).lineTo({60, 70});
        st = {};
        st.width = 8.f;
        st.join = LineJoin::Miter;
        CHECK_NEAR(strokeArea(corner, st), 80.0 * 8.0, 0.001);
        st.join = LineJoin::Bevel;
        CHECK_NEAR(strokeArea(corner, st), 80.0 * 8.0 - 8.0, 0.001);
        st.join = LineJoin::Round;
        CHECK_NEAR(strokeArea(corner, st), 80.0 * 8.0 - (16.0 - kPi * 4.0), 0.002);
        // closed shape: every vertex is joined (a square outline = outer square - inner square)
        Path box;
        box.addRect({30, 20, 40, 40});
        st.join = LineJoin::Miter;
        CHECK_NEAR(strokeArea(box, st), 48.0 * 48.0 - 32.0 * 32.0, 0.001);
        // miter limit: a 30 degree spike has a miter ratio of 1 / sin(15 deg) = 3.86
        Path spike;
        spike.moveTo({20, 60}).lineTo({70, 60}).lineTo({20.f + 50.f * (1.f - std::cos(kPi / 6.f)), 60.f - 50.f * std::sin(kPi / 6.f)});
        st.width = 6.f;
        st.miterLimit = 4.f;
        const double mitered = strokeArea(spike, st);
        st.miterLimit = 3.5f;
        const double bevelled = strokeArea(spike, st);
        UI_CHECK(mitered > bevelled + 20.0); // the long tip is gone
        { // and the tip pixel itself: beyond the corner along the bisector
            DrawList a, b;
            st.miterLimit = 4.f;
            a.strokePath(spike, st, kWhite);
            st.miterLimit = 3.5f;
            b.strokePath(spike, st, kWhite);
            const Vec2 tip{70.f + 8.f * std::cos(kPi / 12.f), 60.f + 8.f * std::sin(kPi / 12.f)};
            UI_CHECK(ch(render(a, 128, 96).at(int(tip.x), int(tip.y)), 3) > 200);
            UI_CHECK_EQ(render(b, 128, 96).at(int(tip.x), int(tip.y)), 0u);
        }
        // dashes: 10 on / 5 off over a length of 100 -> 70 on; with an offset of 5 -> 65 on
        Path longLine;
        longLine.moveTo({10, 20}).lineTo({110, 20});
        st = {};
        st.width = 4.f;
        const f32 pattern[2]{10.f, 5.f};
        st.setDashes(pattern);
        CHECK_NEAR(strokeArea(longLine, st), 70.0 * 4.0, 0.001);
        st.setDashes(pattern, 5.f);
        CHECK_NEAR(strokeArea(longLine, st), 65.0 * 4.0, 0.001);
        {
            DrawList dl;
            dl.strokePath(longLine, st, kWhite);
            const Image img = render(dl, 128, 96);
            UI_CHECK_EQ(ch(img.at(12, 20), 3), u8(255)); // on: 0..5
            UI_CHECK_EQ(img.at(17, 20), 0u);             // off: 5..10
            UI_CHECK_EQ(ch(img.at(24, 20), 3), u8(255)); // on: 10..20
        }
        const f32 odd[3]{6.f, 2.f, 4.f}; // an odd pattern repeats: 6 on 2 off 4 on 6 off 2 on 4 off (period 24, 12 on)
        st.setDashes(odd);
        CHECK_NEAR(strokeArea(longLine, st), (4.0 * 12.0 + 4.0) * 4.0, 0.001); // 4 periods, then 4 of the next 6 on
        // dashes follow curves: a dashed circle covers about on / (on + off) of the solid ring
        Path circle;
        circle.addCircle({64, 48}, 30.f);
        st = {};
        st.width = 3.f;
        const double solid = strokeArea(circle, st);
        CHECK_NEAR(solid, 2.0 * kPi * 30.0 * 3.0, 0.003);
        const f32 half[2]{5.f, 5.f};
        st.setDashes(half);
        CHECK_NEAR(strokeArea(circle, st), solid * 0.5, 0.04);
        // under a similarity transform the width scales with it
        DrawList dl;
        st = {};
        st.width = 3.f;
        dl.strokePath(line, st, kWhite, Affine::scaling(0.5f, 0.5f));
        CHECK_NEAR(area(render(dl, 128, 96)), 30.0 * 1.5, 0.002);
    }
    // ---- gradients ----
    {
        const GradientStop rgb[3]{{0.f, Color::rgb(0xFF0000)}, {0.5f, Color::rgb(0x00FF00)}, {1.f, Color::rgb(0x0000FF)}};
        DrawList dl;
        dl.gradientRect({0, 0, 100, 10}, Gradient::linear({0, 0}, {100, 0}, rgb));
        Image img = render(dl, 100, 10);
        auto px = [&](int x, int y = 5) { return img.at(x, y); };
        UI_CHECK(ch(px(0), 0) >= 251 && ch(px(0), 1) <= 4 && ch(px(0), 3) == 255);
        UI_CHECK(std::abs(int(ch(px(24), 0)) - 130) <= 2 && std::abs(int(ch(px(24), 1)) - 125) <= 2); // t = 0.245
        UI_CHECK(ch(px(50), 1) >= 250 && ch(px(50), 0) <= 3 && ch(px(50), 2) <= 3);
        UI_CHECK(std::abs(int(ch(px(74), 1)) - 130) <= 2 && std::abs(int(ch(px(74), 2)) - 125) <= 2);
        UI_CHECK(ch(px(99), 2) >= 251);
        // pad / repeat / reflect outside 0..1
        const GradientStop bw[2]{{0.f, Color::rgb(0x000000)}, {1.f, Color::rgb(0xFFFFFF)}};
        auto strip = [&](GradientSpread s) {
            DrawList d;
            d.gradientRect({0, 0, 60, 4}, Gradient::linear({0, 0}, {20, 0}, bw, s));
            return render(d, 60, 4);
        };
        const Image pad = strip(GradientSpread::Pad), rep = strip(GradientSpread::Repeat), ref = strip(GradientSpread::Reflect);
        UI_CHECK(ch(pad.at(45, 1), 0) == 255);
        UI_CHECK(std::abs(int(ch(rep.at(25, 1), 0)) - 70) <= 2); // t = 1.275 -> 0.275
        UI_CHECK(std::abs(int(ch(ref.at(25, 1), 0)) - 185) <= 2); // t = 1.275 -> 0.725
        UI_CHECK(std::abs(int(ch(ref.at(45, 1), 0)) - 70) <= 2);  // t = 2.275 -> 0.275
        // radial: the centre colour, the edge colour, padded beyond the radius
        DrawList rd;
        rd.gradientRect({0, 0, 80, 80}, Gradient::radial({40, 40}, 30.f, bw));
        img = render(rd, 80, 80);
        UI_CHECK(ch(img.at(40, 40), 0) <= 6);
        UI_CHECK(std::abs(int(ch(img.at(55, 40), 0)) - 132) <= 3); // 15.5 px from the centre
        UI_CHECK(ch(img.at(78, 40), 0) == 255 && ch(img.at(2, 2), 0) == 255);
        UI_CHECK_EQ(img.at(55, 40), img.at(40, 55)); // radially symmetric
        // conic: t grows clockwise on screen from +x
        const GradientStop four[5]{{0.f, Color::rgb(0xFF0000)}, {0.25f, Color::rgb(0x00FF00)}, {0.5f, Color::rgb(0x0000FF)},
                                   {0.75f, Color::rgb(0xFFFF00)}, {1.f, Color::rgb(0xFF0000)}};
        DrawList cd;
        cd.gradientRect({0, 0, 80, 80}, Gradient::conic({40, 40}, 0.f, four));
        img = render(cd, 80, 80);
        UI_CHECK(ch(img.at(75, 40), 0) > 240 && ch(img.at(75, 40), 1) < 16);  // +x: red
        UI_CHECK(ch(img.at(40, 75), 1) > 240 && ch(img.at(40, 75), 0) < 16);  // +y (down): green
        UI_CHECK(ch(img.at(5, 40), 2) > 240 && ch(img.at(5, 40), 1) < 16);    // -x: blue
        UI_CHECK(ch(img.at(40, 5), 0) > 240 && ch(img.at(40, 5), 1) > 240);   // -y (up): yellow
        // premultiplied interpolation: opaque red -> transparent gives half red at half alpha, not a dark fringe
        const GradientStop fade[2]{{0.f, Color::rgb(0xFF0000)}, {1.f, Color{0.f, 0.f, 0.f, 0.f}}};
        DrawList fd;
        fd.gradientRect({0, 0, 100, 4}, Gradient::linear({0, 0}, {100, 0}, fade));
        img = render(fd, 100, 4);
        UI_CHECK(std::abs(int(ch(img.at(49, 1), 0)) - 128) <= 2 && std::abs(int(ch(img.at(49, 1), 3)) - 128) <= 2);
        UI_CHECK(ch(img.at(49, 1), 1) == 0);
        // a gradient on a path, in the path's space under a transform (the gradient rotates with the shape)
        Path sq;
        sq.addRect({-20, -20, 40, 40});
        DrawList gp;
        gp.fillPath(sq, Brush(Gradient::linear({-20, 0}, {20, 0}, bw)), FillRule::NonZero, Affine::translation(40, 40) * Affine::rotation(kPi / 2.f));
        img = render(gp, 80, 80);
        UI_CHECK(ch(img.at(40, 22), 0) < 20 && ch(img.at(40, 58), 0) > 235); // black at the top, white at the bottom
        UI_CHECK(std::abs(int(ch(img.at(25, 40), 0)) - int(ch(img.at(55, 40), 0))) <= 1);
        // gradient stroke; brush opacity; a single stop is a solid colour; no stops paint nothing
        DrawList gs;
        Path hl;
        hl.moveTo({10, 10}).lineTo({90, 10});
        StrokeStyle st;
        st.width = 6.f;
        Brush b(Gradient::linear({10, 0}, {90, 0}, bw));
        b.opacity = 0.5f;
        gs.strokePath(hl, st, b);
        const GradientStop one[1]{{0.3f, Color::rgb(0x00FF00)}};
        gs.gradientRect({10, 20, 20, 10}, Gradient::linear({0, 0}, {1, 0}, one));
        gs.gradientRect({40, 20, 20, 10}, Gradient::linear({0, 0}, {1, 0}, {}));
        img = render(gs, 100, 40);
        UI_CHECK(std::abs(int(ch(img.at(89, 10), 3)) - 128) <= 1 && std::abs(int(ch(img.at(89, 10), 0)) - 127) <= 3);
        UI_CHECK_EQ(img.at(15, 25), 0xFF00FF00u);
        UI_CHECK_EQ(img.at(45, 25), 0u);
        // degenerate geometry (zero-length linear, zero radius): the last stop everywhere, no NaN
        DrawList dg;
        dg.gradientRect({0, 0, 10, 10}, Gradient::linear({5, 5}, {5, 5}, bw));
        dg.gradientRect({10, 0, 10, 10}, Gradient::radial({15, 5}, 0.f, bw));
        img = render(dg, 20, 10);
        UI_CHECK_EQ(img.at(2, 2), 0xFFFFFFFFu);
        UI_CHECK_EQ(img.at(15, 5), 0xFFFFFFFFu);
    }
    // ---- clip to path, per-corner rounded clip, layers ----
    {
        Path circle;
        circle.addCircle({40, 40}, 25.f);
        DrawList dl;
        dl.pushClipPath(circle);
        dl.rect({0, 0, 80, 80}, Color::rgb(0xFF0000));
        dl.rect({0, 40, 80, 40}, Color::rgb(0x0000FF));
        dl.popClip();
        dl.rect({70, 70, 5, 5}, Color::rgb(0x00FF00)); // after the group: unclipped
        const Image img = render(dl, 80, 80);
        UI_CHECK_EQ(img.at(40, 30), 0xFF0000FFu);
        UI_CHECK_EQ(img.at(40, 50), 0xFFFF0000u);
        UI_CHECK_EQ(img.at(5, 5), 0u);
        UI_CHECK_EQ(img.at(60, 60), 0u); // inside the bounds, outside the circle
        UI_CHECK_EQ(img.at(72, 72), 0xFF00FF00u);
        double a = 0.0;
        for (int y = 0; y < 68; ++y)
            for (int x = 0; x < 68; ++x) a += double(img.at(x, y) >> 24) / 255.0;
        CHECK_NEAR(a, kPi * 625.0, 0.003);
        // nested: a path clip inside a rect clip inside a path clip
        Path big;
        big.addRect({10, 10, 60, 60});
        DrawList n;
        n.pushClipPath(big);
        n.pushClip({0, 0, 40, 80});
        n.pushClipPath(circle);
        n.rect({0, 0, 80, 80}, kWhite);
        n.popClip();
        n.popClip();
        n.popClip();
        const Image ni = render(n, 80, 80);
        UI_CHECK_EQ(ni.at(30, 40), 0xFFFFFFFFu);
        UI_CHECK_EQ(ni.at(45, 40), 0u); // cut by the rect clip
        CHECK_NEAR(area(ni), kPi * 625.0 / 2.0, 0.004);
        // per-corner radii: top-left rounded by 20, the other corners square
        DrawList pc;
        pc.pushClipRounded({0, 0, 40, 40}, {20.f, 0.f, 0.f, 0.f});
        pc.rect({0, 0, 40, 40}, kWhite);
        pc.popClip();
        const Image pi = render(pc, 40, 40);
        UI_CHECK_EQ(pi.at(0, 0), 0u);
        UI_CHECK_EQ(pi.at(39, 0), 0xFFFFFFFFu);
        UI_CHECK_EQ(pi.at(0, 39), 0xFFFFFFFFu);
        UI_CHECK_EQ(pi.at(39, 39), 0xFFFFFFFFu);
        CHECK_NEAR(area(pi), 1600.0 - (400.0 - kPi * 100.0), 0.002);
        UI_CHECK(pc.cmds()[0].op == DrawOp::PushClipPath);
        DrawList eq; // equal radii stay the classic clip op
        eq.pushClipRounded({0, 0, 40, 40}, {6.f, 6.f, 6.f, 6.f});
        UI_CHECK(eq.cmds()[0].op == DrawOp::PushClip && eq.cmds()[0].radius[0] == 6.f && !eq.hasVectorOps());
        // opacity layer: overlapping children are composited once (blue hides red inside the layer)
        DrawList ly;
        ly.rect({0, 0, 40, 20}, kWhite);
        ly.pushLayer(0.5f, {0, 0, 40, 20});
        ly.rect({0, 0, 30, 20}, Color::rgb(0xFF0000));
        ly.rect({10, 0, 30, 20}, Color::rgb(0x0000FF));
        ly.popClip();
        const Image li = render(ly, 40, 20);
        UI_CHECK(std::abs(int(ch(li.at(5, 10), 0)) - 255) <= 1 && std::abs(int(ch(li.at(5, 10), 1)) - 128) <= 1);  // red over white
        UI_CHECK(std::abs(int(ch(li.at(20, 10), 2)) - 255) <= 1 && std::abs(int(ch(li.at(20, 10), 1)) - 128) <= 1); // blue only
        DrawList flat; // without the layer, the half-transparent blue lets the red under it show: (0.5, 0.25, 0.75)
        flat.rect({0, 0, 40, 20}, kWhite);
        flat.rect({0, 0, 30, 20}, Color::rgb(0xFF0000, 0.5f));
        flat.rect({10, 0, 30, 20}, Color::rgb(0x0000FF, 0.5f));
        UI_CHECK(std::abs(int(ch(render(flat, 40, 20).at(20, 10), 1)) - 64) <= 1);
        // layer with empty bounds = the current clip; an unbalanced group still ends at the end of the list
        DrawList ub;
        ub.pushClip({0, 0, 10, 10});
        ub.pushLayer(1.f);
        ub.rect({0, 0, 40, 20}, kWhite);
        const Image ui = render(ub, 40, 20);
        UI_CHECK_EQ(ui.at(5, 5), 0xFFFFFFFFu);
        UI_CHECK_EQ(ui.at(15, 5), 0u);
        // a path under a rounded rect clip takes the clip's antialiased coverage
        DrawList rc;
        rc.pushClip({0, 0, 40, 40}, 20.f);
        Path full;
        full.addRect({0, 0, 40, 40});
        rc.fillPath(full, kWhite);
        rc.popClip();
        CHECK_NEAR(area(render(rc, 40, 40)), kPi * 400.0, 0.004);
    }
    // ---- 9-slice / sub-rect images ----
    {
        Image src; // 12 x 12: 4 px corners (red, green, blue, yellow), grey edges, white centre
        src.resize(12, 12, 0xFFFFFFFFu);
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 12; ++x) {
                const bool l = x < 4, r = x >= 8, t = y < 4, b = y >= 8;
                u32 c = 0xFFFFFFFFu;
                if (t && l) c = 0xFF0000FFu;
                else if (t && r) c = 0xFF00FF00u;
                else if (b && r) c = 0xFFFF0000u;
                else if (b && l) c = 0xFF00FFFFu;
                else if (l || r || t || b) c = 0xFF808080u;
                src.pixels[std::size_t(y) * 12 + std::size_t(x)] = c;
            }
        src.pixels[0] = 0xFF112233u; // a marker texel: corners are copied 1:1
        DrawList dl;
        dl.ninePatch(0, {10, 5, 40, 30}, Edges{4, 4, 4, 4});
        Image img = render(dl, 60, 40, 0, {}, &src);
        UI_CHECK_EQ(img.at(10, 5), 0xFF112233u);
        UI_CHECK_EQ(img.at(11, 5), 0xFF0000FFu);
        UI_CHECK_EQ(img.at(13, 8), 0xFF0000FFu);
        UI_CHECK_EQ(img.at(14, 8), 0xFF808080u); // the top edge starts right after the 4 px corner
        UI_CHECK_EQ(img.at(46, 5), 0xFF00FF00u);
        UI_CHECK_EQ(img.at(49, 34), 0xFFFF0000u);
        UI_CHECK_EQ(img.at(10, 34), 0xFF00FFFFu);
        UI_CHECK_EQ(img.at(30, 6), 0xFF808080u);
        UI_CHECK_EQ(img.at(11, 20), 0xFF808080u);
        UI_CHECK_EQ(img.at(30, 20), 0xFFFFFFFFu); // stretched centre
        UI_CHECK_EQ(img.at(14, 9), 0xFFFFFFFFu);
        UI_CHECK_EQ(img.at(45, 30), 0xFFFFFFFFu);
        UI_CHECK_EQ(img.at(9, 5), 0u);
        UI_CHECK_EQ(img.at(50, 20), 0u);
        // destination insets differ from the source insets (a 2x border)
        DrawList d2;
        const Edges di{8, 8, 8, 8};
        d2.ninePatch(0, {0, 0, 48, 48}, Edges{4, 4, 4, 4}, {}, &di);
        img = render(d2, 48, 48, 0, {}, &src);
        UI_CHECK_EQ(img.at(7, 7), 0xFF0000FFu);
        UI_CHECK_EQ(img.at(8, 7), 0xFF808080u);
        UI_CHECK_EQ(img.at(8, 8), 0xFFFFFFFFu);
        // sub-rect: the bottom-right corner cell scaled 4x, with opacity
        DrawList d3;
        d3.imageRect(0, {0, 0, 16, 16}, {8, 8, 4, 4});
        d3.imageRect(0, {16, 0, 16, 16}, {0, 0, 4, 4}, 0.5f);
        img = render(d3, 32, 16, 0, {}, &src);
        UI_CHECK_EQ(img.at(0, 0), 0xFFFF0000u);
        UI_CHECK_EQ(img.at(15, 15), 0xFFFF0000u);
        UI_CHECK(std::abs(int(ch(img.at(30, 8), 3)) - 128) <= 1 && std::abs(int(ch(img.at(30, 8), 0)) - 128) <= 1);
        // smooth sampling blends inside a cell but never across a slice boundary
        DrawList d4;
        d4.ninePatch(0, {0, 0, 48, 48}, Edges{4, 4, 4, 4}, {}, &di, 1.f, true);
        img = render(d4, 48, 48, 0, {}, &src);
        UI_CHECK_EQ(img.at(7, 7), 0xFF0000FFu); // still pure red at the corner's inner edge
        UI_CHECK_EQ(img.at(8, 8), 0xFFFFFFFFu);
        UI_CHECK(img.at(1, 1) != 0xFF0000FFu && img.at(1, 1) != 0xFF112233u); // the marker texel blends into its neighbours
        UI_CHECK(ch(img.at(1, 1), 3) == 255 && ch(img.at(1, 1), 0) > ch(img.at(1, 1), 2));
        // a missing image, an out-of-range record: nothing drawn, no crash
        DrawList d5;
        d5.ninePatch(7, {0, 0, 10, 10}, Edges{1, 1, 1, 1});
        UI_CHECK(area(render(d5, 16, 16, 0, {}, &src)) == 0.0);
    }
    // ---- translate / scale carry the vector ops; scissored == full ----
    {
        Image src;
        src.resize(6, 6, 0xFF4080C0u);
        for (int i = 0; i < 6; ++i) src.pixels[std::size_t(i) * 7] = 0xFF00FFFFu;
        const GradientStop stops[3]{{0.f, Color::rgb(0xFF4400)}, {0.6f, Color::rgb(0x22CC88)}, {1.f, Color::rgb(0x2244FF, 0.4f)}};
        const f32 dash[2]{7.f, 3.f};
        // one scene, recorded at scale k: every coordinate, width, dash and inset multiplied by k
        auto scene = [&](DrawList& dl, f32 k) {
            const Affine s = Affine::scaling(k, k);
            Path blob;
            blob.moveTo({10, 12}).cubicTo({30, 2}, {52, 20}, {40, 44}).quadTo({22, 58}, {8, 40}).close();
            dl.fillPath(blob, Brush(Gradient::linear({8, 10}, {50, 50}, stops)), FillRule::NonZero, s);
            StrokeStyle st;
            st.width = 2.5f * k;
            st.join = LineJoin::Round;
            st.cap = LineCap::Round;
            st.setDashes(std::array<f32, 2>{dash[0] * k, dash[1] * k}, 2.f * k);
            Path scaled;
            scaled.addPath(blob, &s);
            dl.strokePath(scaled, st, Color::rgb(0xFFFFFF, 0.9f));
            Path st5 = star({80, 30}, 24.f);
            dl.fillPath(st5, Brush(Gradient::radial({80, 30}, 24.f, stops)), FillRule::EvenOdd, s);
            dl.gradientRect({60 * k, 60 * k, 44 * k, 30 * k}, Gradient::conic({82 * k, 75 * k}, 0.5f, stops), {8 * k, 0, 12 * k, 3 * k});
            Path clipC;
            clipC.addCircle({30, 80}, 18.f);
            dl.pushClipPath(clipC, FillRule::NonZero, s);
            dl.rect({10 * k, 60 * k, 40 * k, 40 * k}, Color::rgb(0x3366CC));
            dl.line({10 * k, 62 * k}, {50 * k, 98 * k}, Color::rgb(0xFFEE00), 3 * k);
            dl.popClip();
            dl.pushLayer(0.6f, {56 * k, 2 * k, 50 * k, 20 * k});
            dl.rect({58 * k, 4 * k, 20 * k, 14 * k}, Color::rgb(0xFF0000));
            dl.rect({70 * k, 6 * k, 30 * k, 12 * k}, Color::rgb(0x00FF00), 4 * k);
            dl.popClip();
            const Edges di{3 * k, 3 * k, 3 * k, 3 * k};
            dl.ninePatch(0, {4 * k, 100 * k, 40 * k, 16 * k}, Edges{2, 2, 2, 2}, {}, &di);
            dl.pushClipRounded({52 * k, 96 * k, 50 * k, 20 * k}, {10 * k, 2 * k, 10 * k, 2 * k});
            dl.rect({52 * k, 96 * k, 50 * k, 20 * k}, Color::rgb(0xCC44AA));
            dl.popClip();
        };
        DrawList one, two;
        scene(one, 1.f);
        scene(two, 2.f);
        const Image ref2 = render(two, 224, 240, 0xFF101418u, {}, &src);
        DrawList scaled = one;
        scaled.scale(2.f);
        const Image got2 = render(scaled, 224, 240, 0xFF101418u, {}, &src);
        std::printf("scale(2) vs recorded at 2x: max channel diff %d\n", maxDiff(ref2, got2));
        UI_CHECK_EQ(maxDiff(ref2, got2), 0);
        UI_CHECK(scaled.hash() != one.hash());
        // and it is not an upscale of the 1x frame: curves are re-flattened and edges stay one pixel soft
        const Image ref1 = render(one, 112, 120, 0xFF101418u, {}, &src);
        int nearestDiff = 0;
        for (int y = 0; y < 240; ++y)
            for (int x = 0; x < 224; ++x) nearestDiff += ref2.at(x, y) != ref1.at(x / 2, y / 2);
        UI_CHECK(nearestDiff > 500);
        // fractional scale: finite, and the geometry lands where it should
        DrawList frac = one;
        frac.scale(1.5f);
        const Image f15 = render(frac, 168, 180, 0xFF101418u, {}, &src);
        UI_CHECK(f15.at(int(30 * 1.5f), int(80 * 1.5f)) == 0xFFCC6633u || ch(f15.at(int(30 * 1.5f), int(80 * 1.5f)), 3) == 255);
        UI_CHECK(f15.at(int(80 * 1.5f), int(30 * 1.5f)) == 0xFF101418u); // the star's even-odd centre stays empty
        // translate by whole pixels: the image moves with it (coverage bit for bit; a gradient's colour may round
        // differently by one step, its matrix being re-based)
        DrawList moved = one;
        moved.translate({16.f, 9.f});
        const Image mv = render(moved, 140, 140, 0xFF101418u, {}, &src);
        const Image base = render(one, 140, 140, 0xFF101418u, {}, &src);
        int worst = 0, alphaBad = 0;
        for (int y = 0; y < 120; ++y)
            for (int x = 0; x < 112; ++x) {
                const u32 a = mv.at(x + 16, y + 9), b = base.at(x, y);
                for (int c = 0; c < 4; ++c) worst = std::max(worst, std::abs(int(ch(a, c)) - int(ch(b, c))));
            }
        UI_CHECK(worst <= 1);
        DrawList solidA, solidB; // without gradients: exactly the same pixels
        Path blob;
        blob.moveTo({10, 12}).cubicTo({30, 2}, {52, 20}, {40, 44}).quadTo({22, 58}, {8, 40}).close();
        StrokeStyle bst;
        bst.width = 2.5f;
        bst.join = LineJoin::Round;
        solidA.fillPath(blob, Color::rgb(0x40C0FF, 0.8f));
        solidA.strokePath(blob, bst, kWhite);
        solidB = solidA;
        solidB.translate({16.f, 9.f});
        const Image sa = render(solidA, 90, 80), sb = render(solidB, 90, 80);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) alphaBad += sa.at(x, y) != sb.at(x + 16, y + 9);
        UI_CHECK_EQ(alphaBad, 0);
        // a scissored render equals the full render inside the scissor and leaves the rest alone
        const Rect sc{37.f, 21.f, 51.f, 73.f};
        const Image part = render(one, 112, 120, 0xFF101418u, sc, &src);
        int inside = 0, outside = 0;
        for (int y = 0; y < 120; ++y)
            for (int x = 0; x < 112; ++x) {
                const bool in = x >= 37 && x < 88 && y >= 21 && y < 94;
                if (in) inside += part.at(x, y) != ref1.at(x, y);
                else outside += part.at(x, y) != 0xFF101418u;
            }
        UI_CHECK_EQ(inside, 0);
        UI_CHECK_EQ(outside, 0);
        // rasterizeCommands (the GPU packer's entry): the layer of one path command, composited = the direct render
        DrawList single;
        Path c;
        c.addCircle({20.5f, 20.25f}, 15.f);
        single.fillPath(c, Color::rgb(0x40C0FF, 0.7f));
        Image layer;
        rasterizeCommands(single, 0, 1, single.cmds()[0].rect.intersect({0, 0, 48, 48}), 48, {}, layer);
        const Image direct = render(single, 48, 48);
        int mismatch = 0;
        const int lx = int(std::floor(single.cmds()[0].rect.x)), lyy = int(std::floor(single.cmds()[0].rect.y));
        for (int y = 0; y < layer.height; ++y)
            for (int x = 0; x < layer.width; ++x) mismatch += layer.at(x, y) != direct.at(x + lx, y + lyy);
        UI_CHECK_EQ(mismatch, 0);
    }
    // ---- coverage cache ----
    {
        clearPathCache();
        Path icon;
        icon.addCircle({12, 12}, 9.f);
        icon.moveTo({8, 12}).lineTo({11, 15}).lineTo({16, 9});
        StrokeStyle st;
        st.width = 2.f;
        st.cap = LineCap::Round;
        st.join = LineJoin::Round;
        DrawList a, b, c;
        a.strokePath(icon, st, kWhite);
        b.strokePath(icon, st, kWhite, Affine::translation(40.f, 17.f));
        c.strokePath(icon, st, kWhite, Affine::translation(40.25f, 17.f));
        const Image ia = render(a, 96, 64);
        const PathCacheStats s0 = pathCacheStats();
        UI_CHECK_EQ(s0.misses, u64(1));
        UI_CHECK_EQ(s0.entries, 1u);
        const Image ib = render(b, 96, 64);
        const PathCacheStats s1 = pathCacheStats();
        UI_CHECK_EQ(s1.hits, u64(1)); // same shape at another whole-pixel position
        UI_CHECK_EQ(s1.misses, u64(1));
        int bad = 0;
        for (int y = 0; y < 30; ++y)
            for (int x = 0; x < 30; ++x) bad += ia.at(x, y) != ib.at(x + 40, y + 17);
        UI_CHECK_EQ(bad, 0);
        (void)render(c, 96, 64); // a different sub-pixel phase is another mask
        UI_CHECK_EQ(pathCacheStats().misses, u64(2));
        (void)render(a, 96, 64);
        UI_CHECK_EQ(pathCacheStats().hits, u64(2));
        // cached and uncached agree: the same draw with a cold cache
        clearPathCache();
        const Image cold = render(b, 96, 64);
        UI_CHECK_EQ(maxDiff(cold, ib), 0);
        // big paths are not cached
        Path big;
        big.addCircle({200, 200}, 190.f);
        DrawList d;
        d.fillPath(big, kWhite);
        (void)render(d, 400, 400);
        UI_CHECK_EQ(pathCacheStats().uncached, u64(1));
        // a path partly outside the target is clipped, not cached, and still right
        Path off;
        off.addCircle({0, 0}, 20.f);
        DrawList od;
        od.fillPath(off, kWhite);
        CHECK_NEAR(area(render(od, 40, 40)), kPi * 100.0, 0.005);
        // ... on every side: a rect larger than the target covers all of it, one sticking out to the right / left
        // covers up to the edge (the fill continues past the last edge inside the window)
        Path over;
        over.addRect({-50, -30, 300, 200});
        DrawList ov;
        ov.fillPath(over, kWhite);
        UI_CHECK(area(render(ov, 64, 48)) == 64.0 * 48.0);
        Path right, left;
        right.addRect({10.5f, 8, 500, 20});
        left.addRect({-500, 8, 520.25f, 20});
        DrawList rl, ll;
        rl.fillPath(right, kWhite);
        ll.fillPath(left, kWhite);
        CHECK_NEAR(area(render(rl, 64, 48)), 53.5 * 20.0, 0.0005);
        CHECK_NEAR(area(render(ll, 64, 48)), 20.25 * 20.0, 0.0005);
        // a thin diagonal across a large target: rendered by spans (no bounding-box mask), correct area
        Path diag;
        diag.moveTo({10, 10}).lineTo({1890, 1070});
        StrokeStyle thin;
        thin.width = 2.f;
        DrawList dd;
        dd.strokePath(diag, thin, kWhite);
        const Image di = render(dd, 1920, 1080);
        CHECK_NEAR(area(di), std::hypot(1880.0, 1060.0) * 2.0, 0.001);
        UI_CHECK(ch(di.at(950, 540), 3) > 0 && di.at(950, 500) == 0u);
    }
    // ---- robustness ----
    {
        DrawList dl;
        Path nan;
        nan.moveTo({0, 0}).lineTo({std::nanf(""), 5}).lineTo({10, 10}).close();
        dl.fillPath(nan, kWhite);
        Path huge;
        huge.moveTo({-1e9f, -1e9f}).lineTo({1e9f, 0}).lineTo({0, 1e9f}).close();
        dl.fillPath(huge, kWhite);
        Path inf;
        inf.moveTo({0, 0}).lineTo({INFINITY, 0}).lineTo({5, 5});
        StrokeStyle st;
        st.width = 3.f;
        dl.strokePath(inf, st, kWhite);
        dl.fillPath(Path{}, kWhite);            // empty: not recorded
        dl.strokePath(nan, StrokeStyle{.width = 0.f}, kWhite); // zero width: not recorded
        dl.fillPath(nan, Color{});              // transparent: not recorded
        UI_CHECK_EQ(dl.size(), std::size_t(3));
        Path single;
        single.moveTo({5, 5});
        dl.fillPath(single, kWhite);
        Path degenerate;
        degenerate.moveTo({5, 5}).lineTo({15, 5}).close(); // zero area
        dl.fillPath(degenerate, kWhite);
        dl.popClip(); // stray pop
        dl.fillPath(nan, kWhite, FillRule::NonZero, Affine::scaling(0.f, 0.f));
        const Image img = render(dl, 32, 32);
        UI_CHECK(area(img) == 0.0);
        // a visible path with sane geometry next to them still draws
        Path ok;
        ok.addRect({2, 2, 4, 4});
        dl.fillPath(ok, kWhite);
        UI_CHECK(area(render(dl, 32, 32)) == 16.0);
        // clear() resets the side records and keeps working
        dl.clear();
        UI_CHECK(!dl.hasVectorOps() && dl.paths().empty() && dl.pathPoints().empty());
        dl.fillPath(ok, kWhite);
        UI_CHECK(dl.hasVectorOps() && dl.paths().size() == 1);
        // hashes: classic lists hash as before the vector ops existed; vector data is part of the hash
        DrawList h1, h2, h3;
        h1.rect({1, 2, 3, 4}, kWhite);
        h2.rect({1, 2, 3, 4}, kWhite);
        UI_CHECK_EQ(h1.hash(), h2.hash());
        h2.fillPath(ok, kWhite);
        h3.rect({1, 2, 3, 4}, kWhite);
        Path ok2;
        ok2.addRect({2, 2, 4, 5});
        h3.fillPath(ok2, kWhite);
        UI_CHECK(h1.hash() != h2.hash());
        UI_CHECK(h2.hash() != h3.hash());
    }
    return finish("fuse_ui_paths");
}
