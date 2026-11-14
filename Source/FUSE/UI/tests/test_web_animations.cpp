// Gate fuse_ui_web_animations: UiContext::animate (Element.animate-style scripted animations).
//   * keyframe offsets (explicit, spread evenly when left out, per property), per-keyframe easing, a single keyframe
//     (to-animation from the underlying value), animateCss() from keyframe text
//   * timing: delay, endDelay, iterations, direction, fill, the iteration easing
//   * control: pause / play / reverse / finish / cancel, currentTime, playbackRate (negative too), play after finish
//   * AnimationFinished, handle lifetime (cancelled, finished without a fill, node destroyed), animations(node),
//     commitAnimationStyles
//   * stacking: scripted animations apply over CSS animations and transitions, later ones over earlier ones
// (Adapted from the gui-framework gate of the same name: iterationStart, composite modes, timelines and the
// callback form are not ported.)
#include "test_util.hpp"

#include <cmath>
#include <limits>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
bool near(f32 a, f32 b, f32 eps = 2e-3f) { return std::fabs(a - b) <= eps; }
Keyframe opacityKey(f32 offset, f32 value, Easing e = Easing::Linear) { return {offset, Prop::Opacity, PropValue::ofNumber(value), e}; }
Keyframe widthKey(f32 offset, f32 px) { return {offset, Prop::Width, PropValue::ofLength(Length::px(px)), Easing::Linear}; }
AnimationTiming timing(f32 duration, AnimFill fill = AnimFill::None) {
    AnimationTiming t;
    t.duration = duration;
    t.fill = fill;
    return t;
}
} // namespace

