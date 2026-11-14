// Gate fuse_ui_css_masking: clip-path, mask-image and filter on the style engine, painted by the CPU reference
// rasteriser through the draw list's effect groups.
//   * parse: clip-path inset / circle / ellipse / polygon (lengths, percentages, `at` positions, round, evenodd),
//     mask-image linear-gradient / radial-gradient (angles, sides, stop positions), mask-mode, the mask shorthand,
//     filter lists (every function, defaults, drop-shadow), `none`; bad values
//   * clip-path: pixels inside / outside each shape, children are cut too, hit testing follows the shape
//   * mask: the gradient's alpha (or luminance) multiplies the subtree; fully masked points are not hit
//   * filter: colour functions against the CSS formulas, blur against a true Gaussian of the same deviation (mass,
//     spread, symmetry), drop-shadow offset / tint / under the source, lists in order, opacity
//   * filter transitions interpolate matching lists and from / to `none`
//   * damage covers what a blur spreads; nothing is drawn or allocated for the effects when the node is hidden
// (Adapted from the gui-framework gate of the same name: clip-path path() / url() / reference boxes, image masks,
// mask-size / position / repeat / composite, container / viewport units and @container style() are not ported.)
#include "test_util.hpp"

#include <cmath>
#include <cstdio>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }
NodeHandle make(UiContext& ui, NodeHandle p, std::string_view cls) {
    const NodeHandle n = ui.create(ui.registry().find("panel"), p);
    if (!cls.empty()) ui.addClass(n, cls);
    return n;
}
struct Shot {
    Image img;
    Shot(UiContext& ui, int w, int h, u32 clear = 0xFF000000u) {
        img.resize(w, h, clear);
        CpuRasterizer().render(ui.drawList(), img);
    }
    int ch(int x, int y, int c) const { return int((img.at(x, y) >> (8 * c)) & 0xFF); }
    int r(f32 x, f32 y) const { return ch(int(x), int(y), 0); }
    int g(f32 x, f32 y) const { return ch(int(x), int(y), 1); }
    int b(f32 x, f32 y) const { return ch(int(x), int(y), 2); }
};
bool about(int got, f32 want, int tol = 2) { return std::abs(got - int(std::lround(want))) <= tol; }
bool hasOp(const DrawList& dl, DrawOp op) {
    for (const DrawCmd& c : dl.cmds())
        if (c.op == op) return true;
    return false;
}
} // namespace

