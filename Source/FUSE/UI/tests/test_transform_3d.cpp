// Gate fuse_ui_transform_3d: CSS Transforms 2 (3D functions, perspective / perspective-origin, backface-visibility,
// transform-style: preserve-3d, 3D interpolation), projective CPU paint + hit testing, pointer coordinates mapped into
// transformed widgets (toLocal / fromLocal, Event::pos vs Event::client), view-timeline insets, animation-range,
// timeline-scope, CSS animations never auto-removed (Web Animations: only script animations are replaceable).
#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
NodeHandle box(UiContext& ui, NodeHandle p, std::string_view css, u32 cls = 0) {
    const NodeHandle n = ui.create(cls ? cls : ui.registry().find("panel"), p);
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
        if (!ui.setCss(n, trim(d.substr(0, colon)), trim(d.substr(colon + 1)))) { std::fprintf(stderr, "bad css: %.*s\n", int(d.size()), d.data()); UI_CHECK(false); }
    }
    return n;
}
bool near(f32 a, f32 b, f32 eps = 1e-2f) { return std::fabs(a - b) <= eps; }
bool painted(const UiContext& ui, NodeHandle n) {
    for (const DrawCmd& c : ui.drawList().cmds())
        if (c.node == n.index && c.op == DrawOp::Rect) return true;
    return false;
}
Vec2 g_pos, g_client;
int g_hits = 0;
struct Probe {
    static bool onEvent(EventContext& ec, const Event& e) {
        if (e.type == EventType::PointerMove && ec.node == ec.target) g_pos = e.pos, g_client = e.client, ++g_hits;
        return false;
    }
};
} // namespace

