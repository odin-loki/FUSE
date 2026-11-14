// Gate fuse_ui_css_animations: CSS animations on the style engine - @keyframes and the `animation` shorthand /
// animation-* longhands (name, duration, timing-function, delay, iteration-count, direction, fill-mode, play-state).
//   * parsing: the shorthand in any order, comma lists, list cycling of the longhands, keyframe selectors
//   * keyframe timing: values at exact times for linear / eased segments, implicit 0% / 100% keyframes (the underlying
//     value), per-keyframe easing, delay (positive and negative), iteration count (whole, fractional, infinite),
//     direction (reverse / alternate / alternate-reverse), fill modes before and after
//   * play-state: paused holds, running resumes where it was; removing the name ends the animation; display: none
//     ends it and showing restarts it; a retimed animation keeps its position
//   * several animations on one node, var() in keyframes, a later sheet replacing the @keyframes rule
//   * AnimationFinished, needsUpdate() while running and at rest, reduced motion, node destruction mid-flight,
//     no style-table growth and no allocation per animated frame
// (Adapted from the gui-framework gate of the same name: cubic-bezier / steps, animation events other than the end,
// transition longhands and animation-composition are not ported.)
#include "test_alloc_hooks.hpp"
#include "test_util.hpp"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;

static long g_allocs = 0;
static bool g_count = false;
// Every global allocation form is replaced and counted (test_alloc_hooks.hpp).
void fuse::ui::test::countAlloc(std::size_t) noexcept {
    if (g_count) ++g_allocs;
}

namespace {
bool near(f32 a, f32 b, f32 eps = 2e-3f) { return std::fabs(a - b) <= eps; }
NodeHandle make(UiContext& ui, std::string_view cls) {
    const NodeHandle n = ui.create(ui.registry().find("panel"), ui.root());
    if (!cls.empty()) ui.addClass(n, cls);
    return n;
}
const char* kSheet = R"css(
:root { --mid: 0.5; }
@keyframes fade { from { opacity: 0; } to { opacity: 1; } }
@keyframes grow { 0% { width: 100px; } 50% { width: 200px; animation-timing-function: linear; } 100% { width: 400px; } }
@keyframes pulse { 50% { opacity: var(--mid); } }
@keyframes mixed { from { opacity: 0.2; width: 10px; } 25%, 75% { opacity: 0.8; } to { opacity: 0.4; width: 50px; } }
@keyframes hide { from { text-align: center; } to { text-align: right; } }
@keyframes turn { from { transform: rotate(0deg); } to { transform: rotate(90deg); } }
panel { width: 20px; height: 20px; flex-shrink: 0; }
.fade { animation: fade 1s linear; }
.ease { animation: fade 1s ease-in; }
.grow { animation-name: grow; animation-duration: 2s; animation-timing-function: ease-in; }
.pulse { opacity: 1; animation: pulse 2s linear; }
.delay { animation: fade 1s linear 0.5s; }
.early { animation: fade 1s linear -0.25s; }
.back { opacity: 0.7; animation: fade 1s linear 0.5s backwards; }
.fwd { opacity: 0.7; animation: 1s fade linear forwards; }
.both { animation: fade 1s linear 0.5s both; }
.twice { animation: fade 1s linear 2; }
.frac { animation: fade 1s linear 1.5 forwards; }
.loop { animation: fade 1s linear infinite; }
.rev { animation: fade 1s linear reverse; }
.alt { animation: fade 1s linear 3 alternate; }
.altrev { animation: fade 1s linear 2 alternate-reverse; }
.paused { animation-play-state: paused; }
.two { animation: fade 1s linear, grow 2s linear; }
.mixed { animation: mixed 4s linear; }
.hide { animation: hide 1s linear; }
.turn { animation: turn 1s linear forwards; }
.slow { animation-duration: 4s; }
.gone { display: none; }
)css";
} // namespace