int main() {
    // ---- parsing ----
    {
        PropValue v;
        for (const char* ok : {"none", "inset(10px)", "inset(5% 10px round 4px)", "inset(1px 2px 3px 4px)", "circle()", "circle(50px at left top)",
                               "circle(closest-side at 20% 30%)", "circle(40%)", "ellipse(40px 20px)", "ellipse(30% 10% at center)",
                               "ellipse()", "polygon(0 0, 100% 0, 50% 100%)", "polygon(evenodd, 0 0, 10px 0, 10px 10px, 0 10px)"})
            UI_CHECK(parsePropValue(Prop::ClipPath, ok, v));
        for (const char* bad : {"", "inset()", "circle(1px 2px)", "polygon(0 0, 1px 1px)", "polygon(0 0 0, 1 1, 2 2)", "ellipse(1px)", "inset(1px) circle()",
                                "url(#x)", "path('M 0 0 L 10 0 L 0 10 Z')", "circle(-4px)", "inset(1px round 10%)", "square(4px)",
                                "polygon(0 0, 1px 0, 2px 0, 3px 0, 4px 0, 5px 0, 6px 0, 7px 0, 8px 0)"})
            UI_CHECK(!parsePropValue(Prop::ClipPath, bad, v));
        ComputedStyle s;
        UI_CHECK(parsePropValue(Prop::ClipPath, "circle(25% at 10px 75%)", v));
        applyProp(s, Prop::ClipPath, v);
        UI_CHECK(s.clipPath.kind == ClipShapeKind::Circle && near(s.clipPath.v[0], 25.f) && (s.clipPath.pct & 1u) && near(s.clipPath.v[1], 10.f) &&
                 !(s.clipPath.pct & 2u) && near(s.clipPath.v[2], 75.f) && (s.clipPath.pct & 4u));
        UI_CHECK(clipShapeContains(s.clipPath, {0, 0, 100, 100}, {10, 75}) && !clipShapeContains(s.clipPath, {0, 0, 100, 100}, {40, 75}));

        for (const char* ok : {"none", "linear-gradient(black, transparent)", "linear-gradient(to right, #000, #0000 50%, #000)",
                               "linear-gradient(45deg, red 10%, blue 90%)", "linear-gradient(to top left, white, black)",
                               "radial-gradient(black, transparent)", "radial-gradient(circle, rgba(0, 0, 0, 1) 40%, rgba(0, 0, 0, 0) 60%)",
                               "linear-gradient(0.25turn, red, green, blue, white)"})
            UI_CHECK(parsePropValue(Prop::MaskImage, ok, v));
        for (const char* bad : {"", "linear-gradient(black)", "linear-gradient(to nowhere, black, white)", "conic-gradient(black, white)",
                                "url(mask.png)", "linear-gradient(red, green, blue, white, black)", "linear-gradient(red 10, blue)", "black"})
            UI_CHECK(!parsePropValue(Prop::MaskImage, bad, v));
        UI_CHECK(parsePropValue(Prop::MaskImage, "linear-gradient(to right, red, green, blue)", v));
        applyProp(s, Prop::MaskImage, v);
        UI_CHECK(s.mask.kind == GradientSpecKind::Linear && s.mask.stopCount == 3 && near(s.mask.angle, 1.5707963f) && near(s.mask.offset[1], 0.5f) &&
                 near(s.mask.offset[2], 1.f) && near(s.mask.color[1].g, 128.f / 255.f, 0.01f));
        auto sh = StyleSheet::parse(".a { mask: radial-gradient(black, transparent) luminance; } .b { mask-mode: alpha; mask-image: none; }");
        UI_CHECK(sh && sh->rules()[0].decls.size() == 2 && sh->rules()[1].decls.size() == 2);

        for (const char* ok : {"none", "blur(4px)", "blur()", "brightness(1.5)", "brightness(150%)", "contrast(0)", "grayscale()", "sepia(0.5)",
                               "saturate(3)", "hue-rotate(90deg)", "hue-rotate(0.5turn)", "invert(100%)", "opacity(25%)",
                               "drop-shadow(2px 3px 4px red)", "drop-shadow(#000 1px 1px)", "drop-shadow(1px 1px)",
                               "blur(2px) brightness(0.5) contrast(2) drop-shadow(0 4px 6px rgba(0, 0, 0, 0.5))"})
            UI_CHECK(parsePropValue(Prop::Filter, ok, v));
        for (const char* bad : {"", "blur(-1px)", "blur(10%)", "brightness(-1)", "shine(2)", "hue-rotate(90)", "drop-shadow(1px)", "blur(1px",
                                "blur(1px) blur(1px) blur(1px) blur(1px) blur(1px)", "drop-shadow(1px 1px) drop-shadow(2px 2px)", "url(#f)"})
            UI_CHECK(!parsePropValue(Prop::Filter, bad, v));
        UI_CHECK(parsePropValue(Prop::Filter, "blur(2px) drop-shadow(1px 2px 3px #ff0000) opacity(50%)", v));
        applyProp(s, Prop::Filter, v);
        UI_CHECK(s.filter.count == 3 && s.filter.fn[0].kind == FilterKind::Blur && near(s.filter.fn[0].amount, 2.f));
        UI_CHECK(s.filter.fn[1].kind == FilterKind::DropShadow && near(s.filter.fn[1].amount, 3.f) && near(s.filter.shadowOffset.y, 2.f) &&
                 near(s.filter.shadowColor.r, 1.f) && s.filter.fn[2].kind == FilterKind::Opacity && near(s.filter.fn[2].amount, 0.5f));
        // interpolation: matching lists blend, `none` is each function's identity
        ComputedStyle a, b, out;
        UI_CHECK(parsePropValue(Prop::Filter, "blur(8px) brightness(2)", v));
        applyProp(b, Prop::Filter, v);
        lerpStyleProp(out, a, b, Prop::Filter, 0.25f);
        UI_CHECK(out.filter.count == 2 && near(out.filter.fn[0].amount, 2.f) && near(out.filter.fn[1].amount, 1.25f));
        UI_CHECK(parsePropValue(Prop::Filter, "blur(4px) brightness(1)", v));
        applyProp(a, Prop::Filter, v);
        lerpStyleProp(out, a, b, Prop::Filter, 0.5f);
        UI_CHECK(near(out.filter.fn[0].amount, 6.f) && near(out.filter.fn[1].amount, 1.5f));
    }

    UiContext ui({.viewport = {300, 200}});
    UI_CHECK(ui.addStyleSheet(R"css(
panel { flex-shrink: 0; }
.stage { layout: absolute; width: 300px; height: 200px; }
.box { width: 100px; height: 100px; background: #ff0000; left: 20px; top: 20px; }
.kid { width: 100px; height: 100px; background: #0000ff; }
.circle { clip-path: circle(50px); }
.tri { clip-path: polygon(0 0, 100% 0, 0 100%); }
.inset { clip-path: inset(10px 20px 30px 40px); }
.oval { clip-path: ellipse(50% 25%); }
.fade { mask-image: linear-gradient(to right, black, transparent); }
.spot { mask-image: radial-gradient(black 30%, transparent 30%); }
.lum { mask: linear-gradient(to bottom, white, black) luminance; }
.gray { filter: grayscale(1); }
.bright { filter: brightness(0.5); }
.inv { filter: invert(1); }
.hue { filter: hue-rotate(120deg); }
.sat { filter: saturate(0); }
.half { filter: opacity(0.5); }
.chain { filter: invert(1) brightness(0.5); }
.blur { filter: blur(6px); }
.shadow { filter: drop-shadow(30px 20px 0px #00ff00); }
.soft { filter: drop-shadow(0px 0px 5px #ffffff); }
.tr { transition: filter 1s linear; }
.gone { display: none; }
)css").has_value());
    const NodeHandle stage = make(ui, ui.root(), "stage");
    const NodeHandle box = make(ui, stage, "box");
    ui.update(0.f);
    auto with = [&](const char* cls) {
        ui.addClass(box, cls);
        ui.update(0.f);
    };
    auto without = [&](const char* cls) {
        ui.removeClass(box, cls);
        ui.update(0.f);
    };

    // ---- clip-path ----
    with("circle");
    {
        const Shot s(ui, 300, 200);
        UI_CHECK(hasOp(ui.drawList(), DrawOp::PushClipPath));
        UI_CHECK(s.r(70, 70) == 255 && s.r(23, 23) == 0 && s.r(117, 117) == 0 && s.r(70, 22) == 255 && s.r(118, 70) == 255);
        UI_CHECK(ui.hitTest({70, 70}) == box && ui.hitTest({23, 23}) == stage && ui.hitTest({117, 23}) == stage);
    }
    without("circle");
    with("tri");
    {
        const NodeHandle kid = make(ui, box, "kid"); // children are cut by the parent's shape too
        ui.update(0.f);
        const Shot s(ui, 300, 200);
        UI_CHECK(s.b(40, 40) == 255 && s.b(100, 100) == 0 && s.r(100, 100) == 0 && s.b(110, 25) == 255 && s.b(25, 110) == 255);
        UI_CHECK(ui.hitTest({40, 40}) == kid && ui.hitTest({100, 100}) == stage);
        ui.destroy(kid);
    }
    without("tri");
    with("inset");
    {
        const Shot s(ui, 300, 200); // box 20 .. 120: left 40, top 10, right 20, bottom 30 -> x 60 .. 100, y 30 .. 90
        UI_CHECK(s.r(62, 32) == 255 && s.r(98, 88) == 255 && s.r(58, 60) == 0 && s.r(102, 60) == 0 && s.r(80, 28) == 0 && s.r(80, 92) == 0);
        UI_CHECK(ui.hitTest({80, 60}) == box && ui.hitTest({50, 60}) == stage);
    }
    without("inset");
    with("oval");
    {
        const Shot s(ui, 300, 200); // rx 50, ry 25 about (70, 70)
        UI_CHECK(s.r(25, 70) == 255 && s.r(115, 70) == 255 && s.r(70, 48) == 255 && s.r(70, 40) == 0 && s.r(30, 50) == 0);
    }
    without("oval");
    UI_CHECK(!hasOp(ui.drawList(), DrawOp::PushClipPath));

    // ---- mask ----
    with("fade");
    {
        const Shot s(ui, 300, 200);
        UI_CHECK(hasOp(ui.drawList(), DrawOp::PushMask));
        for (int x : {25, 45, 70, 95, 115}) UI_CHECK(about(s.r(f32(x), 70), 255.f * (1.f - (f32(x) + 0.5f - 20.f) / 100.f), 3));
        UI_CHECK(s.g(70, 70) == 0);
        UI_CHECK(ui.hitTest({30, 70}) == box && ui.hitTest({119.9f, 70}) == stage); // fully masked: not hit
    }
    without("fade");
    with("spot");
    {
        const Shot s(ui, 300, 200); // an ellipse to the corners: 30% of the half diagonal = 21.2 px from the centre
        UI_CHECK(s.r(70, 70) == 255 && s.r(85, 70) == 255 && s.r(70, 88) == 255 && s.r(95, 70) == 0 && s.r(25, 25) == 0);
        UI_CHECK(ui.hitTest({70, 70}) == box && ui.hitTest({100, 70}) == stage);
    }
    without("spot");
    with("lum");
    {
        const Shot s(ui, 300, 200); // white at the top (kept) to black at the bottom (gone)
        UI_CHECK(about(s.r(70, 22), 255.f * 0.975f, 4) && about(s.r(70, 70), 255.f * 0.495f, 4) && s.r(70, 118) < 10);
    }
    without("lum");

    // ---- filter: colour functions (red #ff0000 in, the CSS matrices out) ----
    auto filtered = [&](const char* cls, f32 r, f32 g, f32 b) {
        with(cls);
        const Shot s(ui, 300, 200);
        const bool ok = about(s.r(70, 70), r) && about(s.g(70, 70), g) && about(s.b(70, 70), b) && s.r(10, 10) == 0;
        if (!ok) std::fprintf(stderr, "  filter .%s: got %d %d %d, want %.0f %.0f %.0f\n", cls, s.r(70, 70), s.g(70, 70), s.b(70, 70), r, g, b);
        UI_CHECK(ok);
        UI_CHECK(hasOp(ui.drawList(), DrawOp::PushFilter));
        without(cls);
    };
    filtered("gray", 255 * 0.2126f, 255 * 0.2126f, 255 * 0.2126f); // luminance weights (0.213 in the matrix)
    filtered("bright", 127.5f, 0, 0);
    filtered("inv", 0, 255, 255);
    filtered("sat", 255 * 0.213f, 255 * 0.213f, 255 * 0.213f);
    filtered("hue", 0, 255 * (0.213f + 0.5f * 0.213f + 0.866f * 0.143f), 0); // red turns towards green (clamped channels)
    filtered("chain", 0, 127.5f, 127.5f); // in order: invert, then darken
    with("half");
    {
        const Shot s(ui, 300, 200, 0xFFFFFFFFu); // over white: half of the red shows
        UI_CHECK(about(s.r(70, 70), 255) && about(s.g(70, 70), 127.5f) && about(s.b(70, 70), 127.5f));
    }
    without("half");

    // ---- filter: blur against a Gaussian ----
    with("blur");
    {
        const Shot s(ui, 300, 200);
        // far inside it is still solid; along a horizontal cut through the middle the edge is the Gaussian's CDF
        UI_CHECK(s.r(70, 70) == 255);
        f32 worst = 0.f, mass = 0.f;
        for (int x = 0; x < 60; ++x) {
            const f32 d = f32(x) + 0.5f - 20.f; // distance into the box from its left edge
            const f32 want = 0.5f * (1.f + std::erf(d / (6.f * std::sqrt(2.f))));
            worst = std::max(worst, std::fabs(f32(s.r(f32(x), 70)) / 255.f - want));
            mass += f32(s.r(f32(x), 70)) / 255.f;
        }
        std::printf("blur(6px) edge vs Gaussian CDF: worst error %.3f, mass %.2f (ideal 40)\n", worst, mass);
        UI_CHECK(worst < 0.03f);
        UI_CHECK(std::fabs(mass - 40.f) < 0.5f); // blurring moves coverage, it does not create or lose it
        UI_CHECK(s.r(19, 70) > 100 && s.r(19, 70) < 130 && s.r(1, 70) == 0); // the edge itself is half; 19 px out: nothing
        for (int k = 1; k < 16; ++k) UI_CHECK(std::abs(s.r(f32(20 - k), 70) - s.r(70, f32(20 - k))) <= 1); // the same in x and y
        for (int k = 1; k < 16; ++k) UI_CHECK(std::abs(s.r(f32(20 - k), 70) + s.r(f32(19 + k), 70) - 255) <= 2); // odd symmetry about the edge
        const Rect d = ui.damage(); // the blur reaches outside the box: the damage covers it
        UI_CHECK(d.x <= 4 && d.y <= 4 && d.right() >= 136 && d.bottom() >= 136);
    }
    without("blur");
    UI_CHECK(ui.damage().x <= 4 && ui.damage().right() >= 136); // and removing it repaints the same area

    // ---- filter: drop-shadow ----
    with("shadow");
    {
        const Shot s(ui, 300, 200);
        UI_CHECK(s.r(70, 70) == 255 && s.g(70, 70) == 0);                       // the source is on top
        UI_CHECK(s.g(135, 130) == 255 && s.r(135, 130) == 0);                   // the shadow: moved by (30, 20), green
        UI_CHECK(s.g(125, 70) == 255 && s.g(152, 70) == 0 && s.g(70, 142) == 0 && s.g(70, 130) == 255 && s.g(45, 130) == 0);
        UI_CHECK(ui.hitTest({135, 130}) == stage); // a shadow is not the node
    }
    without("shadow");
    with("soft");
    {
        const Shot s(ui, 300, 200); // a white glow around the red box, fading out
        UI_CHECK(s.r(70, 70) == 255 && s.g(70, 70) == 0);
        UI_CHECK(s.g(17, 70) > 40 && s.g(17, 70) < 110 && s.g(12, 70) > 0 && s.g(12, 70) < s.g(17, 70) && s.g(2, 70) == 0);
    }
    without("soft");

    // ---- transitions ----
    ui.addClass(box, "tr");
    ui.update(0.f);
    ui.addClass(box, "bright"); // none -> brightness(0.5): through brightness(0.75)
    ui.update(0.f);
    ui.update(0.5f);
    UI_CHECK(ui.computed(box).filter.count == 1 && near(ui.computed(box).filter.fn[0].amount, 0.75f));
    {
        const Shot s(ui, 300, 200);
        UI_CHECK(about(s.r(70, 70), 255 * 0.75f));
    }
    ui.update(0.6f);
    UI_CHECK(near(ui.computed(box).filter.fn[0].amount, 0.5f) && !ui.styleAnimating(box));
    ui.removeClass(box, "tr");
    ui.removeClass(box, "bright");
    ui.update(0.f);

    // ---- hidden nodes draw nothing ----
    with("blur");
    with("gone");
    UI_CHECK(!hasOp(ui.drawList(), DrawOp::PushFilter));
    {
        const Shot s(ui, 300, 200);
        UI_CHECK(s.r(70, 70) == 0);
    }
    without("gone");
    UI_CHECK(hasOp(ui.drawList(), DrawOp::PushFilter));
    ui.update(0.f);
    UI_CHECK(!ui.needsUpdate());
    return finish("fuse_ui_css_masking");
}
