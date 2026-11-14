// Gate fuse_ui_style_motion: style transitions and property animation (plan §4.6 / §4.10).
// `transition` parsing (lists, box shorthands, none, inline); a hover colour fades through intermediate values in the
// context's time, ends exactly on the target and leaves the context idle (needsUpdate false, nextWakeSeconds +inf);
// retargeting mid-way starts from the on-screen value; the style table does not grow and nothing is allocated while a
// transition runs; layout properties transition through layout; animateProperty tween / spring, animateKeyframes;
// enter / exit animations of popups (fade, slide; an exiting popup is painted but not hit, then destroyed); reduced
// motion turns all of it into jumps; the default theme declares no motion (its rendering is static).
#include "test_alloc_hooks.hpp"
#include "test_util.hpp"

#include <atomic>
#include <cmath>
#include <limits>
#include <new>

namespace {
std::atomic<long> g_allocs{0};
bool g_counting = false;
} // namespace

// Every global allocation form is replaced and counted (test_alloc_hooks.hpp).
void fuse::ui::test::countAlloc(std::size_t) noexcept {
    if (g_counting) g_allocs.fetch_add(1, std::memory_order_relaxed);
}

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
bool near(f32 a, f32 b, f32 eps = 0.005f) { return std::abs(a - b) <= eps; }
bool grey(Color c, f32 v, f32 eps = 0.005f) { return near(c.r, v, eps) && near(c.g, v, eps) && near(c.b, v, eps); }
Vec2 centre(const Rect& r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
/// Alpha of the background rect the node `n` painted (-1: it painted none).
f32 paintedAlpha(const UiContext& ui, NodeHandle n) {
    for (const DrawCmd& c : ui.drawList().cmds())
        if (c.node == n.index && c.op == DrawOp::Rect && c.fill.a > 0.f) return c.fill.a;
    return -1.f;
}
NodeHandle firstOfClass(UiContext& ui, std::string_view cls) {
    const u32 id = ui.registry().find(cls);
    for (u32 i = 0; i < 4096; ++i)
        if (const NodeHandle n = ui.handleOf(i); ui.alive(n) && ui.classOf(n) == id) return n;
    return {};
}

void transitionGate() {
    UiContext ui({.viewport = {400, 300}, .font = &font()});
    std::vector<StyleDiagnostic> diags;
    UI_CHECK(ui.addStyleSheet(R"(
        .box { width: 100px; height: 40px; background: #000000; border-radius: 0px; flex-shrink: 0; }
        .box:hover { background: #ffffff; border-radius: 10px; }
        .fx { transition: background 0.2s linear, border-radius 0.1s linear; }
        .many { transition: all 150ms ease-out 50ms; }
        .off { transition: none; }
        .grow { width: 100px; transition: width 0.2s linear; }
        .grow:hover { width: 200px; }
        .bad { transition: display 1s; }
    )", &diags).has_value());
    UI_CHECK_EQ(diags.size(), 1u); // `display` is not animatable
    ui.setCss(ui.root(), "padding", "10px");
    ui.setCss(ui.root(), "gap", "10px");
    ui.setCss(ui.root(), "align-items", "start");
    const NodeHandle plain = *ui.create("panel", ui.root(), "plain");
    const NodeHandle fx = *ui.create("panel", ui.root(), "fx");
    const NodeHandle grow = *ui.create("panel", ui.root(), "grow");
    const NodeHandle after = *ui.create("panel", ui.root(), "after");
    ui.addClass(plain, "box");
    ui.addClass(fx, "box");
    ui.addClass(fx, "fx");
    ui.addClass(grow, "box");
    ui.addClass(grow, "grow");
    ui.addClass(after, "box");
    ui.update(0.f);
    const Color black = Color::rgb(0x000000), white = Color::rgb(0xFFFFFF);

    // parsing: a list, box shorthands expand, `all`, `none`
    {
        const ComputedStyle& s = ui.computed(fx);
        UI_CHECK_EQ(int(s.transitionCount), 5);
        const Transition* bg = findTransition(s, Prop::Background);
        UI_CHECK(bg && near(bg->duration, 0.2f) && bg->ease == Easing::Linear && bg->delay == 0.f);
        UI_CHECK(findTransition(s, Prop::RadiusBR) && near(findTransition(s, Prop::RadiusBR)->duration, 0.1f));
        UI_CHECK(!findTransition(s, Prop::Color));
        ui.addClass(after, "many");
        ui.update(0.f);
        const Transition* all = findTransition(ui.computed(after), Prop::Opacity);
        UI_CHECK(all && all->prop == kTransitionAll && near(all->duration, 0.15f) && near(all->delay, 0.05f) && all->ease == Easing::Out);
        ui.addClass(after, "off");
        ui.update(0.f);
        UI_CHECK_EQ(int(ui.computed(after).transitionCount), 0);
        ui.removeClass(after, "off");
        ui.removeClass(after, "many");
        UI_CHECK(ui.setCss(after, "transition", "opacity 0.3s, color 0.1s")); // inline: one entry per property
        ui.update(0.f);
        UI_CHECK_EQ(int(ui.computed(after).transitionCount), 2);
        ui.clearInline(after);
        ui.update(0.f);
    }
    // the first style of a node never transitions
    UI_CHECK(ui.computed(fx).background == black && !ui.styleAnimating(fx));
    UI_CHECK(!ui.needsUpdate() && std::isinf(ui.nextWakeSeconds()));

    // without a transition the hover colour switches at once
    ui.pointerMove(centre(ui.rect(plain)));
    ui.update(0.f);
    UI_CHECK(ui.computed(plain).background == white);
    UI_CHECK(!ui.needsUpdate());

    // with one it fades in the context's time: black -> grey -> white, then idle
    ui.pointerMove(centre(ui.rect(fx)));
    ui.update(0.f);
    UI_CHECK(ui.computed(fx).background == black && ui.styleAnimating(fx) && ui.animating());
    UI_CHECK(ui.needsUpdate() && ui.nextWakeSeconds() == 0.f);
    ui.update(0.05f);
    UI_CHECK(grey(ui.computed(fx).background, 0.25f));
    UI_CHECK(near(ui.computed(fx).radius[0], 5.f, 0.01f)); // its own duration
    UI_CHECK(ui.stats().repainted);
    ui.update(0.05f);
    UI_CHECK(grey(ui.computed(fx).background, 0.5f));
    UI_CHECK(near(ui.computed(fx).radius[0], 10.f, 0.01f) && near(ui.computed(fx).radius[2], 10.f, 0.01f));
    UI_CHECK(ui.styleAnimating(fx));
    ui.update(0.1f);
    UI_CHECK(ui.computed(fx).background == white && !ui.styleAnimating(fx));
    ui.update(0.f);
    UI_CHECK(!ui.needsUpdate() && std::isinf(ui.nextWakeSeconds()) && !ui.animating());
    ui.update(1.f);
    UI_CHECK(!ui.stats().repainted); // idle: no work
    // update(0) does not advance it
    ui.pointerMove({390.f, 290.f});
    ui.update(0.f);
    ui.update(0.f);
    UI_CHECK(ui.computed(fx).background == white && ui.styleAnimating(fx));
    // retarget mid-way: back from where it is on screen
    ui.update(0.1f);
    UI_CHECK(grey(ui.computed(fx).background, 0.5f));
    ui.pointerMove(centre(ui.rect(fx)));
    ui.update(0.f);
    UI_CHECK(grey(ui.computed(fx).background, 0.5f));
    ui.update(0.1f);
    UI_CHECK(grey(ui.computed(fx).background, 0.75f));
    ui.update(0.1f);
    UI_CHECK(ui.computed(fx).background == white && !ui.styleAnimating(fx));

    // steady state: hover cycles neither grow the style table nor allocate
    auto cycle = [&] {
        ui.pointerMove({390.f, 290.f});
        for (int i = 0; i < 16; ++i) ui.update(1.f / 60.f);
        ui.pointerMove(centre(ui.rect(fx)));
        for (int i = 0; i < 16; ++i) ui.update(1.f / 60.f);
    };
    cycle();
    const u32 table = ui.styles().tableSize();
    g_allocs = 0;
    g_counting = true;
    for (int i = 0; i < 5; ++i) cycle();
    g_counting = false;
    UI_CHECK_NO_ALLOC(g_allocs.load());
    UI_CHECK_EQ(ui.styles().tableSize(), table);
    UI_CHECK(ui.computed(fx).background == white);

    // a layout property transitions through layout: the box and what follows it move every frame
    ui.setCss(ui.root(), "flex-direction", "row");
    ui.update(0.f);
    const f32 afterX = ui.rect(after).x;
    ui.pointerMove(centre(ui.rect(grow)));
    ui.update(0.f);
    ui.update(0.1f);
    UI_CHECK(near(ui.rect(grow).w, 150.f, 0.6f));
    UI_CHECK(near(ui.rect(after).x, afterX + 50.f, 0.6f));
    ui.update(0.1f);
    UI_CHECK_EQ(ui.rect(grow).w, 200.f);
    UI_CHECK_EQ(ui.rect(after).x, afterX + 100.f);
    ui.update(0.f);
    UI_CHECK(!ui.needsUpdate());

    // a stylesheet swap is not a state change: no fade
    ui.pointerMove({390.f, 290.f});
    for (int i = 0; i < 30; ++i) ui.update(1.f / 60.f);
    UI_CHECK(ui.computed(fx).background == black);
    UI_CHECK(ui.addStyleSheet(".fx { background: #808080; }").has_value());
    ui.update(0.f);
    UI_CHECK(near(ui.computed(fx).background.r, 128.f / 255.f) && !ui.styleAnimating(fx));

    // reduced motion: transitions become jumps (also one already running)
    ui.pointerMove(centre(ui.rect(fx)));
    ui.update(0.f);
    ui.update(0.05f);
    UI_CHECK(ui.styleAnimating(fx));
    ui.setReducedMotion(true);
    ui.update(0.001f);
    UI_CHECK(ui.computed(fx).background == white && !ui.styleAnimating(fx));
    ui.pointerMove({390.f, 290.f});
    ui.update(0.f);
    UI_CHECK(near(ui.computed(fx).background.r, 128.f / 255.f) && !ui.styleAnimating(fx));
    ui.update(0.f);
    UI_CHECK(!ui.needsUpdate());
    // destroying a node in mid transition releases its slot
    ui.setReducedMotion(false);
    ui.pointerMove(centre(ui.rect(fx)));
    ui.update(0.f);
    ui.update(0.05f);
    UI_CHECK(ui.styleAnimating(fx));
    ui.destroy(fx);
    ui.update(0.016f);
    ui.update(0.f);
    UI_CHECK(!ui.needsUpdate() && !ui.animating());
}

void propertyGate() {
    UiContext ui({.viewport = {500, 300}, .font = &font()});
    ui.setCss(ui.root(), "align-items", "start");
    const NodeHandle box = *ui.create("panel", ui.root(), "box");
    ui.setCss(box, "width", "100px");
    ui.setCss(box, "height", "40px");
    ui.setCss(box, "background", "#ff0000");
    ui.update(0.f);

    // tween: from the current value to `to`; the end value stays as the inline value
    ui.animateProperty(box, Prop::Width, PropValue::ofLength(Length::px(200.f)), AnimSpec::tween(0.2f, Easing::Linear));
    UI_CHECK(ui.styleAnimating(box) && ui.needsUpdate());
    ui.update(0.1f);
    UI_CHECK(near(ui.computed(box).width.value, 150.f, 0.01f) && near(ui.rect(box).w, 150.f, 0.6f));
    ui.update(0.1f);
    UI_CHECK_EQ(ui.rect(box).w, 200.f);
    UI_CHECK(!ui.styleAnimating(box));
    ui.update(0.f);
    UI_CHECK(!ui.needsUpdate() && std::isinf(ui.nextWakeSeconds()));
    ui.markAllDirty(); // a restyle keeps it: it is an inline value now
    ui.update(0.f);
    UI_CHECK_EQ(ui.rect(box).w, 200.f);
    // colours
    ui.animateProperty(box, Prop::Background, PropValue::ofColor(Color::rgb(0x0000FF)), AnimSpec::tween(0.1f, Easing::Linear));
    ui.update(0.05f);
    UI_CHECK(near(ui.computed(box).background.r, 0.5f) && near(ui.computed(box).background.b, 0.5f));
    ui.update(0.05f);
    UI_CHECK(ui.computed(box).background == Color::rgb(0x0000FF));
    // delay
    AnimSpec late = AnimSpec::tween(0.1f, Easing::Linear);
    late.delay = 0.1f;
    ui.animateProperty(box, Prop::Opacity, PropValue::ofNumber(0.f), late);
    ui.update(0.1f);
    UI_CHECK_EQ(ui.computed(box).opacity, 1.f);
    ui.update(0.05f);
    UI_CHECK(near(ui.computed(box).opacity, 0.5f));
    ui.update(0.06f);
    UI_CHECK_EQ(ui.computed(box).opacity, 0.f);
    ui.setCss(box, "opacity", "1");
    ui.update(0.f);

    // spring: under-damped overshoots the target, then settles exactly on it and the context goes idle
    ui.animateProperty(box, Prop::Width, PropValue::ofLength(Length::px(300.f)), AnimSpec::springy(200.f, 8.f));
    f32 peak = 0.f;
    int frames = 0;
    while (ui.styleAnimating(box) && frames < 2000) {
        ui.update(1.f / 60.f);
        peak = std::max(peak, ui.computed(box).width.value);
        ++frames;
    }
    UI_CHECK(frames > 20 && frames < 2000);
    UI_CHECK(peak > 320.f); // overshoot
    UI_CHECK_EQ(ui.rect(box).w, 300.f);
    ui.update(0.f);
    UI_CHECK(!ui.needsUpdate());
    // a critically damped spring does not overshoot; the result does not depend on the frame rate
    ui.animateProperty(box, Prop::Width, PropValue::ofLength(Length::px(100.f)), AnimSpec::springy(170.f, 26.f));
    f32 low = 1e9f;
    f32 at30 = 0.f;
    for (int i = 0; i < 600 && ui.styleAnimating(box); ++i) {
        ui.update(1.f / 30.f);
        low = std::min(low, ui.computed(box).width.value);
        if (i == 5) at30 = ui.computed(box).width.value; // t = 0.2 s
    }
    UI_CHECK(low >= 99.5f);
    UI_CHECK_EQ(ui.rect(box).w, 100.f);
    ui.animateProperty(box, Prop::Width, PropValue::ofLength(Length::px(300.f)), AnimSpec::tween(0.f)); // zero duration: a jump
    ui.update(0.f);
    UI_CHECK_EQ(ui.rect(box).w, 300.f);
    ui.animateProperty(box, Prop::Width, PropValue::ofLength(Length::px(100.f)), AnimSpec::springy(170.f, 26.f));
    for (int i = 0; i < 24; ++i) ui.update(1.f / 120.f); // t = 0.2 s at 120 Hz
    UI_CHECK(near(ui.computed(box).width.value, at30, 1.f));
    ui.stopPropertyAnimations(box); // jumps to the end value
    ui.update(0.f);
    UI_CHECK_EQ(ui.rect(box).w, 100.f);
    UI_CHECK(!ui.styleAnimating(box));

    // keyframes: opacity 1 -> 0.2 -> 0.8 over a second
    const PropKeyframe keys[] = {{0.f, PropValue::ofNumber(1.f)}, {0.5f, PropValue::ofNumber(0.2f)}, {1.f, PropValue::ofNumber(0.8f)}};
    ui.animateKeyframes(box, Prop::Opacity, keys, 3, 1.f);
    ui.update(0.25f);
    UI_CHECK(near(ui.computed(box).opacity, 0.6f));
    ui.update(0.25f);
    UI_CHECK(near(ui.computed(box).opacity, 0.2f));
    ui.update(0.25f);
    UI_CHECK(near(ui.computed(box).opacity, 0.5f));
    ui.update(0.25f);
    UI_CHECK(near(ui.computed(box).opacity, 0.8f) && !ui.styleAnimating(box));
    // looping keyframes run until stopped
    ui.animateKeyframes(box, Prop::Opacity, keys, 3, 1.f, Easing::Linear, true);
    for (int i = 0; i < 10; ++i) ui.update(0.25f);
    UI_CHECK(ui.styleAnimating(box) && near(ui.computed(box).opacity, 0.2f)); // t = 2.5 s
    ui.stopPropertyAnimations(box);
    ui.update(0.f);
    ui.update(0.f);
    UI_CHECK(!ui.styleAnimating(box) && !ui.needsUpdate());

    // reduced motion: animateProperty lands at once; animate() too
    ui.setReducedMotion(true);
    ui.animateProperty(box, Prop::Width, PropValue::ofLength(Length::px(250.f)), AnimSpec::springy());
    ui.update(0.f);
    UI_CHECK_EQ(ui.rect(box).w, 250.f);
    UI_CHECK(!ui.styleAnimating(box));
    ui.setRange(box, 0.f, 1.f);
    ui.animate(box, UiContext::AnimProp::Value, 1.f, 5.f);
    ui.update(0.001f);
    UI_CHECK_EQ(ui.value(box), 1.f);
    UI_CHECK(!ui.animating());
}

void popupGate() {
    UiContext ui({.viewport = {500, 400}, .font = &font()});
    w::install(ui);
    UI_CHECK(ui.addStyleSheet("popup { enter-animation: slide-down; enter-duration: 0.2s; exit-animation: fade; exit-duration: 0.2s; }").has_value());
    const NodeHandle owner = w::button(ui, ui.root(), "Menu", "owner");
    ui.update(0.f);

    const NodeHandle scope = w::popupMenu(ui, owner, "One\nTwo\nThree", {40.f, 60.f});
    ui.update(0.f);
    const NodeHandle menu = ui.firstChild(scope);
    UI_CHECK(ui.alive(menu) && ui.animating());
    UI_CHECK_EQ(ui.rect(scope).y, -8.f); // slide-down: arrives from above
    UI_CHECK(paintedAlpha(ui, menu) < 0.f);  // fully transparent on the first frame
    ui.update(0.1f);
    const f32 mid = paintedAlpha(ui, menu);
    UI_CHECK(mid > 0.3f && mid < 0.97f);
    UI_CHECK(ui.rect(scope).y < 0.f && ui.rect(scope).y > -8.f);
    ui.update(0.11f);
    const f32 full = paintedAlpha(ui, menu);
    UI_CHECK(full > mid);
    UI_CHECK_EQ(ui.rect(scope).y, 0.f);
    ui.update(0.f);
    UI_CHECK(!ui.needsUpdate());
    const NodeHandle item = firstOfClass(ui, "menuitem");
    const Vec2 pi = centre(ui.rect(item));
    UI_CHECK(ui.hitTest(pi) == item);

    // exit: the closed popup keeps painting while it fades, is not hit, then is destroyed
    w::closePopup(ui, scope);
    ui.update(0.f);
    UI_CHECK(ui.alive(scope) && ui.styleAnimating(scope));
    UI_CHECK(!ui.modal().valid());
    UI_CHECK(near(paintedAlpha(ui, menu), full, 0.01f));
    UI_CHECK(ui.hitTest(pi) != item);
    ui.update(0.1f);
    const f32 fading = paintedAlpha(ui, menu);
    UI_CHECK(fading > 0.f && fading < full);
    ui.update(0.11f);
    UI_CHECK(paintedAlpha(ui, menu) < 0.f); // hidden
    ui.update(0.02f);
    UI_CHECK(!ui.alive(scope)); // destroyed after the exit time
    ui.update(0.f);
    UI_CHECK(!ui.needsUpdate() && std::isinf(ui.nextWakeSeconds()));

    // a dialog shown again before its exit finished comes back
    const NodeHandle dlg = w::dialog(ui, "Title", "dlg");
    w::button(ui, w::dialogBody(ui, dlg), "OK");
    const NodeHandle dscope = ui.parent(dlg);
    w::openDialog(ui, dlg);
    for (int i = 0; i < 30; ++i) ui.update(1.f / 60.f);
    UI_CHECK(ui.computed(dscope).visible && !ui.animating());
    w::closeDialog(ui, dlg);
    ui.update(0.f);
    ui.update(0.1f);
    UI_CHECK(ui.computed(dscope).visible && ui.styleAnimating(dscope)); // still fading out
    w::openDialog(ui, dlg);
    for (int i = 0; i < 30; ++i) ui.update(1.f / 60.f);
    UI_CHECK(ui.computed(dscope).visible && !ui.styleAnimating(dscope) && !ui.animating());
    UI_CHECK(ui.hitTest(centre(ui.rect(dlg))).valid() && ui.modal() == dscope);
    w::closeDialog(ui, dlg);
    for (int i = 0; i < 30; ++i) ui.update(1.f / 60.f);
    UI_CHECK(!ui.computed(dscope).visible && !ui.styleAnimating(dscope));
    ui.update(0.f);
    UI_CHECK(!ui.needsUpdate());

    // reduced motion: no enter, no exit - the popup is there at once and gone at the next update
    ui.setReducedMotion(true);
    const NodeHandle s2 = w::popupMenu(ui, owner, "One\nTwo", {40.f, 60.f});
    ui.update(0.f);
    UI_CHECK(!ui.animating() && ui.rect(s2).y == 0.f && paintedAlpha(ui, ui.firstChild(s2)) > 0.9f);
    w::closePopup(ui, s2);
    ui.update(0.f);
    ui.update(0.f);
    UI_CHECK(!ui.alive(s2));
}

void defaultThemeGate() {
    // the default theme declares no motion: every node of the sample tree and of an open menu is static
    UiContext ui({.viewport = {640, 520}, .font = &font()});
    w::install(ui);
    buildSample(ui);
    const NodeHandle scope = w::popupMenu(ui, ui.find("apply"), "One\nTwo", {40.f, 60.f});
    ui.update(0.f);
    u32 nodes = 0;
    for (u32 i = 0; i < 4096; ++i) {
        const NodeHandle n = ui.handleOf(i);
        if (!ui.alive(n)) continue;
        ++nodes;
        const ComputedStyle& s = ui.computed(n);
        UI_CHECK(s.transitionCount == 0 && s.enterAnim == MotionKind::None && s.exitAnim == MotionKind::None && !s.rtl);
    }
    UI_CHECK(nodes > 30);
    UI_CHECK(!ui.animating());
    UI_CHECK_EQ(ui.exitDuration(scope), 0.f);
    UiTestDriver drv(ui);
    drv.hover("cancel");
    UI_CHECK(!ui.animating() && !ui.needsUpdate());
    // the opt-in classes of the theme
    ui.addClass(ui.find("cancel"), "fx");
    ui.addClass(scope, "fx-fade");
    ui.update(0.f);
    UI_CHECK(ui.computed(ui.find("cancel")).transitionCount > 0);
    UI_CHECK(ui.exitDuration(scope) > 0.f);
}
} // namespace

int main() {
    transitionGate();
    propertyGate();
    popupGate();
    defaultThemeGate();
    return finish("fuse_ui_style_motion");
}
