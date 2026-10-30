// Gate fuse_ui_transform_affine: full 2D affine transforms and the rest of the Task 10 motion work.
//   * parse: rotate (deg / rad / turn / grad), skew / skewX / skewY, matrix(6), transform-origin keywords / % / px
//   * interpolation of mismatched lists through the decomposed form (shortest angle), matrix composition
//   * paint: a rotated box goes through a transform layer (DrawList::pushTransform); CPU pixels land where expected
//   * hit testing maps the point through the inverse transform; damage covers the transformed box
//   * CSS animation-timeline: scroll(), view(), named scroll-timeline / view-timeline, none
//   * KeyframeEffect retarget (setKeyframes, target) and replace: auto-removal of fully overridden finished
//     animations, persist()
#include "test_util.hpp"

#include <cmath>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
NodeHandle box(UiContext& ui, NodeHandle p, std::string_view css) {
    const NodeHandle n = ui.create(ui.registry().find("panel"), p);
    std::string_view rest = css;
    while (!rest.empty()) {
        const auto semi = rest.find(';');
        std::string_view d = rest.substr(0, semi);
        rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
        const auto colon = d.find(':');
        if (colon == std::string_view::npos) continue;
        auto trim = [](std::string_view s) {
            while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
            while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
            return s;
        };
        UI_CHECK(ui.setCss(n, trim(d.substr(0, colon)), trim(d.substr(colon + 1))));
    }
    return n;
}
bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }
Affine2 parsed(std::string_view css) {
    ComputedStyle s;
    PropValue v;
    if (!parsePropValue(Prop::Transform, css, v)) return Affine2{0, 0, 0, 0, 0, 0};
    applyProp(s, Prop::Transform, v);
    s.transformOrigin = {Length::px(0.f), Length::px(0.f)};
    return transformOf(s, Rect{});
}
u32 ch(u32 p, int c) { return (p >> (8 * c)) & 0xFFu; }
} // namespace

