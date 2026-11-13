// Gate fuse_ui_transform_affine: the CSS `transform` property (2D) on the style engine and the draw list.
//   * parse: translate / translateX / translateY (px and %), scale / scaleX / scaleY, rotate (deg / rad / turn / grad),
//     skew / skewX / skewY, matrix(6), function lists (composed left to right), `none`; transform-origin
//   * the resolved matrix: percentages against the border box, about the transform origin
//   * interpolation through the decomposed form (shortest angle), in transitions
//   * paint: a translated box moves by whole pixels and stays sharp; rotated / scaled boxes go through a transform
//     group (DrawOp::PushTransform) and land where the matrix says; nested transforms compose; layout is unaffected
//   * hit testing through the inverse transform (also nested, also with overflow clips); toLocal / fromLocal; the
//     pointer position handlers get is in the node's layout space
//   * damage covers the transformed box, before and after a change; render scale 2 paints the same picture twice as big
// (Adapted from the gui-framework gate of the same name: 3D functions, scroll / view timelines and KeyframeEffect
// retargeting are not ported.)
#include "test_util.hpp"

#include <cmath>
#include <cstdio>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
constexpr f32 kPi = 3.14159265358979f;
bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool parse(const char* text, Transform2D& out) {
    PropValue v;
    if (!parsePropValue(Prop::Transform, text, v)) return false;
    ComputedStyle s;
    applyProp(s, Prop::Transform, v);
    out = s.transform;
    return true;
}
bool isMatrix(const Transform2D& t, f32 a, f32 b, f32 c, f32 d, f32 tx, f32 ty) {
    return near(t.m[0], a) && near(t.m[1], b) && near(t.m[2], c) && near(t.m[3], d) && near(t.m[4], tx) && near(t.m[5], ty);
}
NodeHandle make(UiContext& ui, NodeHandle p, std::string_view cls) {
    const NodeHandle n = ui.create(ui.registry().find("panel"), p);
    if (!cls.empty()) ui.addClass(n, cls);
    return n;
}
struct Shot {
    Image img;
    explicit Shot(UiContext& ui, int w, int h) {
        img.resize(w, h, 0xFF000000u);
        CpuRasterizer().render(ui.drawList(), img);
    }
    int r(f32 x, f32 y) const { return int(img.at(int(x), int(y)) & 0xFF); }
    int g(f32 x, f32 y) const { return int((img.at(int(x), int(y)) >> 8) & 0xFF); }
    int b(f32 x, f32 y) const { return int((img.at(int(x), int(y)) >> 16) & 0xFF); }
};
bool hasOp(const DrawList& dl, DrawOp op) {
    for (const DrawCmd& c : dl.cmds())
        if (c.op == op) return true;
    return false;
}
} // namespace

