// Gate fuse_ui_transform_timelines: `transform` lists (translate / scale parse, paint mapping about the box centre,
// per-function interpolation), animation composite add vs accumulate on transform lists (add appends, accumulate
// sums matching functions), the document timeline (documentTime, animStartTime) and progress-based scroll / view
// timelines (Scroll-driven Animations: progress follows the scroll offset, inactive without overflow, no frames
// requested while idle).
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
Affine2 xf(UiContext& ui, NodeHandle n) { return transformOf(ui.computed(n), Rect{}); }
const DrawCmd* firstRect(const UiContext& ui, NodeHandle n) {
    for (const DrawCmd& c : ui.drawList().cmds())
        if (c.node == n.index && c.op == DrawOp::Rect) return &c;
    return nullptr;
}
} // namespace

int main() {
    // ---- parse + paint -----------------------------------------------------------------------------------------
    {
        UiContext ui({.viewport = {200, 200}});
        const NodeHandle n = box(ui, ui.root(), "width: 20px; height: 10px; background: #f00; flex-shrink: 0");
        UI_CHECK(ui.setCss(n, "transform", "translate(10px, 5px) scale(2)"));
        UI_CHECK(!ui.setCss(n, "transform", "frobnicate(1)"));
        UI_CHECK(!ui.setCss(n, "transform", "translate(1em)"));
        ui.update(0.f);
        UI_CHECK_EQ(int(ui.computed(n).transformCount), 4);
        const Affine2 m = xf(ui, n);
        UI_CHECK(near(m.a, 2.f) && near(m.d, 2.f) && near(m.e, 10.f) && near(m.f, 5.f));
        // painted about the centre (10, 5): x' = 10 + 2 (x - 10) + 10 -> the 0..20 box spans 0..40
        const Rect r = ui.rect(n);
        UI_CHECK_RECT(r, 0, 0, 20, 10);
        const DrawCmd* c = firstRect(ui, n);
        UI_CHECK(c != nullptr);
        if (c) UI_CHECK(near(c->rect.x, 0.f) && near(c->rect.y, 0.f) && near(c->rect.w, 40.f) && near(c->rect.h, 20.f));
        UI_CHECK(ui.setCss(n, "transform", "none"));
        ui.update(0.f);
        c = firstRect(ui, n);
        if (c) UI_CHECK(near(c->rect.x, 0.f) && near(c->rect.w, 20.f));
        UI_CHECK(ui.damage().w >= 40.f); // the old transformed box repaints
        // a translated parent moves its subtree
        const NodeHandle p = box(ui, ui.root(), "width: 50px; height: 50px; flex-shrink: 0; transform: translateX(30px)");
        const NodeHandle k = box(ui, p, "width: 10px; height: 10px; background: #0f0; flex-shrink: 0");
        ui.update(0.f);
        c = firstRect(ui, k);
        if (c) UI_CHECK(near(c->rect.x, ui.rect(k).x + 30.f));
    }
    // ---- interpolation + composite add vs accumulate -----------------------------------------------------------
    {
        UiContext ui({.viewport = {200, 200}});
        const NodeHandle a = box(ui, ui.root(), "width: 10px; height: 10px; flex-shrink: 0");
        AnimationOptions o;
        o.duration = 1.f;
        auto h = ui.animate(a, "from { transform: translateX(0px) } to { transform: translateX(100px) }", o);
        UI_CHECK(h.valid());
        ui.update(0.f);
        ui.update(0.5f);
        UI_CHECK(near(xf(ui, a).e, 50.f));
        ui.animCancel(h);
        // mismatched lists interpolate the composed maps
        h = ui.animate(a, "from { transform: scale(1) } to { transform: translateX(100px) scaleX(3) }", o);
        ui.update(0.f);
        ui.update(0.5f);
        UI_CHECK(near(xf(ui, a).e, 50.f) && near(xf(ui, a).a, 2.f) && near(xf(ui, a).d, 1.f));
        ui.animCancel(h);

        auto composite = [&](AnimComposite cm, const char* under, const char* effect) {
            const NodeHandle n = box(ui, ui.root(), "width: 10px; height: 10px; flex-shrink: 0");
            UI_CHECK(ui.setCss(n, "transform", under));
            AnimationOptions co;
            co.duration = 1.f;
            co.fill = AnimFill::Forwards;
            co.composite = cm;
            ui.animate(n, std::string("from { transform: ") + effect + " } to { transform: " + effect + " }", co);
            ui.update(0.f);
            ui.update(2.f);
            return xf(ui, n);
        };
        Affine2 add = composite(AnimComposite::Add, "scale(2)", "scale(3)");
        Affine2 acc = composite(AnimComposite::Accumulate, "scale(2)", "scale(3)");
        Affine2 rep = composite(AnimComposite::Replace, "scale(2)", "scale(3)");
        UI_CHECK(near(add.a, 6.f) && near(add.d, 6.f)); // scale(2) scale(3)
        UI_CHECK(near(acc.a, 4.f) && near(acc.d, 4.f)); // scale(2 + 3 - 1)
        UI_CHECK(near(rep.a, 3.f));
        add = composite(AnimComposite::Add, "scale(2)", "translateX(10px)");
        acc = composite(AnimComposite::Accumulate, "scale(2)", "translateX(10px)");
        UI_CHECK(near(add.e, 20.f) && near(add.a, 2.f));  // translate in the scaled space
        UI_CHECK(near(acc.e, 10.f) && near(acc.a, 2.f));  // composed maps summed
        add = composite(AnimComposite::Add, "translateX(5px)", "translateX(7px)");
        acc = composite(AnimComposite::Accumulate, "translateX(5px)", "translateX(7px)");
        UI_CHECK(near(add.e, 12.f) && near(acc.e, 12.f)); // translations agree either way
        UI_CHECK_EQ(int(ui.computed(ui.lastChild(ui.root())).transformCount), 1); // accumulate keeps one function
        // compositeProp directly: add appends
        PropValue u, v;
        UI_CHECK(parsePropValue(Prop::Transform, "scale(2)", u) && parsePropValue(Prop::Transform, "translateY(4px)", v));
        UI_CHECK_EQ(int(compositeProp(u, v, AnimComposite::Add).trackCount), 3);
        UI_CHECK_EQ(int(compositeProp(u, v, AnimComposite::Accumulate).trackCount), 4); // canonical form
    }
    // ---- document timeline -------------------------------------------------------------------------------------
    {
        UiContext ui({.viewport = {200, 200}});
        const NodeHandle n = box(ui, ui.root(), "width: 10px; height: 10px");
        UI_CHECK_EQ(ui.documentTime(), 0.0);
        ui.update(0.25f);
        ui.update(0.25f);
        UI_CHECK(near(f32(ui.documentTime()), 0.5f));
        AnimationOptions o;
        o.duration = 2.f;
        const auto h = ui.animate(n, "to { opacity: 0 }", o);
        ui.update(0.25f);
        // it starts on the first frame after animate() (the pending play task): start = that frame's time
        UI_CHECK(near(f32(ui.animStartTime(h)), 0.75f));
        ui.update(0.25f);
        UI_CHECK(near(f32(ui.animStartTime(h)), 0.75f) && near(ui.animCurrentTime(h), 0.25f));
        UI_CHECK(near(f32(ui.documentTime()), 1.f));
        ui.animPause(h);
        UI_CHECK(std::isnan(ui.animStartTime(h)));
        UI_CHECK(std::isnan(ui.animTimelineProgress(h)));
    }
    // ---- scroll timeline ---------------------------------------------------------------------------------------
    {
        UiContext ui({.viewport = {200, 200}});
        const NodeHandle sc = box(ui, ui.root(), "height: 100px; overflow: scroll; flex-shrink: 0");
        const NodeHandle item = box(ui, sc, "height: 500px; flex-shrink: 0");
        AnimationOptions o;
        o.timeline.kind = AnimTimeline::Kind::Scroll;
        const auto h = ui.animate(item, "from { opacity: 0 } to { opacity: 1 }", o);
        UI_CHECK(h.valid());
        ui.update(0.f);
        UI_CHECK(near(ui.animTimelineProgress(h), 0.f));
        UI_CHECK(near(ui.computed(item).opacity, 0.f));
        ui.scrollTo(sc, {0.f, 200.f}); // range 400
        ui.update(0.f);
        UI_CHECK(near(ui.animTimelineProgress(h), 0.5f));
        UI_CHECK(near(ui.computed(item).opacity, 0.5f));
        UI_CHECK(!ui.animating()); // time does not drive it: the app may idle
        ui.update(5.f);
        UI_CHECK(near(ui.computed(item).opacity, 0.5f));
        UI_CHECK(ui.animPlayState(h) == AnimPlayState::Running);
        ui.scrollTo(sc, {0.f, 400.f});
        ui.update(0.f);
        UI_CHECK(near(ui.computed(item).opacity, 1.f));
        UI_CHECK(ui.animPlayState(h) == AnimPlayState::Running); // progress timelines do not finish
        // explicit source + x axis without overflow: inactive (no effect)
        AnimationOptions ox;
        ox.timeline.kind = AnimTimeline::Kind::Scroll;
        ox.timeline.source = sc;
        ox.timeline.axis = 0;
        const NodeHandle other = box(ui, ui.root(), "width: 10px; height: 10px; flex-shrink: 0; opacity: 0.25");
        const auto hx = ui.animate(other, "from { opacity: 1 } to { opacity: 1 }", ox);
        ui.update(0.f);
        UI_CHECK(std::isnan(ui.animTimelineProgress(hx)));
        UI_CHECK(near(ui.computed(other).opacity, 0.25f));
        // the reverse playback rate runs the timeline backwards
        ui.animSetPlaybackRate(h, -1.f);
        ui.scrollTo(sc, {0.f, 100.f});
        ui.update(0.f);
        UI_CHECK(near(ui.computed(item).opacity, 0.75f));
    }
    // ---- view timeline -----------------------------------------------------------------------------------------
    {
        UiContext ui({.viewport = {200, 400}});
        const NodeHandle sc = box(ui, ui.root(), "height: 100px; overflow: scroll; flex-shrink: 0");
        box(ui, sc, "height: 300px; flex-shrink: 0");
        const NodeHandle subj = box(ui, sc, "height: 50px; flex-shrink: 0");
        box(ui, sc, "height: 300px; flex-shrink: 0");
        AnimationOptions o;
        o.timeline.kind = AnimTimeline::Kind::View;
        const auto h = ui.animate(subj, "from { opacity: 0 } to { opacity: 1 }", o);
        ui.update(0.f);
        UI_CHECK(near(ui.computed(subj).opacity, 0.f)); // below the scrollport: before the range
        ui.scrollTo(sc, {0.f, 275.f}); // subject top at 25: (100 - 25) / (100 + 50)
        ui.update(0.f);
        UI_CHECK(near(ui.animTimelineProgress(h), 0.5f));
        UI_CHECK(near(ui.computed(subj).opacity, 0.5f));
        // insets shrink the scrollport: 25..75 -> (75 - 25) / (50 + 50)
        AnimationOptions oi = o;
        oi.timeline.insetStart = oi.timeline.insetEnd = 25.f;
        const auto hi = ui.animate(subj, "from { width: 0px } to { width: 100px }", oi);
        ui.update(0.f);
        UI_CHECK(near(ui.animTimelineProgress(hi), 0.5f));
        UI_CHECK(near(ui.computed(subj).width.value, 50.f));
    }
    return finish("fuse_ui_transform_timelines");
}