int main() {
    // ---- parsing + 3D matrices ---------------------------------------------------------------------------------
    {
        UiContext ui({.viewport = {200, 200}});
        const NodeHandle n = box(ui, ui.root(), "width: 20px; height: 20px; flex-shrink: 0");
        for (const char* v : {"translate3d(1px, 2px, 3px)", "translateZ(4px)", "scale3d(1, 2, 3)", "scaleZ(2)", "rotateX(30deg)",
                              "rotateY(30deg)", "rotateZ(30deg)", "rotate3d(1, 1, 0, 45deg)", "perspective(100px)",
                              "perspective(none)", "matrix3d(1,0,0,0, 0,1,0,0, 0,0,1,0, 5,6,7,1)"})
            UI_CHECK(ui.setCss(n, "transform", v));
        UI_CHECK(!ui.setCss(n, "transform", "matrix3d(1, 2, 3)"));
        UI_CHECK(!ui.setCss(n, "transform", "rotate3d(1, 1, 0)"));
        UI_CHECK(ui.setCss(n, "transform", "translate3d(1px, 2px, 3px)"));
        ui.update(0.f);
        Mat4 m = transform3dOf(ui.computed(n), Rect{});
        UI_CHECK(!m.is2D());
        UI_CHECK(near(m.at(0, 3), 1.f) && near(m.at(1, 3), 2.f) && near(m.at(2, 3), 3.f));
        UI_CHECK(ui.setCss(n, "transform", "matrix3d(1,0,0,0, 0,1,0,0, 0,0,1,0, 5,6,7,1)"));
        ui.update(0.f);
        m = transform3dOf(ui.computed(n), Rect{});
        UI_CHECK(near(m.at(0, 3), 5.f) && near(m.at(1, 3), 6.f) && near(m.at(2, 3), 7.f));
        UI_CHECK(ui.setCss(n, "perspective", "200px"));
        UI_CHECK(ui.setCss(n, "perspective-origin", "left top"));
        UI_CHECK(ui.setCss(n, "backface-visibility", "hidden"));
        UI_CHECK(ui.setCss(n, "transform-style", "preserve-3d"));
        UI_CHECK(ui.setCss(n, "transform-origin", "10px 10px 5px"));
        ui.update(0.f);
        UI_CHECK(near(ui.computed(n).perspective, 200.f));
        UI_CHECK(ui.computed(n).backfaceHidden && ui.computed(n).preserve3d);
    }
    // ---- 3D interpolation: rotateY halfway, mismatched lists through decomposition --------------------------------
    {
        UiContext ui({.viewport = {200, 200}});
        const NodeHandle a = box(ui, ui.root(), "width: 10px; height: 10px; flex-shrink: 0");
        AnimationOptions o;
        o.duration = 1.f;
        ui.animate(a, "from { transform: rotateY(0deg) } to { transform: rotateY(60deg) }", o);
        ui.update(0.f);
        ui.update(0.5f);
        Mat4 m = transform3dOf(ui.computed(a), Rect{});
        UI_CHECK(near(m.at(0, 0), std::cos(0.5236f)) && near(m.at(0, 2), std::sin(0.5236f)));
        const NodeHandle b = box(ui, ui.root(), "width: 10px; height: 10px; flex-shrink: 0");
        ui.animate(b, "from { transform: translateZ(0px) } to { transform: rotateX(90deg) translateZ(10px) }", o);
        ui.update(0.f);
        ui.update(0.5f);
        m = transform3dOf(ui.computed(b), Rect{});
        UI_CHECK(!m.is2D());
    }
    // ---- perspective scales by d / (d - z) about perspective-origin; toLocal / fromLocal round trip --------------
    {
        UiContext ui({.viewport = {200, 200}});
        const NodeHandle p = box(ui, ui.root(), "width: 200px; height: 200px; flex-shrink: 0; perspective: 100px");
        const NodeHandle c = box(ui, p, "width: 200px; height: 200px; flex-shrink: 0; transform: translateZ(50px)");
        ui.update(0.f);
        Vec2 s, l;
        UI_CHECK(ui.fromLocal(c, {150.f, 100.f}, s));
        UI_CHECK(near(s.x, 200.f) && near(s.y, 100.f));
        UI_CHECK(ui.toLocal(c, {150.f, 120.f}, l));
        UI_CHECK(near(l.x, 125.f) && near(l.y, 110.f));
        UI_CHECK(ui.hitTest({195.f, 100.f}) == c);
    }
    // ---- projective CPU paint + hit testing: rotateY(60deg) under perspective narrows the box ----------------------
    {
        UiContext ui({.viewport = {200, 200}});
        const NodeHandle p = box(ui, ui.root(), "width: 200px; height: 200px; flex-shrink: 0; perspective: 400px");
        const NodeHandle c = box(ui, p, "width: 100px; height: 100px; margin: 50px; flex-shrink: 0; background: #f00; transform: rotateY(60deg)");
        ui.update(0.f);
        Image img;
        img.resize(200, 200, 0xFF000000u);
        CpuRasterizer().render(ui.drawList(), img);
        auto red = [&](int x, int y) { return (img.at(x, y) & 0xFF) > 128; }; // RGBA8, R in the low byte
        UI_CHECK(red(100, 100));
        UI_CHECK(!red(55, 100) && !red(145, 100)); // the 50..150 box is now ~25 px narrower on each side
        UI_CHECK(ui.hitTest({100.f, 100.f}) == c);
        UI_CHECK(ui.hitTest({55.f, 100.f}) != c);
        // back faces: rotateY(180deg) + backface-visibility: hidden paints nothing
        UI_CHECK(ui.setCss(c, "transform", "rotateY(180deg)"));
        ui.update(0.f);
        UI_CHECK(painted(ui, c));
        UI_CHECK(ui.setCss(c, "backface-visibility", "hidden"));
        ui.update(0.f);
        UI_CHECK(!painted(ui, c));
    }
    // ---- preserve-3d: children paint as their own planes, nearest last -------------------------------------------
    {
        UiContext ui({.viewport = {200, 200}});
        const NodeHandle p = box(ui, ui.root(), "width: 200px; height: 200px; flex-shrink: 0; perspective: 400px");
        const NodeHandle g = box(ui, p, "width: 200px; height: 200px; flex-shrink: 0; transform-style: preserve-3d; transform: rotateY(1deg)");
        const NodeHandle nearN = box(ui, g, "flex-shrink: 0; margin-left: 50px; margin-top: 50px; width: 100px; height: 100px; background: #f00; transform: translateZ(20px)");
        const NodeHandle farN = box(ui, g, "flex-shrink: 0; margin-left: 50px; margin-top: -100px; width: 100px; height: 100px; background: #00f; transform: translateZ(-20px)");
        ui.update(0.f);
        UI_CHECK(ui.hitTest({100.f, 100.f}) == nearN); // document order puts farN on top; depth says nearN
        (void)farN;
        Image img;
        img.resize(200, 200, 0xFF000000u);
        CpuRasterizer().render(ui.drawList(), img);
        const u32 px = img.at(100, 100);
        UI_CHECK((px & 0xFF) > 128 && ((px >> 16) & 0xFF) < 64); // red (low byte) on top, not blue
    }
    // ---- dispatch: Event::pos in the receiver's local space, client untouched -------------------------------------
    {
        UiContext ui({.viewport = {200, 200}});
        WidgetClass wc;
        wc.name = "probe3d";
        wc.interactive = true;
        wc.onEvent = &Probe::onEvent;
        const u32 cls = ui.registry().add(wc);
        const NodeHandle p = box(ui, ui.root(), "width: 200px; height: 200px; flex-shrink: 0; perspective: 300px");
        const NodeHandle c = box(ui, p, "width: 100px; height: 100px; margin: 50px; flex-shrink: 0; transform: rotateX(40deg) rotateZ(10deg)", cls);
        ui.update(0.f);
        g_hits = 0;
        ui.pointerMove({110.f, 95.f});
        UI_CHECK(g_hits == 1);
        Vec2 l;
        UI_CHECK(ui.toLocal(c, {110.f, 95.f}, l));
        UI_CHECK(near(g_pos.x, l.x) && near(g_pos.y, l.y));
        UI_CHECK(near(g_client.x, 110.f) && near(g_client.y, 95.f));
        UI_CHECK(!near(g_pos.x, 110.f) || !near(g_pos.y, 95.f));
    }
    // ---- view timelines: insets, animation-range, timeline-scope (CSS) --------------------------------------------
    {
        UiContext ui({.viewport = {200, 400}});
        UI_CHECK(ui.addStyleSheet("@keyframes fade { from { opacity: 0 } to { opacity: 1 } }").has_value());
        const NodeHandle top = box(ui, ui.root(), "flex-shrink: 0; timeline-scope: --t");
        const NodeHandle sc = box(ui, top, "height: 100px; overflow: scroll; flex-shrink: 0");
        box(ui, sc, "height: 300px; flex-shrink: 0");
        const NodeHandle subj = box(ui, sc, "height: 50px; flex-shrink: 0; view-timeline: --t");
        box(ui, sc, "height: 300px; flex-shrink: 0");
        const NodeHandle a = box(ui, subj, "height: 1px; flex-shrink: 0; animation: fade 1s linear; animation-timeline: view(block 25px)");
        (void)a; // parses view(<axis> <inset>)
        UI_CHECK(ui.setCss(subj, "animation", "fade 1s linear"));
        UI_CHECK(ui.setCss(subj, "animation-timeline", "view(25px)"));
        // entry range on a node outside the scroller, through the scoped name from outside the scroller
        const NodeHandle other = box(ui, top, "height: 10px; flex-shrink: 0; animation: fade 1s linear; animation-timeline: --t; animation-range: entry 0% entry 100%");
        ui.update(0.f);
        ui.scrollTo(sc, {0.f, 275.f}); // subject top at 25; insets 25: (75 - 25) / (50 + 50)
        ui.update(0.f);
        UI_CHECK(near(ui.computed(subj).opacity, 0.5f));
        // entry: top 100 -> 50 over 50 px; top 25 is past the end (no fill: base opacity 1)
        UI_CHECK(near(ui.computed(other).opacity, 1.f));
        ui.scrollTo(sc, {0.f, 225.f}); // top at 75: halfway through entry
        ui.update(0.f);
        UI_CHECK(near(ui.computed(other).opacity, 0.5f));
        // without the scope the name is not visible outside the scroller: inactive
        UI_CHECK(ui.setCss(top, "timeline-scope", "none"));
        ui.update(0.f);
        UI_CHECK(near(ui.computed(other).opacity, 1.f));
    }
    // ---- CSS animations are never auto-removed --------------------------------------------------------------------
    {
        UiContext ui({.viewport = {200, 200}});
        UI_CHECK(ui.addStyleSheet("@keyframes a { to { opacity: 0.2 } } @keyframes b { to { opacity: 0.7 } } "
                                  "@keyframes c { to { opacity: 1 } }").has_value());
        const NodeHandle n = box(ui, ui.root(), "width: 10px; height: 10px; flex-shrink: 0; animation: a 0.1s forwards, b 0.1s forwards");
        ui.update(0.f);
        for (int i = 0; i < 5; ++i) ui.update(0.1f);
        UI_CHECK(near(ui.computed(n).opacity, 0.7f));
        // b -> c (10 s): c starts from the underlying value, which is a's fill only if a was kept
        UI_CHECK(ui.setCss(n, "animation", "a 0.1s forwards, c 10s linear forwards"));
        ui.update(0.f);
        UI_CHECK(near(ui.computed(n).opacity, 0.2f, 0.02f));
    }
    return finish("fuse_ui_transform_3d");
}
