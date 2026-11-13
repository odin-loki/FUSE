// Gate fuse_ui_web_animations: UiContext::animate (Element.animate-style scripted animations): keyframe offsets
// (explicit / computed), per-keyframe easing and composite, timing (delay, endDelay, iterations, iterationStart,
// direction, fill, easing), play / pause / reverse / finish / cancel / currentTime / playbackRate, finish and cancel
// signals and callbacks, commitStyles, getAnimations, stacking with CSS animations and transitions, node destroy.
#include "test_util.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }
AnimKeyframe kf(f32 off, Prop p, PropValue v) {
    AnimKeyframe k;
    k.offset = off;
    k.values.push_back({p, v});
    return k;
}
PropValue px(f32 v) { PropValue p; (void)parsePropValue(Prop::Width, std::to_string(v) + "px", p); return p; }
PropValue num(Prop prop, f32 v) { PropValue p; (void)parsePropValue(prop, std::to_string(v), p); return p; }
} // namespace

int main() {
    UiContext ui({.viewport = {800, 600}});
    const u32 panel = ui.registry().find("panel");
    UI_CHECK(ui.addStyleSheet(R"css(
@keyframes css { from { width: 0px; } to { width: 100px; } }
panel { width: 10px; height: 10px; opacity: 1; }
.css { animation: css 1s linear; }
.t { transition: width 1s linear; }
.t.big { width: 200px; }
)css").has_value());
    auto w = [&](NodeHandle n) { return ui.computed(n).width.value; };
    std::vector<std::string> log;
    auto listen = [&](NodeHandle n) {
        ui.connect(n, SignalKind::ScriptFinish, [&log](UiContext&, NodeHandle, const SignalArgs& a) { log.push_back("finish:" + std::string(a.text)); });
        ui.connect(n, SignalKind::ScriptCancel, [&log](UiContext&, NodeHandle, const SignalArgs& a) { log.push_back("cancel:" + std::string(a.text)); });
    };

    { // basic: two keyframes, linear, fill none; finish signal + callback; then back to the base value
        const NodeHandle n = ui.create(panel, ui.root());
        listen(n);
        int cb = 0;
        AnimationOptions o;
        o.duration = 1.f;
        o.id = "slide";
        o.onFinish = [&](UiContext& c, AnimationHandle a) { ++cb; UI_CHECK(c.animPlayState(a) == AnimPlayState::Finished); };
        const AnimationHandle a = ui.animate(n, {kf(-1, Prop::Width, px(0)), kf(-1, Prop::Width, px(100))}, o);
        UI_CHECK(a.valid() && ui.animValid(a) && ui.animTarget(a) == n && ui.animId(a) == "slide");
        UI_CHECK(ui.animPlayState(a) == AnimPlayState::Running);
        ui.update(0.f);
        UI_CHECK(near(w(n), 0.f));
        UI_CHECK(ui.needsUpdate());
        ui.update(0.25f);
        UI_CHECK(near(w(n), 25.f) && near(ui.animCurrentTime(a), 0.25f) && near(ui.animProgress(a), 0.25f));
        UI_CHECK_EQ(ui.animations(n).size(), std::size_t(1));
        UI_CHECK_EQ(ui.animations(ui.root(), true).size(), std::size_t(1));
        ui.update(1.f);
        UI_CHECK(near(w(n), 10.f) && ui.animPlayState(a) == AnimPlayState::Finished);
        UI_CHECK(log.size() == 1 && log[0] == "finish:slide" && cb == 1);
        UI_CHECK(ui.animations(n).empty()); // finished without fill
        log.clear();
        ui.destroy(n);
        UI_CHECK(!ui.animValid(a));
    }
    { // computed offsets, per-keyframe easing, fill forwards, delay, iterations + alternate
        const NodeHandle n = ui.create(panel, ui.root());
        AnimKeyframe a0 = kf(-1, Prop::Width, px(0)), a1 = kf(-1, Prop::Width, px(50)), a2 = kf(-1, Prop::Width, px(100));
        AnimationOptions o;
        o.duration = 2.f;
        o.fill = AnimFill::Forwards;
        const AnimationHandle a = ui.animate(n, {a0, a1, a2}, o); // offsets 0, 0.5, 1
        ui.update(0.f);
        ui.update(1.5f);
        UI_CHECK(near(w(n), 75.f));
        ui.update(5.f);
        UI_CHECK(near(w(n), 100.f) && ui.animPlayState(a) == AnimPlayState::Finished);
        UI_CHECK_EQ(ui.animations(n).size(), std::size_t(1)); // still filling
        ui.animCancel(a);
        ui.update(0.f);
        UI_CHECK(near(w(n), 10.f) && std::isnan(ui.animCurrentTime(a)));

        AnimationOptions o2;
        o2.duration = 1.f;
        o2.delay = 0.5f;
        o2.iterations = 2.f;
        o2.direction = AnimDirection::Alternate;
        o2.fill = AnimFill::Both;
        AnimKeyframe s0 = kf(0, Prop::Width, px(0));
        s0.hasEase = true;
        s0.ease = Easing{Easing::Kind::Steps, 0, 0, 0, 0, 2, Easing::StepPos::JumpEnd};
        const AnimationHandle b = ui.animate(n, {s0, kf(1, Prop::Width, px(100))}, o2);
        ui.update(0.f);
        UI_CHECK(near(w(n), 0.f)); // backwards fill during the delay
        ui.update(0.75f);           // t = 0.75, progress 0.25 -> step 0
        UI_CHECK(near(w(n), 0.f));
        ui.update(0.5f);            // t = 1.25, progress 0.75 -> step 0.5
        UI_CHECK(near(w(n), 50.f));
        ui.update(0.5f);            // t = 1.75: second iteration reversed, directed 0.75 -> 50
        UI_CHECK(near(w(n), 50.f));
        ui.update(5.f);
        UI_CHECK(near(w(n), 0.f) && ui.animPlayState(b) == AnimPlayState::Finished); // ends at the start (alternate)
        ui.destroy(n);
    }
    { // playback control
        const NodeHandle n = ui.create(panel, ui.root());
        listen(n);
        AnimationOptions o;
        o.duration = 1.f;
        o.fill = AnimFill::Both;
        o.id = "ctl";
        const AnimationHandle a = ui.animate(n, {kf(0, Prop::Width, px(0)), kf(1, Prop::Width, px(100))}, o);
        ui.update(0.f);
        ui.update(0.4f);
        UI_CHECK(ui.animPause(a) && ui.animPlayState(a) == AnimPlayState::Paused);
        ui.update(1.f);
        UI_CHECK(near(w(n), 40.f));
        UI_CHECK(ui.animSetCurrentTime(a, 0.8f));
        ui.update(0.f);
        UI_CHECK(near(w(n), 80.f));
        UI_CHECK(ui.animPlay(a));
        UI_CHECK(ui.animSetPlaybackRate(a, 0.5f) && near(ui.animPlaybackRate(a), 0.5f));
        ui.update(0.2f);
        UI_CHECK(near(w(n), 90.f));
        UI_CHECK(ui.animReverse(a) && near(ui.animPlaybackRate(a), -0.5f));
        ui.update(0.4f);
        UI_CHECK(near(w(n), 70.f));
        ui.update(10.f);
        UI_CHECK(near(w(n), 0.f) && ui.animPlayState(a) == AnimPlayState::Finished && near(ui.animCurrentTime(a), 0.f));
        UI_CHECK(log.size() == 1 && log[0] == "finish:ctl");
        UI_CHECK(ui.animPlay(a)); // reversed play from the start jumps to the end
        UI_CHECK(near(ui.animCurrentTime(a), 1.f));
        UI_CHECK(ui.animFinish(a));
        ui.update(0.f);
        UI_CHECK(near(w(n), 0.f) && log.size() == 2);
        UI_CHECK(ui.animCancel(a));
        ui.update(0.f);
        UI_CHECK(near(w(n), 10.f) && log.size() == 3 && log[2] == "cancel:ctl");
        // infinite: finish refused
        AnimationOptions inf;
        inf.duration = 1.f;
        inf.iterations = INFINITY;
        const AnimationHandle b = ui.animate(n, {kf(1, Prop::Width, px(100))}, inf); // implicit 0% from the base
        ui.update(0.f);
        ui.update(0.5f);
        UI_CHECK(near(w(n), 55.f));
        UI_CHECK(!ui.animFinish(b));
        ui.update(1.f);
        UI_CHECK(near(w(n), 55.f));
        // autoplay off: idle until played
        AnimationOptions idle;
        idle.duration = 1.f;
        idle.autoplay = false;
        ui.animCancel(b);
        const AnimationHandle c = ui.animate(n, {kf(0, Prop::Width, px(0)), kf(1, Prop::Width, px(100))}, idle);
        ui.update(0.5f);
        UI_CHECK(ui.animPlayState(c) == AnimPlayState::Idle && near(w(n), 10.f));
        ui.animPlay(c);
        ui.update(0.5f);
        UI_CHECK(near(w(n), 50.f));
        ui.destroy(n);
        UI_CHECK(!ui.animValid(c));
    }
    { // composite, stacking over CSS animations, transitions on top, commitStyles, CSS keyframes text
        const NodeHandle n = ui.create(panel, ui.root());
        ui.addClass(n, "css");
        AnimationOptions o;
        o.duration = 1.f;
        o.composite = AnimComposite::Add;
        const AnimationHandle a = ui.animate(n, {kf(0, Prop::Width, px(5)), kf(1, Prop::Width, px(5))}, o);
        ui.update(0.f);
        ui.update(0.5f);
        UI_CHECK(near(w(n), 55.f)); // css 50 + 5
        ui.animCancel(a);
        ui.update(0.f);
        UI_CHECK(near(w(n), 50.f));
        ui.destroy(n);

        const NodeHandle m = ui.create(panel, ui.root());
        AnimationOptions o2;
        o2.duration = 1.f;
        const AnimationHandle c = ui.animate(m, "from { width: 0px; opacity: 0 } 50% { width: 80px; } to { width: 100px; opacity: 1 }", o2);
        UI_CHECK(c.valid());
        ui.update(0.f);
        ui.update(0.25f);
        UI_CHECK(near(w(m), 40.f) && near(ui.computed(m).opacity, 0.25f));
        UI_CHECK(ui.animPause(c) && ui.animCommitStyles(c));
        ui.animCancel(c);
        ui.update(0.f);
        UI_CHECK(near(w(m), 40.f) && near(ui.computed(m).opacity, 0.25f)); // committed to the inline style
        ui.destroy(m);
    }
    { // invalid input
        const NodeHandle n = ui.create(panel, ui.root());
        AnimationOptions o;
        o.duration = 1.f;
        UI_CHECK(!ui.animate(n, std::vector<AnimKeyframe>{}, o).valid());
        UI_CHECK(!ui.animate(n, {kf(0.8f, Prop::Width, px(0)), kf(0.2f, Prop::Width, px(1))}, o).valid());
        UI_CHECK(!ui.animate(n, {kf(1.5f, Prop::Width, px(0))}, o).valid());
        o.duration = -1.f;
        UI_CHECK(!ui.animate(n, {kf(1, Prop::Width, px(0))}, o).valid());
        UI_CHECK(!ui.animate(NodeHandle{}, {kf(1, Prop::Width, px(0))}, AnimationOptions{}).valid());
        UI_CHECK(!ui.animPlay(AnimationHandle{}) && !ui.animValid(AnimationHandle{}));
        // many short animations on one node stay bounded (idle / finished ones are pruned)
        o.duration = 0.f;
        AnimationHandle first;
        for (int i = 0; i < 100; ++i) {
            const AnimationHandle h = ui.animate(n, {kf(1, Prop::Opacity, num(Prop::Opacity, 0.5f))}, o);
            if (i == 0) first = h;
            ui.update(0.01f);
        }
        UI_CHECK(!ui.animValid(first));
        ui.destroy(n);
        ui.update(0.f);
        UI_CHECK(!ui.needsUpdate());
    }
    return finish("fuse_ui_web_animations");
}