int main() {
    // ---- parsing ----
    {
        std::vector<StyleDiagnostic> d;
        auto s = StyleSheet::parse("@keyframes k { from { opacity: 0 } 30%, 60% { opacity: 0.5 } to { opacity: 1 } bogus { opacity: 1 } } "
                                   ".a { animation: 250ms ease-out k 1s 3 alternate both paused, none; } "
                                   ".b { animation-name: x, y, z; animation-duration: 1s, 2s; animation-delay: 100ms; } "
                                   ".c { animation: k 1s 2s 3s; } .d { animation: k 1s wobble; } .e { animation-duration: -1s; }", &d);
        UI_CHECK(s.has_value());
        UI_CHECK_EQ(d.size(), std::size_t(4)); // the bogus keyframe selector, three bad animation values
        if (s) {
            UI_CHECK(s->keyframes().size() == 1 && s->keyframes()[0].name == animationNameId("k"));
            UI_CHECK_EQ(s->keyframes()[0].frames.size(), std::size_t(4));
            UI_CHECK(near(s->keyframes()[0].frames[1].offset, 0.3f) && near(s->keyframes()[0].frames[2].offset, 0.6f));
            ComputedStyle cs;
            for (const auto& decl : s->rules()[0].decls) applyProp(cs, decl.prop, decl.value);
            UI_CHECK_EQ(cs.animationCount, u8(2));
            const AnimationSpec& a = cs.animations[0];
            UI_CHECK(a.name == animationNameId("k") && near(a.duration, 0.25f) && near(a.delay, 1.f) && near(a.iterations, 3.f));
            UI_CHECK(a.ease == Easing::Out && a.direction == AnimDirection::Alternate && a.fill == AnimFill::Both && a.paused);
            UI_CHECK(cs.animations[1].name == 0u && !cs.animations[1].paused);
            ComputedStyle cb;
            for (const auto& decl : s->rules()[1].decls) applyProp(cb, decl.prop, decl.value);
            UI_CHECK_EQ(cb.animationCount, u8(3));
            UI_CHECK(near(cb.animations[0].duration, 1.f) && near(cb.animations[1].duration, 2.f) && near(cb.animations[2].duration, 1.f)); // cycles
            UI_CHECK(near(cb.animations[2].delay, 0.1f));
        }
        PropValue v;
        UI_CHECK(parsePropValue(Prop::AnimationIterationCount, "infinite, 2.5", v) && std::isinf(v.f[0]) && near(v.f[1], 2.5f));
        UI_CHECK(!parsePropValue(Prop::AnimationDirection, "sideways", v) && !parsePropValue(Prop::AnimationFillMode, "up", v));
        UI_CHECK(!parsePropValue(Prop::AnimationName, "a, b, c, d, e", v)); // more than kMaxAnimations
        KeyframesRule k;
        UI_CHECK(parseKeyframes("from { opacity: 0 } to { opacity: 1 }", k) && k.frames.size() == 2);
        UI_CHECK(!parseKeyframes("", k) && !parseKeyframes("from { opacity: 0 ", k));
    }

    UiContext ui({.viewport = {800, 600}});
    std::vector<StyleDiagnostic> diags;
    UI_CHECK(ui.addStyleSheet(kSheet, &diags).has_value());
    for (const auto& x : diags) std::fprintf(stderr, "  diagnostic line %u: %s\n", x.line, x.message.c_str());
    UI_CHECK(diags.empty());
    ui.update(0.f);
    auto op = [&](NodeHandle n) { return ui.computed(n).opacity; };
    auto wd = [&](NodeHandle n) { return ui.computed(n).width.value; };
    /// The node's opacity (or `what`) `t` seconds after a fresh node got `cls`.
    auto at = [&](const char* cls, f32 t, f32 (*what)(const ComputedStyle&) = nullptr) {
        const NodeHandle n = make(ui, cls);
        ui.update(0.f);
        if (t > 0.f) ui.update(t);
        const f32 v = what ? what(ui.computed(n)) : ui.computed(n).opacity;
        ui.destroy(n);
        ui.update(0.f);
        return v;
    };
    f32 (*const width)(const ComputedStyle&) = [](const ComputedStyle& c) { return c.width.value; };

    // ---- keyframe timing ----
    UI_CHECK(near(at("fade", 0.f), 0.f));        // the first frame already shows the first keyframe
    UI_CHECK(near(at("fade", 0.25f), 0.25f));
    UI_CHECK(near(at("fade", 0.75f), 0.75f));
    UI_CHECK(near(at("fade", 1.5f), 1.f));       // over: back to the underlying value (1)
    UI_CHECK(near(at("ease", 0.5f), 0.125f));    // ease-in is cubic: 0.5^3
    // per-keyframe easing: the first half eases in (the animation's function), the second half is linear
    UI_CHECK(near(at("grow", 0.5f, width), 100.f + 100.f * 0.125f, 0.05f));
    UI_CHECK(near(at("grow", 1.f, width), 200.f, 0.05f));
    UI_CHECK(near(at("grow", 1.5f, width), 300.f, 0.05f));
    // implicit first / last keyframes are the underlying value; var() resolves against the sheet's tokens
    UI_CHECK(near(at("pulse", 0.5f), 0.75f) && near(at("pulse", 1.f), 0.5f) && near(at("pulse", 1.5f), 0.75f));
    // a keyframe list per property: opacity has four keys, width only the ends
    UI_CHECK(near(at("mixed", 0.5f), 0.5f) && near(at("mixed", 2.f), 0.8f) && near(at("mixed", 3.5f), 0.6f));
    UI_CHECK(near(at("mixed", 2.f, width), 30.f, 0.05f));
    // properties that do not interpolate switch half-way
    {
        const NodeHandle n = make(ui, "hide");
        ui.update(0.f);
        UI_CHECK(ui.computed(n).textAlign == TextAlignment::Center);
        ui.update(0.4f);
        UI_CHECK(ui.computed(n).textAlign == TextAlignment::Center);
        ui.update(0.2f);
        UI_CHECK(ui.computed(n).textAlign == TextAlignment::End);
        ui.destroy(n);
    }
    // transforms interpolate (and hold with fill: forwards)
    {
        const NodeHandle n = make(ui, "turn");
        ui.update(0.f);
        ui.update(0.5f);
        const Transform2D t = ui.computed(n).transform;
        UI_CHECK(near(t.m[0], std::cos(0.785398f)) && near(t.m[1], std::sin(0.785398f)));
        ui.update(1.f);
        UI_CHECK(near(ui.computed(n).transform.m[0], 0.f) && near(ui.computed(n).transform.m[1], 1.f));
        ui.destroy(n);
    }
    // delay and fill modes
    UI_CHECK(near(at("delay", 0.25f), 1.f) && near(at("delay", 1.f), 0.5f)); // before the delay: no effect
    UI_CHECK(near(at("early", 0.f), 0.25f) && near(at("early", 0.5f), 0.75f)); // a negative delay starts part-way in
    UI_CHECK(near(at("back", 0.25f), 0.f) && near(at("back", 2.f), 0.7f));
    UI_CHECK(near(at("fwd", 0.5f), 0.5f) && near(at("fwd", 3.f), 1.f));
    UI_CHECK(near(at("both", 0.25f), 0.f) && near(at("both", 5.f), 1.f));
    // iteration count and direction
    UI_CHECK(near(at("twice", 0.25f), 0.25f) && near(at("twice", 1.25f), 0.25f) && near(at("twice", 2.5f), 1.f));
    UI_CHECK(near(at("frac", 1.25f), 0.25f) && near(at("frac", 4.f), 0.5f)); // ends half-way through the second run
    UI_CHECK(near(at("loop", 7.25f), 0.25f));
    UI_CHECK(near(at("rev", 0.25f), 0.75f));
    UI_CHECK(near(at("alt", 0.25f), 0.25f) && near(at("alt", 1.25f), 0.75f) && near(at("alt", 2.25f), 0.25f));
    UI_CHECK(near(at("altrev", 0.25f), 0.75f) && near(at("altrev", 1.25f), 0.25f));
    // two animations on one node run side by side
    {
        const NodeHandle n = make(ui, "two");
        ui.update(0.f);
        ui.update(0.5f);
        UI_CHECK(near(op(n), 0.5f) && near(wd(n), 150.f, 0.05f));
        AnimationHandle hs[4];
        UI_CHECK_EQ(ui.animations(n, hs), 2u);
        UI_CHECK(ui.animationState(hs[0]) == AnimPlayState::Running && near(ui.animationTime(hs[1]), 0.5f));
        ui.update(1.f);
        UI_CHECK(near(op(n), 1.f) && near(wd(n), 300.f, 0.05f));
        UI_CHECK(ui.animationState(hs[0]) == AnimPlayState::Finished && ui.animationState(hs[1]) == AnimPlayState::Running);
        ui.destroy(n);
    }

    // ---- play state, restart, retime ----
    {
        const NodeHandle n = make(ui, "loop");
        ui.update(0.f);
        ui.update(0.25f);
        UI_CHECK(near(op(n), 0.25f) && ui.needsUpdate() && ui.animating());
        ui.addClass(n, "paused");
        ui.update(0.f);
        ui.update(0.5f);
        UI_CHECK(near(op(n), 0.25f)); // holds
        UI_CHECK(!ui.needsUpdate() && !ui.animating()); // and asks for no frames
        UI_CHECK(ui.styleAnimating(n));
        ui.removeClass(n, "paused");
        ui.update(0.f);
        ui.update(0.25f);
        UI_CHECK(near(op(n), 0.5f)); // resumes where it was
        ui.addClass(n, "slow"); // a longer duration: the same animation, retimed (0.5 s into 4 s)
        ui.update(0.f);
        UI_CHECK(near(op(n), 0.125f));
        ui.removeClass(n, "slow");
        ui.removeClass(n, "loop"); // the name is gone: the animation ends
        ui.update(0.f);
        UI_CHECK(near(op(n), 1.f) && !ui.styleAnimating(n));
        AnimationHandle hs[2];
        UI_CHECK_EQ(ui.animations(n, hs), 0u);
        ui.addClass(n, "loop"); // and starts from the beginning when it comes back
        ui.update(0.f);
        UI_CHECK(near(op(n), 0.f));
        ui.update(0.5f);
        ui.addClass(n, "gone"); // display: none ends it; showing restarts it
        ui.update(0.f);
        UI_CHECK(!ui.computed(n).visible && ui.animations(n, hs) == 0u);
        ui.removeClass(n, "gone");
        ui.update(0.f);
        ui.update(0.25f);
        UI_CHECK(near(op(n), 0.25f));
        ui.destroy(n);
        ui.update(0.f);
    }

    // ---- the end: signal, idle, held values ----
    {
        struct Seen { int count = 0; f32 index = -1.f; };
        static Seen seen;
        seen = {};
        const NodeHandle n = make(ui, "fwd");
        ui.connect(n, SignalKind::AnimationFinished, [](UiContext&, NodeHandle, const SignalArgs& a) {
            ++seen.count;
            seen.index = a.value;
        });
        ui.update(0.f);
        AnimationHandle h[1];
        UI_CHECK_EQ(ui.animations(n, h), 1u);
        ui.update(0.5f);
        UI_CHECK_EQ(seen.count, 0);
        ui.update(0.6f);
        UI_CHECK_EQ(seen.count, 1);
        UI_CHECK(near(seen.index, f32(h[0].index)));
        UI_CHECK(near(op(n), 1.f) && ui.styleAnimating(n)); // fill: forwards holds the last keyframe over opacity: 0.7
        ui.update(0.f);
        UI_CHECK(!ui.needsUpdate() && !ui.animating());
        for (int k = 0; k < 5; ++k) ui.update(0.1f);
        UI_CHECK_EQ(seen.count, 1);
        // the held value follows the cascade underneath it: nothing else changes
        ui.setCss(n, "height", "33px");
        ui.update(0.f);
        UI_CHECK(near(op(n), 1.f) && near(ui.computed(n).height.value, 33.f));
        ui.destroy(n);
        ui.update(0.f);
    }

    // ---- a later sheet replaces the @keyframes rule of a running animation ----
    {
        const NodeHandle n = make(ui, "loop");
        ui.update(0.f);
        ui.update(0.5f);
        UI_CHECK(near(op(n), 0.5f));
        UI_CHECK(ui.addStyleSheet("@keyframes fade { from { opacity: 1; } to { opacity: 0.5; } }").has_value());
        ui.update(0.f);
        UI_CHECK(near(op(n), 0.75f)); // the same position on the new keyframes
        ui.destroy(n);
        ui.update(0.f);
    }

    // ---- steady state: no style-table growth, no allocation per animated frame ----
    {
        std::vector<NodeHandle> nodes;
        for (int k = 0; k < 24; ++k) nodes.push_back(make(ui, k % 3 == 0 ? "loop" : k % 3 == 1 ? "alt" : "two"));
        ui.update(0.f);
        for (int k = 0; k < 10; ++k) ui.update(0.016f);
        const u32 table = ui.styles().tableSize();
        g_allocs = 0;
        g_count = true;
        for (int k = 0; k < 120; ++k) ui.update(0.016f);
        g_count = false;
        std::printf("24 animated nodes, 120 frames: %ld allocations, style table %u -> %u\n", g_allocs, table, ui.styles().tableSize());
        UI_CHECK_NO_ALLOC(g_allocs);
        UI_CHECK_EQ(ui.styles().tableSize(), table);
        // destroyed mid-flight
        for (NodeHandle n : nodes) ui.destroy(n);
        ui.update(0.016f);
        ui.update(0.016f);
        UI_CHECK(!ui.animating());
    }

    // ---- reduced motion: no frames in between, the end state at once ----
    {
        ui.setReducedMotion(true);
        const NodeHandle f = make(ui, "fwd"), l = make(ui, "loop");
        ui.update(0.f);
        ui.update(0.016f);
        UI_CHECK(near(op(f), 0.5f)); // the last keyframe of `fade` as the sheet added above defines it
        UI_CHECK(!ui.animating());
        ui.update(0.5f);
        UI_CHECK(near(op(l), 1.f)); // an endless animation stays on its first frame
        ui.setReducedMotion(false);
        ui.destroy(f);
        ui.destroy(l);
    }
    return finish("fuse_ui_css_animations");
}