int main() {
    // ---- parsing ----
    {
        Transform2D t;
        UI_CHECK(parse("none", t) && t.identity());
        UI_CHECK(parse("translate(10px, 20px)", t) && isMatrix(t, 1, 0, 0, 1, 10, 20));
        UI_CHECK(parse("translateX(5px) translateY(-7px)", t) && isMatrix(t, 1, 0, 0, 1, 5, -7));
        UI_CHECK(parse("scale(2)", t) && isMatrix(t, 2, 0, 0, 2, 0, 0) && parse("scale(2, 3)", t) && isMatrix(t, 2, 0, 0, 3, 0, 0));
        UI_CHECK(parse("scaleX(50%) scaleY(4)", t) && isMatrix(t, 0.5f, 0, 0, 4, 0, 0));
        UI_CHECK(parse("rotate(90deg)", t) && isMatrix(t, 0, 1, -1, 0, 0, 0));
        UI_CHECK(parse("rotate(0.25turn)", t) && isMatrix(t, 0, 1, -1, 0, 0, 0) && parse("rotate(100grad)", t) && isMatrix(t, 0, 1, -1, 0, 0, 0));
        UI_CHECK(parse("rotate(1.5707963rad)", t) && isMatrix(t, 0, 1, -1, 0, 0, 0));
        UI_CHECK(parse("skewX(45deg)", t) && isMatrix(t, 1, 0, 1, 1, 0, 0) && parse("skewY(45deg)", t) && isMatrix(t, 1, 1, 0, 1, 0, 0));
        UI_CHECK(parse("skew(45deg, 0)", t) && isMatrix(t, 1, 0, 1, 1, 0, 0));
        UI_CHECK(parse("matrix(1, 2, 3, 4, 5, 6)", t) && isMatrix(t, 1, 2, 3, 4, 5, 6));
        // left to right: translate then scale scales about the moved origin; the other order scales the translation
        UI_CHECK(parse("translate(10px, 0) scale(2)", t) && isMatrix(t, 2, 0, 0, 2, 10, 0));
        UI_CHECK(parse("scale(2) translate(10px, 0)", t) && isMatrix(t, 2, 0, 0, 2, 20, 0));
        UI_CHECK(parse("rotate(90deg) translate(10px, 0)", t) && isMatrix(t, 0, 1, -1, 0, 0, 10));
        // percentages stay symbolic until the box is known (and go through the functions before them)
        UI_CHECK(parse("translate(-50%, 25%)", t) && isMatrix(t, 1, 0, 0, 1, 0, 0) && near(t.pct[0], -0.5f) && near(t.pct[3], 0.25f));
        UI_CHECK(parse("scale(2) translateX(50%)", t) && near(t.pct[0], 1.f) && near(t.pct[2], 0.f));
        for (const char* bad : {"", "rotate(90)", "translate(1px, 2px, 3px)", "scale()", "spin(1deg)", "rotate(10deg", "matrix(1, 2, 3)",
                                "translate(1em)", "rotate3d(1, 0, 0, 10deg)", "translateZ(4px)", "scale(a)", "10px"})
            UI_CHECK(!parse(bad, t));
        // transform-origin
        auto s = StyleSheet::parse(".a { transform-origin: left top; } .b { transform-origin: 10px 75%; } .c { transform-origin: bottom; } .d { transform-origin: top right; }");
        UI_CHECK(s.has_value() && s->rules().size() == 4);
        if (s && s->rules().size() == 4) {
            ComputedStyle a, b, c, d;
            for (const auto& x : s->rules()[0].decls) applyProp(a, x.prop, x.value);
            for (const auto& x : s->rules()[1].decls) applyProp(b, x.prop, x.value);
            for (const auto& x : s->rules()[2].decls) applyProp(c, x.prop, x.value);
            for (const auto& x : s->rules()[3].decls) applyProp(d, x.prop, x.value);
            UI_CHECK(a.originX == Length::pct(0.f) && a.originY == Length::pct(0.f));
            UI_CHECK(b.originX == Length::px(10.f) && b.originY == Length::pct(75.f));
            UI_CHECK(c.originX == Length::pct(50.f) && c.originY == Length::pct(100.f));
            UI_CHECK(d.originX == Length::pct(100.f) && d.originY == Length::pct(0.f));
        }
        // the resolved matrix: about the centre by default; percentages of the border box
        ComputedStyle cs;
        const Rect box{100, 50, 200, 100};
        UI_CHECK(parse("rotate(90deg)", cs.transform));
        Vec2 p = styleTransform(cs, box).apply({300, 100}); // the middle of the right edge goes to the bottom
        UI_CHECK(near(p.x, 200.f) && near(p.y, 200.f));
        cs.originX = cs.originY = Length::pct(0.f);
        p = styleTransform(cs, box).apply({300, 50});
        UI_CHECK(near(p.x, 100.f) && near(p.y, 250.f));
        UI_CHECK(parse("translate(50%, -100%)", cs.transform));
        p = styleTransform(cs, box).apply({100, 50});
        UI_CHECK(near(p.x, 200.f) && near(p.y, -50.f));
    }

    // ---- interpolation ----
    {
        ComputedStyle a, b, out;
        UI_CHECK(parse("rotate(0deg)", a.transform) && parse("rotate(90deg) scale(3)", b.transform));
        lerpStyleProp(out, a, b, Prop::Transform, 0.5f);
        UI_CHECK(isMatrix(out.transform, 2.f * std::cos(kPi / 4), 2.f * std::sin(kPi / 4), -2.f * std::sin(kPi / 4), 2.f * std::cos(kPi / 4), 0, 0));
        UI_CHECK(parse("rotate(170deg)", a.transform) && parse("rotate(-170deg)", b.transform)); // 20 degrees apart, not 340
        lerpStyleProp(out, a, b, Prop::Transform, 0.5f);
        UI_CHECK(near(std::abs(out.transform.m[0]), 1.f) && near(out.transform.m[1], 0.f, 1e-2f));
        UI_CHECK(parse("translate(10px, 0)", a.transform) && parse("translate(30px, 50%)", b.transform));
        lerpStyleProp(out, a, b, Prop::Transform, 0.25f);
        UI_CHECK(isMatrix(out.transform, 1, 0, 0, 1, 15, 0) && near(out.transform.pct[3], 0.125f));
        UI_CHECK(!sameStyleProp(a, b, Prop::Transform) && sameStyleProp(a, a, Prop::Transform));
    }

    UiContext ui({.viewport = {400, 300}});
    UI_CHECK(ui.addStyleSheet(R"css(
panel { flex-shrink: 0; }
.stage { layout: absolute; width: 400px; height: 300px; background: #000000; }
.box { width: 100px; height: 40px; background: #ff0000; }
.move { transform: translate(120px, 30px); }
.half { transform: translate(0.4px, 0px); }
.rot { transform: rotate(90deg); }
.big { transform: scale(2); transform-origin: left top; }
.tr { transition: transform 1s linear; }
.kid { width: 20px; height: 20px; background: #00ff00; }
.clip { overflow: clip; }
)css").has_value());
    const NodeHandle stage = make(ui, ui.root(), "stage");
    const NodeHandle box = make(ui, stage, "box");
    ui.setCss(box, "left", "100px");
    ui.setCss(box, "top", "100px");
    const NodeHandle kid = make(ui, box, "kid");
    ui.update(0.f);
    UI_CHECK_RECT(ui.rect(box), 100, 100, 100, 40);
    UI_CHECK(!hasOp(ui.drawList(), DrawOp::PushTransform));

    // ---- translate: exact, layout untouched ----
    ui.addClass(box, "move");
    ui.update(0.f);
    UI_CHECK_RECT(ui.rect(box), 100, 100, 100, 40); // layout does not know about transforms
    UI_CHECK(hasOp(ui.drawList(), DrawOp::PushTransform));
    {
        const Shot s(ui, 400, 300);
        UI_CHECK(s.r(150, 120) == 0 && s.r(250, 131) == 255 && s.r(319, 169) == 255 && s.r(320, 150) == 0 && s.r(219, 150) == 0);
        UI_CHECK(s.g(225, 135) == 255 && s.r(225, 135) == 0); // the child moved with it
        UI_CHECK(ui.hitTest({150, 120}) == stage && ui.hitTest({300, 160}) == box && ui.hitTest({225, 135}) == kid);
        const Rect d = ui.damage(); // the old place and the new one
        UI_CHECK(d.x <= 100 && d.y <= 100 && d.right() >= 320 && d.bottom() >= 170);
        UI_CHECK(near(ui.toLocal(box, {300, 160}).x, 180.f) && near(ui.toLocal(kid, {225, 135}).y, 105.f));
        UI_CHECK(near(ui.fromLocal(box, {100, 100}).x, 220.f) && near(ui.fromLocal(box, {100, 100}).y, 130.f));
    }
    // a fractional translation snaps to whole pixels: edges stay sharp
    ui.removeClass(box, "move");
    ui.addClass(box, "half");
    ui.update(0.f);
    {
        const Shot s(ui, 400, 300);
        UI_CHECK(s.r(99, 130) == 0 && s.r(100, 130) == 255 && s.r(199, 130) == 255 && s.r(200, 130) == 0);
    }
    ui.removeClass(box, "half");

    // ---- rotate: through a transform group ----
    ui.addClass(box, "rot");
    ui.update(0.f);
    {
        // the 100 x 40 box about its centre (150, 120) becomes 40 x 100: x 130 .. 170, y 70 .. 170
        const Shot s(ui, 400, 300);
        UI_CHECK(s.r(140, 75) == 255 && s.r(150, 165) == 255 && s.r(135, 120) == 255 && s.r(165, 120) == 255);
        UI_CHECK(s.r(110, 120) == 0 && s.r(190, 120) == 0 && s.r(150, 60) == 0 && s.r(150, 180) == 0);
        // the child sat at the box's top left (100, 100): after a quarter turn clockwise it is at the top right
        UI_CHECK(s.g(160, 80) == 255 && s.g(140, 80) == 0);
        UI_CHECK(ui.hitTest({140, 75}) == box && ui.hitTest({110, 120}) == stage && ui.hitTest({160, 80}) == kid);
        UI_CHECK(ui.hitTest({135, 160}) == box && ui.hitTest({105, 105}) == stage); // (105, 105) was the kid before
        const Vec2 l = ui.toLocal(kid, {160, 80});
        UI_CHECK(l.x >= 100 && l.x < 120 && l.y >= 100 && l.y < 120);
        const Vec2 back = ui.fromLocal(kid, l);
        UI_CHECK(near(back.x, 160.f, 1e-2f) && near(back.y, 80.f, 1e-2f));
    }
    // handlers get the pointer in the node's layout space
    {
        static Vec2 seen;
        seen = {-1, -1};
        ui.connect(kid, SignalKind::Event, [](UiContext&, NodeHandle, const SignalArgs& a) {
            if (a.event && a.event->type == EventType::PointerDown) seen = a.event->pos;
        });
        ui.pointerMove({160, 80});
        ui.pointerDown({160, 80});
        ui.pointerUp({160, 80});
        UI_CHECK(seen.x >= 100 && seen.x < 120 && seen.y >= 100 && seen.y < 120);
        UI_CHECK(ui.hovered() == kid);
        ui.pointerMove({390, 290});
    }
    ui.removeClass(box, "rot");

    // ---- scale about the top left; nested transforms compose; clips are honoured in local space ----
    ui.addClass(box, "big");
    ui.addClass(box, "clip");
    ui.setCss(kid, "transform", "translate(90px, 0)"); // half outside the box: cut by its clip
    ui.update(0.f);
    {
        const Shot s(ui, 400, 300);
        UI_CHECK(s.r(105, 105) == 255 && s.r(295, 175) == 255 && s.r(305, 140) == 0 && s.r(200, 185) == 0);
        // the kid: local x 190 .. 210 cut at 200, scaled about (100, 100): x 280 .. 300, y 100 .. 140
        UI_CHECK(s.g(290, 120) == 255 && s.g(302, 120) == 0 && s.g(270, 120) == 0);
        UI_CHECK(ui.hitTest({290, 120}) == kid && ui.hitTest({302, 120}) == stage && ui.hitTest({250, 170}) == box);
        const Vec2 l = ui.toLocal(kid, {290, 120}); // through both transforms: box (195, 110), then the kid's -90
        UI_CHECK(near(l.x, 105.f, 1e-2f) && near(l.y, 110.f, 1e-2f));
    }
    ui.removeClass(box, "big");
    ui.removeClass(box, "clip");
    ui.setCss(kid, "transform", "none");
    ui.update(0.f); // (settled before the transition rule arrives: losing scale(2) is not what is being timed)

    // ---- transitions interpolate the matrix; the damage follows the moving box ----
    ui.addClass(box, "tr");
    ui.update(0.f);
    ui.addClass(box, "move");
    ui.update(0.f);
    ui.update(0.5f);
    UI_CHECK(isMatrix(ui.computed(box).transform, 1, 0, 0, 1, 60, 15));
    {
        const Shot s(ui, 400, 300);
        UI_CHECK(s.r(200, 120) == 255 && s.r(255, 150) == 255 && s.r(265, 150) == 0);
        const Rect d = ui.damage();
        UI_CHECK(d.x <= 160 && d.right() >= 260 && d.w < 400);
    }
    ui.update(0.6f);
    UI_CHECK(isMatrix(ui.computed(box).transform, 1, 0, 0, 1, 120, 30) && !ui.styleAnimating(box));
    ui.update(0.f);
    UI_CHECK(!ui.needsUpdate());
    // removing the transform repaints where it was
    ui.removeClass(box, "tr");
    ui.removeClass(box, "move");
    ui.update(0.f);
    UI_CHECK(ui.damage().right() >= 320 && ui.damage().bottom() >= 170);
    UI_CHECK(!hasOp(ui.drawList(), DrawOp::PushTransform));

    // ---- render scale: the same picture, twice as big ----
    {
        ui.addClass(box, "rot");
        ui.update(0.f);
        const Shot one(ui, 400, 300);
        ui.setRenderScale(2.f);
        ui.update(0.f);
        const Shot two(ui, 800, 600);
        int bad = 0;
        for (int y = 0; y < 300; ++y)
            for (int x = 0; x < 400; ++x) {
                const u32 a = one.img.at(x, y);
                // the four device pixels of an interior logical pixel agree with it
                const bool edge = a != one.img.at(std::max(x - 1, 0), y) || a != one.img.at(std::min(x + 1, 399), y) ||
                                  a != one.img.at(x, std::max(y - 1, 0)) || a != one.img.at(x, std::min(y + 1, 299));
                if (!edge && two.img.at(x * 2, y * 2) != a) ++bad;
            }
        std::printf("render scale 2 vs 1: %d interior pixels differ\n", bad);
        UI_CHECK(bad == 0);
        UI_CHECK(ui.hitTest({140, 75}) == box); // hit testing stays in logical units
        ui.setRenderScale(1.f);
    }
    return finish("fuse_ui_transform_affine");
}