int main() {
    UiContext ui({.viewport = {800, 600}});
    UI_CHECK(ui.addStyleSheet("panel { width: 100px; height: 20px; flex-shrink: 0; opacity: 1; } "
                              "@keyframes css { from { opacity: 0.2; } to { opacity: 0.2; } } .css { animation: css 10s linear; } "
                              ".tr { transition: opacity 1s linear; } .dim { opacity: 0.5; }").has_value());
    const u32 panel = ui.registry().find("panel");
    auto make = [&] { return ui.create(panel, ui.root()); };
    auto op = [&](NodeHandle n) { return ui.computed(n).opacity; };
    auto wd = [&](NodeHandle n) { return ui.computed(n).width.value; };
    ui.update(0.f);

    // ---- keyframes ----
    {
        const NodeHandle n = make();
        ui.update(0.f);
        const Keyframe keys[] = {opacityKey(0.f, 0.f), opacityKey(0.25f, 1.f), opacityKey(1.f, 0.5f)};
        const AnimationHandle a = ui.animate(n, keys, timing(4.f));
        UI_CHECK(a.valid() && ui.animationState(a) == AnimPlayState::Running);
        UI_CHECK(near(op(n), 0.f)); // applied at once, before the next update
        ui.update(0.5f);
        UI_CHECK(near(op(n), 0.5f));
        ui.update(0.5f);
        UI_CHECK(near(op(n), 1.f));
        ui.update(1.5f);
        UI_CHECK(near(op(n), 0.75f) && near(ui.animationTime(a), 2.5f));
        ui.update(2.f);
        UI_CHECK(near(op(n), 1.f)); // finished, no fill: the underlying value
        UI_CHECK(ui.animationState(a) == AnimPlayState::Idle && !ui.styleAnimating(n)); // and the handle is released
        // offsets left out are spread per property; easing per keyframe
        const Keyframe spread[] = {opacityKey(-1.f, 0.f, Easing::In), opacityKey(-1.f, 0.8f), opacityKey(-1.f, 0.4f), widthKey(-1.f, 200.f), widthKey(-1.f, 400.f)};
        const AnimationHandle b = ui.animate(n, spread, timing(2.f));
        ui.update(0.5f);
        UI_CHECK(near(op(n), 0.8f * 0.125f)); // first segment 0 .. 0.5, ease-in at its middle
        UI_CHECK(near(wd(n), 250.f, 0.05f));
        ui.update(1.f);
        UI_CHECK(near(op(n), 0.6f) && near(wd(n), 350.f, 0.05f));
        ui.cancelAnimation(b);
        UI_CHECK(near(op(n), 1.f) && near(wd(n), 100.f) && ui.animationState(b) == AnimPlayState::Idle);
        // a single keyframe animates from the underlying value
        const Keyframe to[] = {widthKey(-1.f, 300.f)};
        ui.animate(n, to, timing(1.f, AnimFill::Forwards));
        ui.update(0.5f);
        UI_CHECK(near(wd(n), 200.f, 0.05f));
        ui.update(1.f);
        UI_CHECK(near(wd(n), 300.f, 0.05f));
        // from keyframe text
        const NodeHandle m = make();
        ui.update(0.f);
        const AnimationHandle c = ui.animateCss(m, "from { opacity: 0; width: 10px } 50% { opacity: 1 } to { opacity: 0; width: 30px }", timing(2.f));
        UI_CHECK(c.valid());
        ui.update(0.5f);
        UI_CHECK(near(op(m), 0.5f) && near(wd(m), 15.f, 0.05f));
        UI_CHECK(!ui.animateCss(m, "nonsense", timing(1.f)).valid() && !ui.animate(NodeHandle{}, to, timing(1.f)).valid());
        UI_CHECK(!ui.animate(m, std::span<const Keyframe>{}, timing(1.f)).valid());
        ui.destroy(n);
        ui.destroy(m);
        ui.update(0.f);
    }

    // ---- timing ----
    {
        const Keyframe keys[] = {opacityKey(0.f, 0.f), opacityKey(1.f, 1.f)};
        auto at = [&](AnimationTiming t, f32 time) {
            const NodeHandle n = make();
            ui.update(0.f);
            ui.animate(n, keys, t);
            ui.update(time);
            const f32 v = op(n);
            ui.destroy(n);
            ui.update(0.f);
            return v;
        };
        AnimationTiming t = timing(1.f);
        t.delay = 0.5f;
        UI_CHECK(near(at(t, 0.25f), 1.f) && near(at(t, 1.f), 0.5f));
        t.fill = AnimFill::Backwards;
        UI_CHECK(near(at(t, 0.25f), 0.f));
        t = timing(1.f, AnimFill::Both);
        t.iterations = 2.5f;
        t.direction = AnimDirection::Alternate;
        UI_CHECK(near(at(t, 1.25f), 0.75f) && near(at(t, 2.25f), 0.25f) && near(at(t, 9.f), 0.5f));
        t = timing(1.f);
        t.easing = Easing::In;
        UI_CHECK(near(at(t, 0.5f), 0.125f));
        t = timing(1.f);
        t.direction = AnimDirection::Reverse;
        UI_CHECK(near(at(t, 0.25f), 0.75f));
        t = timing(1.f);
        t.iterations = std::numeric_limits<f32>::infinity();
        UI_CHECK(near(at(t, 100.25f), 0.25f, 0.01f));
        t = timing(0.f, AnimFill::Forwards); // no duration: the end state at once
        UI_CHECK(near(at(t, 0.016f), 1.f));
    }

    // ---- control ----
    {
        static int finished = 0;
        finished = 0;
        const NodeHandle n = make();
        ui.connect(n, SignalKind::AnimationFinished, [](UiContext&, NodeHandle, const SignalArgs& a) { finished += a.flag ? 1 : 100; });
        ui.update(0.f);
        const Keyframe keys[] = {opacityKey(0.f, 0.f), opacityKey(1.f, 1.f)};
        AnimationTiming t = timing(2.f, AnimFill::Both);
        t.endDelay = 1.f;
        const AnimationHandle a = ui.animate(n, keys, t);
        ui.update(0.5f);
        UI_CHECK(near(op(n), 0.25f));
        ui.pauseAnimation(a);
        ui.update(1.f);
        UI_CHECK(near(op(n), 0.25f) && ui.animationState(a) == AnimPlayState::Paused && !ui.animating());
        ui.setAnimationTime(a, 1.f); // seeking while paused shows the new position at once
        UI_CHECK(near(op(n), 0.5f));
        ui.playAnimation(a);
        ui.setPlaybackRate(a, 2.f);
        UI_CHECK(near(ui.playbackRate(a), 2.f));
        ui.update(0.25f);
        UI_CHECK(near(op(n), 0.75f));
        ui.reverseAnimation(a); // rate -2
        ui.update(0.5f);
        UI_CHECK(near(op(n), 0.25f) && ui.animationState(a) == AnimPlayState::Running);
        ui.update(0.5f); // reaches the start: finished at time 0
        UI_CHECK(near(op(n), 0.f) && ui.animationState(a) == AnimPlayState::Finished && near(ui.animationTime(a), 0.f));
        UI_CHECK_EQ(finished, 1);
        ui.reverseAnimation(a); // forwards again, from the start
        ui.setPlaybackRate(a, 1.f);
        ui.update(2.5f); // past the keyframes, inside the end delay: not finished yet
        UI_CHECK(near(op(n), 1.f) && ui.animationState(a) == AnimPlayState::Running);
        UI_CHECK_EQ(finished, 1);
        ui.update(1.f);
        UI_CHECK(ui.animationState(a) == AnimPlayState::Finished);
        UI_CHECK_EQ(finished, 2);
        ui.update(0.f);
        UI_CHECK(!ui.needsUpdate());
        ui.playAnimation(a); // play after the end restarts
        ui.update(1.f);
        UI_CHECK(near(op(n), 0.5f));
        ui.finishAnimation(a);
        UI_CHECK(near(op(n), 1.f));
        ui.update(0.f);
        UI_CHECK(ui.animationState(a) == AnimPlayState::Finished);
        UI_CHECK_EQ(finished, 3);
        // commitStyles + cancel: the value stays as an inline value
        ui.setAnimationTime(a, 0.5f);
        ui.pauseAnimation(a);
        ui.commitAnimationStyles(a);
        ui.cancelAnimation(a);
        ui.update(0.f);
        UI_CHECK(near(op(n), 0.25f) && !ui.styleAnimating(n) && ui.animationState(a) == AnimPlayState::Idle);
        UI_CHECK_EQ(finished, 3); // cancel does not report a finish
        // stale handles are inert
        ui.playAnimation(a);
        ui.setAnimationTime(a, 1.f);
        ui.finishAnimation(a);
        ui.commitAnimationStyles(a);
        UI_CHECK(near(ui.animationTime(a), 0.f) && near(ui.playbackRate(a), 0.f));
        // an endless animation cannot be finished; destroying the node drops its animations
        t = timing(1.f);
        t.iterations = std::numeric_limits<f32>::infinity();
        const AnimationHandle e = ui.animate(n, keys, t);
        ui.finishAnimation(e);
        ui.update(0.25f);
        UI_CHECK(ui.animationState(e) == AnimPlayState::Running && ui.animating());
        ui.destroy(n);
        UI_CHECK(ui.animationState(e) == AnimPlayState::Idle);
        ui.update(0.016f);
        UI_CHECK(!ui.animating());
    }

    // ---- stacking ----
    {
        const NodeHandle n = make();
        ui.addClass(n, "css");
        ui.update(0.f);
        UI_CHECK(near(op(n), 0.2f)); // the CSS animation
        const Keyframe half[] = {opacityKey(0.f, 0.6f), opacityKey(1.f, 0.6f)};
        const Keyframe full[] = {opacityKey(0.f, 0.9f), opacityKey(1.f, 0.9f)};
        const AnimationHandle a = ui.animate(n, half, timing(5.f));
        UI_CHECK(near(op(n), 0.6f)); // scripted over CSS
        const AnimationHandle b = ui.animate(n, full, timing(5.f));
        UI_CHECK(near(op(n), 0.9f)); // later over earlier
        AnimationHandle all[4];
        UI_CHECK_EQ(ui.animations(n, all), 3u);
        UI_CHECK(all[1] == a && all[2] == b);
        ui.cancelAnimation(b);
        UI_CHECK(near(op(n), 0.6f));
        ui.cancelAnimation(a);
        UI_CHECK(near(op(n), 0.2f));
        // over a transition: the animation wins while it runs, the transition's result shows afterwards
        const NodeHandle m = make();
        ui.addClass(m, "tr");
        ui.update(0.f);
        ui.addClass(m, "dim");
        ui.update(0.f);
        ui.update(0.5f);
        UI_CHECK(near(op(m), 0.75f));
        ui.animate(m, full, timing(0.25f));
        UI_CHECK(near(op(m), 0.9f));
        ui.update(0.3f);
        UI_CHECK(near(op(m), 0.6f)); // 0.8 s into the transition
        ui.update(0.3f);
        UI_CHECK(near(op(m), 0.5f) && !ui.styleAnimating(m));
        ui.destroy(n);
        ui.destroy(m);
    }
    return finish("fuse_ui_web_animations");
}