int main() {
    // ---- parse ---------------------------------------------------------------------------------------------------
    {
        Affine2 m = parsed("rotate(90deg)");
        UI_CHECK(near(m.a, 0.f) && near(m.b, 1.f) && near(m.c, -1.f) && near(m.d, 0.f));
        UI_CHECK(near(parsed("rotate(0.25turn)").b, 1.f) && near(parsed("rotate(100grad)").b, 1.f));
        UI_CHECK(near(parsed("rotate(3.14159265rad)").a, -1.f));
        m = parsed("skewX(45deg)");
        UI_CHECK(near(m.a, 1.f) && near(m.c, 1.f) && near(m.b, 0.f));
        m = parsed("skewY(45deg)");
        UI_CHECK(near(m.b, 1.f) && near(m.c, 0.f));
        m = parsed("skew(45deg, 45deg)"); // its own matrix, not skewX then skewY
        UI_CHECK(near(m.b, 1.f) && near(m.c, 1.f) && near(m.a, 1.f) && near(m.d, 1.f));
        m = parsed("matrix(1, 2, 3, 4, 5, 6)");
        UI_CHECK(near(m.a, 1.f) && near(m.b, 2.f) && near(m.c, 3.f) && near(m.d, 4.f) && near(m.e, 5.f) && near(m.f, 6.f));
        m = parsed("translate(10px) rotate(90deg)"); // translate applies last
        const Vec2 q = m.apply({1.f, 0.f});
        UI_CHECK(near(q.x, 10.f) && near(q.y, 1.f));
        PropValue v;
        UI_CHECK(!parsePropValue(Prop::Transform, "rotate(10px)", v));
        UI_CHECK(!parsePropValue(Prop::Transform, "matrix(1, 2, 3)", v));
        UI_CHECK(!parsePropValue(Prop::Transform, "rotate(10)", v));
        UI_CHECK(parsePropValue(Prop::Transform, "rotate(0)", v));
    }
    // ---- decompose / interpolate ---------------------------------------------------------------------------------
    {
        const Affine2 m = parsed("translate(3px, 4px) rotate(30deg) skewX(10deg) scale(2, 0.5)");
        const Affine2 r = recompose(decompose(m));
        UI_CHECK(near(r.a, m.a) && near(r.b, m.b) && near(r.c, m.c) && near(r.d, m.d) && near(r.e, m.e) && near(r.f, m.f));
        PropValue a, b;
        UI_CHECK(parsePropValue(Prop::Transform, "rotate(0deg)", a) && parsePropValue(Prop::Transform, "rotate(90deg)", b));
        ComputedStyle s;
        applyProp(s, Prop::Transform, interpolateProp(a, b, 0.5f));
        s.transformOrigin = {Length::px(0.f), Length::px(0.f)};
        Affine2 h = transformOf(s, Rect{});
        UI_CHECK(near(h.a, std::cos(0.785398f)) && near(h.b, std::sin(0.785398f)));
        // mismatched lists (rotate vs matrix): decomposed, the angle takes the short way
        UI_CHECK(parsePropValue(Prop::Transform, "rotate(170deg)", a) && parsePropValue(Prop::Transform, "matrix(-1, -0.1745, 0.1745, -1, 0, 0)", b));
        applyProp(s, Prop::Transform, interpolateProp(a, b, 0.5f));
        h = transformOf(s, Rect{});
        UI_CHECK(h.a < -0.95f); // near 180deg, not swinging through 0deg
    }
    // ---- paint, origin, hit test, damage ---------------------------------------------------------------------------
    {
        UiContext ui({.viewport = {200, 200}});
        const NodeHandle stage = box(ui, ui.root(), "layout: absolute; width: 200px; height: 200px; flex-shrink: 0");
        const NodeHandle n = box(ui, stage, "left: 50px; top: 90px; width: 100px; height: 20px; background: #ff0000");
        ui.update(0.f);
        UI_CHECK(ui.hitTest({60.f, 100.f}) == n);
        UI_CHECK(!(ui.hitTest({100.f, 60.f}) == n));
        UI_CHECK(ui.setCss(n, "transform", "rotate(90deg)")); // about the centre (100, 100): now 90..110 x 50..150
        ui.update(0.f);
        UI_CHECK(ui.hitTest({100.f, 60.f}) == n);
        UI_CHECK(!(ui.hitTest({60.f, 100.f}) == n));
        const Rect d = ui.damage();
        UI_CHECK(d.x <= 50.f && d.right() >= 150.f && d.y <= 50.f && d.bottom() >= 150.f);
        bool layer = false;
        for (const DrawCmd& c : ui.drawList().cmds()) layer = layer || isTransformLayer(c);
        UI_CHECK(layer);
        Image img;
        img.resize(200, 200, 0xFF000000u);
        CpuRasterizer r;
        r.render(ui.drawList(), img);
        UI_CHECK(ch(img.at(100, 60), 0) > 240 && ch(img.at(100, 60), 1) < 10); // inside the rotated box
        UI_CHECK(ch(img.at(60, 100), 0) < 10);                                 // the old place is empty
        // transform-origin: rotate about the top-left corner (50, 90): the box goes to 30..50 x 90..190
        UI_CHECK(ui.setCss(n, "transform-origin", "left top"));
        ui.update(0.f);
        UI_CHECK(ui.hitTest({40.f, 150.f}) == n && !(ui.hitTest({100.f, 60.f}) == n));
        UI_CHECK(ui.setCss(n, "transform-origin", "0px 0%"));
        ui.update(0.f);
        UI_CHECK(ui.hitTest({40.f, 150.f}) == n);
        // singular: not painted, not hit
        UI_CHECK(ui.setCss(n, "transform", "scale(0)"));
        ui.update(0.f);
        UI_CHECK(!(ui.hitTest({40.f, 150.f}) == n) && !(ui.hitTest({100.f, 100.f}) == n));
        // a transformed parent maps its children for hit testing
        UI_CHECK(ui.setCss(n, "transform", "none"));
        const NodeHandle p = box(ui, stage, "left: 0px; top: 0px; width: 40px; height: 40px; transform: rotate(180deg)");
        const NodeHandle c = box(ui, p, "width: 10px; height: 10px; flex-shrink: 0");
        ui.update(0.f);
        UI_CHECK(ui.hitTest({35.f, 35.f}) == c && !(ui.hitTest({5.f, 5.f}) == c));
    }
    // ---- CSS animation-timeline ----------------------------------------------------------------------------------
    {
        UiContext ui({.viewport = {200, 400}});
        UI_CHECK(ui.addStyleSheet(R"css(
@keyframes fadein { from { opacity: 0; } to { opacity: 1; } }
.s { animation: fadein linear; animation-timeline: scroll(); }
.r { animation: fadein linear; animation-timeline: scroll(self); }
.v { animation: fadein linear; animation-timeline: view(); }
.named { scroll-timeline: --main y; }
.n { animation: fadein 2s linear; animation-timeline: --main; }
.x { animation: fadein linear; animation-timeline: --missing; opacity: 0.5; }
.no { animation: fadein linear; animation-timeline: none; opacity: 0.25; }
)css").has_value());
        const NodeHandle sc = box(ui, ui.root(), "height: 100px; overflow: scroll; flex-shrink: 0");
        ui.addClass(sc, "named");
        const NodeHandle s = box(ui, sc, "height: 100px; flex-shrink: 0");
        ui.addClass(s, "s");
        const NodeHandle n = box(ui, sc, "height: 50px; flex-shrink: 0");
        ui.addClass(n, "n");
        const NodeHandle x = box(ui, sc, "height: 50px; flex-shrink: 0");
        ui.addClass(x, "x");
        const NodeHandle no = box(ui, sc, "height: 50px; flex-shrink: 0");
        ui.addClass(no, "no");
        const NodeHandle v = box(ui, sc, "height: 50px; flex-shrink: 0");
        ui.addClass(v, "v");
        box(ui, sc, "height: 300px; flex-shrink: 0"); // content 600: range 500
        ui.update(0.f);
        UI_CHECK(near(ui.computed(s).opacity, 0.f) && near(ui.computed(n).opacity, 0.f));
        UI_CHECK(near(ui.computed(x).opacity, 0.5f) && near(ui.computed(no).opacity, 0.25f));
        UI_CHECK(!ui.animating());
        ui.scrollTo(sc, {0.f, 250.f});
        ui.update(0.f);
        UI_CHECK(near(ui.computed(s).opacity, 0.5f) && near(ui.computed(n).opacity, 0.5f));
        ui.update(10.f); // time does not move them
        UI_CHECK(near(ui.computed(s).opacity, 0.5f));
        // view(): subject at content y 250..300, port 100: at scroll 250 its top is at the port top: (100 - 0) / 150
        UI_CHECK(near(ui.computed(v).opacity, 2.f / 3.f, 2e-3f));
        // scroll(self) on a box that cannot scroll: inactive (no effect)
        const NodeHandle r = box(ui, ui.root(), "height: 10px; flex-shrink: 0; opacity: 0.75");
        ui.addClass(r, "r");
        ui.update(0.f);
        UI_CHECK(near(ui.computed(r).opacity, 0.75f));
    }
    // ---- retarget ------------------------------------------------------------------------------------------------
    {
        UiContext ui({.viewport = {200, 200}});
        const NodeHandle a = box(ui, ui.root(), "width: 10px; height: 10px; flex-shrink: 0");
        const NodeHandle b = box(ui, ui.root(), "width: 10px; height: 10px; flex-shrink: 0");
        AnimationOptions o;
        o.duration = 1.f;
        const auto h = ui.animate(a, "from { opacity: 0 } to { opacity: 0 }", o);
        ui.update(0.f);
        ui.update(0.5f);
        UI_CHECK(near(ui.computed(a).opacity, 0.f));
        UI_CHECK(ui.animSetKeyframes(h, "from { opacity: 0 } to { opacity: 1 }"));
        ui.update(0.f);
        UI_CHECK(near(ui.computed(a).opacity, 0.5f) && near(ui.animCurrentTime(h), 0.5f));
        UI_CHECK(!ui.animSetKeyframes(h, std::vector<AnimKeyframe>{}));
        UI_CHECK(ui.animSetTarget(h, b));
        ui.update(0.f);
        UI_CHECK(near(ui.computed(a).opacity, 1.f) && near(ui.computed(b).opacity, 0.5f));
        UI_CHECK(ui.animTarget(h) == b);
        UI_CHECK(ui.animations(a).empty() && ui.animations(b).size() == 1);
    }
    // ---- replace / persist ---------------------------------------------------------------------------------------
    {
        UiContext ui({.viewport = {200, 200}});
        const NodeHandle n = box(ui, ui.root(), "width: 10px; height: 10px; flex-shrink: 0");
        int removed = 0;
        AnimationOptions o;
        o.duration = 0.1f;
        o.fill = AnimFill::Forwards;
        o.onRemove = [&](UiContext&, AnimationHandle) { ++removed; };
        const auto h1 = ui.animate(n, "to { opacity: 0.2; width: 20px }", o);
        const auto h2 = ui.animate(n, "to { opacity: 0.4 }", o);
        ui.update(0.f);
        ui.update(0.2f);
        UI_CHECK(ui.animReplaceState(h1) == AnimReplaceState::Active); // width is not overridden
        const auto h3 = ui.animate(n, "to { width: 30px }", o);
        ui.update(0.f);
        ui.update(0.2f);
        ui.update(0.f);
        UI_CHECK(ui.animReplaceState(h1) == AnimReplaceState::Removed && removed == 1);
        UI_CHECK(ui.animReplaceState(h2) == AnimReplaceState::Active && ui.animReplaceState(h3) == AnimReplaceState::Active);
        UI_CHECK(near(ui.computed(n).opacity, 0.4f) && near(ui.computed(n).width.value, 30.f));
        UI_CHECK(ui.animations(n).size() == 2);
        // persist() brings it back (under the later ones) and keeps it
        UI_CHECK(ui.animPersist(h1));
        ui.update(0.f);
        UI_CHECK(ui.animReplaceState(h1) == AnimReplaceState::Persisted && ui.animations(n).size() == 3);
        // a later one replacing a persisted one still removes active ones only
        const auto h4 = ui.animate(n, "to { opacity: 0.6; width: 40px }", o);
        ui.update(0.f);
        ui.update(0.2f);
        ui.update(0.f);
        UI_CHECK(ui.animReplaceState(h1) == AnimReplaceState::Persisted);
        UI_CHECK(ui.animReplaceState(h2) == AnimReplaceState::Removed && ui.animReplaceState(h3) == AnimReplaceState::Removed);
        UI_CHECK(ui.animReplaceState(h4) == AnimReplaceState::Active && removed == 3);
        UI_CHECK(near(ui.computed(n).opacity, 0.6f));
        // without fill a finished animation has no effect: never removed
        AnimationOptions nf;
        nf.duration = 0.1f;
        const auto h5 = ui.animate(n, "to { opacity: 0.1 }", nf);
        const auto h6 = ui.animate(n, "to { opacity: 0.1 }", nf);
        ui.update(0.f);
        ui.update(0.2f);
        ui.update(0.f);
        UI_CHECK(ui.animReplaceState(h5) == AnimReplaceState::Active && ui.animReplaceState(h6) == AnimReplaceState::Active);
    }
    return finish("fuse_ui_transform_affine");
}
