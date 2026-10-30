// Gate fuse_ui_css_animations: CSS Animations 1 (@keyframes, the `animation` shorthand and every longhand: name,
// duration, timing-function, delay, iteration-count, direction, fill-mode, play-state; implicit 0% / 100% keyframes,
// per-keyframe easing, events), the transition-* longhands (list cycling), reversing shortening with
// transitionrun / start / end / cancel, attribute selectors, the :nth-child family and sibling combinators.
#include "test_util.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool parsesAs(Prop p, std::string_view text) { PropValue v; return parsePropValue(p, text, v); }
ComputedStyle styleOf(std::string_view decls) {
    std::vector<StyleDiagnostic> diags;
    auto sh = StyleSheet::parse("x { " + std::string(decls) + " }", &diags);
    UI_CHECK(sh.has_value() && diags.empty());
    ComputedStyle cs;
    if (sh && !sh->rules().empty())
        for (const Declaration& d : sh->rules()[0].decls) applyProp(cs, d.prop, d.value);
    return cs;
}
std::vector<std::string> g_log;
void listen(UiContext& ui, NodeHandle n) {
    static constexpr std::pair<SignalKind, const char*> kKinds[] = {
        {SignalKind::TransitionRun, "run"}, {SignalKind::TransitionStart, "start"}, {SignalKind::TransitionEnd, "end"},
        {SignalKind::TransitionCancel, "cancel"}, {SignalKind::AnimationStart, "astart"},
        {SignalKind::AnimationIteration, "aiter"}, {SignalKind::AnimationEnd, "aend"}, {SignalKind::AnimationCancel, "acancel"}};
    for (auto [k, name] : kKinds)
        ui.connect(n, k, [name](UiContext&, NodeHandle, const SignalArgs& a) {
            g_log.push_back(std::string(name) + ":" + std::string(a.text) + "@" + std::to_string(int(std::lround(a.value * 1000.f))));
        });
}
bool logged(std::string_view e) {
    for (const auto& s : g_log)
        if (s == e) return true;
    return false;
}
} // namespace

int main() {
    // ---- longhand parsing ----
    {
        PropValue v;
        UI_CHECK(parsePropValue(Prop::TransitionDuration, "1s, 200ms", v) && v.trackCount == 2 && near(v.tracks[1].value, 0.2f));
        UI_CHECK(!parsesAs(Prop::TransitionDuration, "-1s"));
        UI_CHECK(parsesAs(Prop::TransitionDelay, "-1s, 2s"));
        UI_CHECK(parsesAs(Prop::TransitionProperty, "none") && !parsesAs(Prop::TransitionProperty, "width, none"));
        UI_CHECK(!parsesAs(Prop::TransitionProperty, "width, nope"));
        UI_CHECK(parsesAs(Prop::TransitionTimingFunction, "ease, steps(2, end), cubic-bezier(0, 0, 1, 1)"));
        UI_CHECK(parsePropValue(Prop::AnimationIterationCount, "infinite, 2.5", v) && std::isinf(v.tracks[0].value) && v.tracks[1].value == 2.5f);
        UI_CHECK(!parsesAs(Prop::AnimationIterationCount, "-1"));
        UI_CHECK(parsePropValue(Prop::AnimationName, "none, spin", v) && v.ids[0] == 0 && v.ids[1] == animationNameId("spin"));
        UI_CHECK(!parsesAs(Prop::AnimationName, "initial") && !parsesAs(Prop::AnimationName, "3d"));
        UI_CHECK(parsePropValue(Prop::AnimationDirection, "alternate-reverse", v) && v.fns[0] == u8(AnimDirection::AlternateReverse));
        UI_CHECK(parsePropValue(Prop::AnimationFillMode, "none, both", v) && v.fns[1] == u8(AnimFill::Both));
        UI_CHECK(parsePropValue(Prop::AnimationPlayState, "running, paused", v) && v.fns[1] == 1);
        UI_CHECK(!parsesAs(Prop::AnimationPlayState, "stopped"));
        // transition-* lists cycle: property entries index durations / delays / easings modulo their lengths
        const ComputedStyle t = styleOf("transition-property: width, opacity, color; transition-duration: 1s, 2s; "
                                        "transition-delay: 0.5s; transition-timing-function: linear, ease-in;");
        TransitionSpec s;
        UI_CHECK(t.transitionFor(u8(Prop::Width), s) && s.duration == 1.f && s.delay == 0.5f && s.ease.x1 == 0.f);
        UI_CHECK(t.transitionFor(u8(Prop::Opacity), s) && s.duration == 2.f && s.delay == 0.5f && s.ease.x1 == 0.42f);
        UI_CHECK(t.transitionFor(u8(Prop::Color), s) && s.duration == 1.f && s.ease.x1 == 0.f);
        UI_CHECK(!t.transitionFor(u8(Prop::Height), s));
        // a later longhand overrides the shorthand's entry
        const ComputedStyle t2 = styleOf("transition: width 1s; transition-duration: 3s;");
        UI_CHECK(t2.transitionFor(u8(Prop::Width), s) && s.duration == 3.f);
        // animation shorthand: every component, any order
        const ComputedStyle a = styleOf("animation: slide 2s ease-in 1s 3 reverse forwards paused, 500ms infinite none;");
        UI_CHECK_EQ(int(a.animationNameCount), 2);
        UI_CHECK(a.animationName[0] == animationNameId("slide") && a.animationName[1] == 0);
        UI_CHECK(a.animationDuration[0] == 2.f && a.animationDelay[0] == 1.f && a.animationIterations[0] == 3.f);
        UI_CHECK(a.animationEase[0].x1 == 0.42f && a.animationDirection[0] == AnimDirection::Reverse);
        UI_CHECK(a.animationFill[0] == AnimFill::Forwards && a.animationPaused[0] == 1);
        UI_CHECK(near(a.animationDuration[1], 0.5f) && std::isinf(a.animationIterations[1]) && a.animationPaused[1] == 0);
        const ComputedStyle a2 = styleOf("animation: none none;"); // name none, fill none
        UI_CHECK(!a2.hasAnimations() && a2.animationFill[0] == AnimFill::None);
        std::vector<StyleDiagnostic> d;
        UI_CHECK(StyleSheet::parse("x { animation: a 1s 2s 3s; }", &d) && !d.empty()); // three times
    }
    // ---- @keyframes parsing ----
    {
        std::vector<StyleDiagnostic> diags;
        auto sh = StyleSheet::parse(R"css(
@keyframes k { to { width: 100px; display: none; } from { width: 0px; } 50%, 75% { width: 50px; animation-timing-function: linear; } }
p { width: 1px; }
@keyframes bad { 150% { width: 1px; } }
)css", &diags);
        UI_CHECK(sh.has_value());
        UI_CHECK_EQ(diags.size(), std::size_t(1)); // 150%
        UI_CHECK_EQ(sh->rules().size(), std::size_t(1));
        UI_CHECK_EQ(sh->keyframes().size(), std::size_t(2));
        const KeyframesRule& k = sh->keyframes()[0];
        UI_CHECK(k.name == animationNameId("k"));
        UI_CHECK_EQ(k.frames.size(), std::size_t(4));
        UI_CHECK(k.frames[0].offset == 0.f && k.frames[1].offset == 0.5f && k.frames[2].offset == 0.75f && k.frames[3].offset == 1.f);
        UI_CHECK(k.frames[1].hasEase && k.frames[1].ease.x1 == 0.f && !k.frames[0].hasEase);
        UI_CHECK_EQ(k.frames[3].decls.size(), std::size_t(1)); // display is not animatable: ignored
    }
    // ---- animations in the context ----
    UiContext ui({.viewport = {800, 600}});
    const u32 panel = ui.registry().find("panel"), label = ui.registry().find("label");
    std::vector<StyleDiagnostic> diags;
    UI_CHECK(ui.addStyleSheet(R"css(
@keyframes grow { from { width: 0px; } to { width: 100px; } }
@keyframes fade { 50% { opacity: 0; } }
@keyframes eased { from { width: 0px; animation-timing-function: steps(2); } to { width: 100px; } }
panel { width: 10px; height: 10px; opacity: 1; }
.a { animation: grow 1s linear; }
.b { animation: fade 2s linear 2; }
.c { animation: grow 1s linear 0.5s 2 alternate both; }
.d { animation: grow 1s linear infinite; }
.p { animation-play-state: paused; }
.f { animation: grow 1s linear 1s forwards; }
.e { animation: eased 1s linear; }
.two { animation: grow 1s linear, fade 1s linear; }
.t { transition: opacity 1s linear; }
.t:hover { opacity: 0; }
)css", &diags).has_value());
    UI_CHECK(diags.empty());
    auto w = [&](NodeHandle n) { return ui.computed(n).width.value; };

    { // .a: plays once, fill none: back to the base value; start / end events
        const NodeHandle n = ui.create(panel, ui.root());
        listen(ui, n);
        ui.addClass(n, "a");
        ui.update(0.f);
        UI_CHECK(near(w(n), 0.f)); // animations start on the first style
        UI_CHECK(logged("astart:grow@0"));
        UI_CHECK_EQ(ui.animationCount(), 1u);
        UI_CHECK(ui.needsUpdate());
        ui.update(0.5f);
        UI_CHECK(near(w(n), 50.f));
        UI_CHECK(near(ui.rect(n).w, 50.f)); // layout follows
        const u32 table = ui.styles().tableSize();
        for (int k = 0; k < 4; ++k) ui.update(0.1f);
        UI_CHECK_EQ(ui.styles().tableSize(), table); // the scratch slot is reused
        ui.update(0.2f);
        UI_CHECK(near(w(n), 10.f));
        UI_CHECK(logged("aend:grow@1000"));
        UI_CHECK_EQ(ui.animationCount(), 0u);
        ui.update(0.f);
        UI_CHECK(!ui.needsUpdate());
        ui.update(1.f);
        UI_CHECK(near(w(n), 10.f)); // does not restart
        // removing and re-adding the name restarts it
        ui.removeClass(n, "a");
        ui.update(0.f);
        ui.addClass(n, "a");
        ui.update(0.25f);
        UI_CHECK(near(w(n), 0.f));
        ui.update(0.25f);
        UI_CHECK(near(w(n), 25.f));
        g_log.clear();
        ui.removeClass(n, "a"); // cancelled mid-run
        ui.update(0.f);
        UI_CHECK(logged("acancel:grow@0"));
        UI_CHECK(near(w(n), 10.f));
        ui.destroy(n);
    }
    { // .b: implicit 0% / 100% keyframes from the base value; 2 iterations with an iteration event
        g_log.clear();
        const NodeHandle n = ui.create(panel, ui.root());
        listen(ui, n);
        ui.addClass(n, "b");
        ui.update(0.f);
        ui.update(0.5f);
        UI_CHECK(near(ui.computed(n).opacity, 0.5f));
        ui.update(0.5f);
        UI_CHECK(near(ui.computed(n).opacity, 0.f));
        ui.update(1.5f); // 2.5 s: second iteration, progress 0.25
        UI_CHECK(near(ui.computed(n).opacity, 0.5f));
        UI_CHECK(logged("aiter:fade@2000"));
        ui.update(2.f);
        UI_CHECK(near(ui.computed(n).opacity, 1.f));
        UI_CHECK(logged("aend:fade@4000"));
        ui.destroy(n);
    }
    { // .c: delay with backwards fill, alternate, forwards fill ends on the reversed iteration's end
        const NodeHandle n = ui.create(panel, ui.root());
        ui.addClass(n, "c");
        ui.update(0.f);
        UI_CHECK(near(w(n), 0.f)); // before phase: backwards fill shows 0%
        ui.update(0.75f);
        UI_CHECK(near(w(n), 25.f));
        ui.update(1.f); // e = 1.25: iteration 1 runs backwards
        UI_CHECK(near(w(n), 75.f));
        ui.update(5.f);
        UI_CHECK(near(w(n), 0.f)); // fills forwards with the end of iteration 1 (reversed: 0%)
        UI_CHECK(!ui.needsUpdate() || ui.animationCount() == 0u);
        ui.destroy(n);
    }
    { // .f: delay without backwards fill shows the base value; forwards keeps 100%
        const NodeHandle n = ui.create(panel, ui.root());
        ui.addClass(n, "f");
        ui.update(0.f);
        UI_CHECK(near(w(n), 10.f));
        ui.update(1.5f);
        UI_CHECK(near(w(n), 50.f));
        ui.update(1.f);
        UI_CHECK(near(w(n), 100.f));
        ui.destroy(n);
    }
    { // .d + .p: infinite, paused keeps the value and stops ticking; resuming continues
        const NodeHandle n = ui.create(panel, ui.root());
        ui.addClass(n, "d");
        ui.update(0.f);
        ui.update(2.3f);
        UI_CHECK(near(w(n), 30.f));
        ui.addClass(n, "p");
        ui.update(0.f);
        UI_CHECK_EQ(ui.animationCount(), 0u);
        UI_CHECK(!ui.needsUpdate());
        ui.update(1.f);
        UI_CHECK(near(w(n), 30.f));
        ui.removeClass(n, "p");
        ui.update(0.f);
        ui.update(0.2f);
        UI_CHECK(near(w(n), 50.f));
        UI_CHECK(ui.needsUpdate());
        ui.destroy(n);
        ui.update(0.f);
        UI_CHECK_EQ(ui.animationCount(), 0u);
    }
    { // per-keyframe easing: steps(2) on the from keyframe
        const NodeHandle n = ui.create(panel, ui.root());
        ui.addClass(n, "e");
        ui.update(0.f);
        ui.update(0.3f);
        UI_CHECK(near(w(n), 0.f));
        ui.update(0.4f);
        UI_CHECK(near(w(n), 50.f));
        ui.destroy(n);
    }
    { // two animations on different properties run together
        const NodeHandle n = ui.create(panel, ui.root());
        ui.addClass(n, "two");
        ui.update(0.f);
        ui.update(0.25f);
        UI_CHECK(near(w(n), 25.f) && near(ui.computed(n).opacity, 0.5f));
        ui.destroy(n);
    }
    { // children inherit nothing odd while a parent animates; a child's own animation runs meanwhile
        const NodeHandle p = ui.create(panel, ui.root());
        const NodeHandle c = ui.create(panel, p);
        ui.addClass(p, "d");
        ui.addClass(c, "a");
        ui.update(0.f);
        ui.update(0.5f);
        UI_CHECK(near(w(p), 50.f) && near(w(c), 50.f));
        ui.destroy(p);
    }
    { // reversing shortening and transition events
        g_log.clear();
        const NodeHandle n = ui.create(panel, ui.root());
        listen(ui, n);
        ui.addClass(n, "t");
        ui.update(0.f);
        ui.setState(n, kStateHover, true);
        ui.update(0.f);
        UI_CHECK(logged("run:opacity@0") && logged("start:opacity@0"));
        ui.update(0.25f);
        UI_CHECK(near(ui.computed(n).opacity, 0.75f));
        g_log.clear();
        ui.setState(n, kStateHover, false); // back to the start value after 25 %: runs 0.25 s
        ui.update(0.f);
        UI_CHECK(logged("cancel:opacity@250") && logged("run:opacity@0"));
        ui.update(0.125f);
        UI_CHECK(near(ui.computed(n).opacity, 0.875f));
        ui.update(0.2f);
        UI_CHECK(near(ui.computed(n).opacity, 1.f));
        UI_CHECK(logged("end:opacity@250"));
        UI_CHECK_EQ(ui.transitionCount(), 0u);
        // reverse twice: the factor compounds (0.25 * 0.5 + 1 - 0.25 = 0.875 of 1 s)
        ui.setState(n, kStateHover, true);
        ui.update(0.f);
        ui.update(0.5f); // 0.5 toward 0
        ui.setState(n, kStateHover, false);
        ui.update(0.f);
        ui.update(0.25f); // factor 0.5: 0.5 s; halfway
        UI_CHECK(near(ui.computed(n).opacity, 0.75f));
        ui.setState(n, kStateHover, true); // reverse again: factor |0.5 * 0.5 + 1 - 0.5| = 0.75
        ui.update(0.f);
        ui.update(0.375f);
        UI_CHECK(near(ui.computed(n).opacity, 0.375f));
        ui.destroy(n);
    }
    // ---- selectors ----
    {
        std::vector<StyleDiagnostic> d;
        auto sh = StyleSheet::parse("a[x].y:hover:nth-child(2) #z { width: 1px; } a[x=\"1,2\"], b:nth-child(2n+1) {} c + d ~ e > f {}", &d);
        UI_CHECK(sh && d.empty());
        UI_CHECK_EQ(sh->rules().size(), std::size_t(4));
        UI_CHECK_EQ(sh->rules()[0].specificity, (1u << 16) | (4u << 8) | 1u);
        UI_CHECK_EQ(sh->rules()[1].chain[0].attrs[0].value, std::string("1,2"));
        UI_CHECK(sh->rules()[3].chain[1].combinator == SimpleSelector::Combinator::Adjacent);
        UI_CHECK(sh->rules()[3].chain[2].combinator == SimpleSelector::Combinator::Sibling);
        UI_CHECK(sh->structural());
        for (const char* bad : {"a[", "a[x=]", "a[x~y]", "a:nth-child(foo)", "a +", "a:nth-child(2n+)"}) {
            d.clear();
            (void)StyleSheet::parse(std::string(bad) + " { width: 1px; }", &d);
            UI_CHECK(!d.empty());
        }
        i32 A = 0, B = 0;
        UI_CHECK(parseNth("odd", A, B) && A == 2 && B == 1);
        UI_CHECK(parseNth(" -n + 3 ", A, B) && A == -1 && B == 3);
        UI_CHECK(parseNth("+5", A, B) && A == 0 && B == 5);
        UI_CHECK(parseNth("n", A, B) && A == 1 && B == 0);
    }
    UiContext s({.viewport = {800, 600}});
    UI_CHECK(s.addStyleSheet(R"css(
panel { height: 1px; }
panel:first-child { height: 2px; }
panel:last-child { height: 3px; }
:nth-child(2n+3) { opacity: 0.25; }
label:nth-last-child(2) { height: 9px; }
label:nth-of-type(2) { width: 11px; }
panel:nth-last-of-type(1) { width: 12px; }
panel:first-of-type { width: 13px; }
panel:only-child { height: 4px; }
panel + label { border-width: 2px; }
.x ~ panel { gap: 4px; }
.k { color: #00ff00; }
panel[data-role]:first-child { color: #0000ff; }
[lang|=en] { height: 21px; }
[data-w~=baz] { width: 22px; }
[data-w^="en-"] { gap: 3px; }
[data-w$=baz] { border-width: 4px; }
[data-w*="foo b"] { opacity: 0.4; }
[data-w="EN-US FOO BAZ" i] { border-radius: 5px; }
[data-w="EN-US FOO BAZ"] { color: #ff0000; }
[data-w~="fo"] { padding: 9px; }
[id=main] { margin: 6px; }
)css", &diags).has_value());
    {
        const NodeHandle box = s.create(panel, s.root());
        NodeHandle k[6];
        k[0] = s.create(panel, box); k[1] = s.create(panel, box); k[2] = s.create(label, box);
        k[3] = s.create(panel, box); k[4] = s.create(label, box); k[5] = s.create(panel, box);
        s.addClass(k[1], "x");
        s.addClass(k[0], "k");
        s.setAttribute(k[0], "data-role", "a");
        s.update(0.f);
        const auto& c = [&](int i) -> const ComputedStyle& { return s.computed(k[i]); };
        UI_CHECK(c(0).height.value == 2.f && c(5).height.value == 3.f && c(1).height.value == 1.f);
        UI_CHECK(near(c(2).opacity, 0.25f) && near(c(4).opacity, 0.25f) && near(c(1).opacity, 1.f));
        UI_CHECK(c(4).height.value == 9.f && c(2).height.value != 9.f);
        UI_CHECK(c(4).width.value == 11.f && c(2).width.value != 11.f);
        UI_CHECK(c(5).width.value == 12.f && c(0).width.value == 13.f);
        UI_CHECK(c(2).borderWidth == 2.f && c(4).borderWidth == 2.f && c(5).borderWidth == 0.f);
        UI_CHECK(c(3).gap == 4.f && c(5).gap == 4.f && c(0).gap == 0.f && c(1).gap == 0.f);
        UI_CHECK(c(0).color == Color::rgb(0x0000ff)); // [attr]:first-child outranks .k
        UI_CHECK(s.computed(box).height.value == 4.f); // only child of the root
        // structural changes restyle siblings
        s.destroy(k[0]);
        const NodeHandle tail = s.create(panel, box);
        s.update(0.f);
        UI_CHECK(c(1).height.value == 2.f && c(5).height.value == 1.f && s.computed(tail).height.value == 3.f);
        UI_CHECK(near(c(3).opacity, 0.25f) && near(c(2).opacity, 1.f)); // positions shifted
        UI_CHECK(c(5).width.value != 12.f && s.computed(tail).width.value == 12.f);
        // attribute operators
        const NodeHandle a = s.create(label, s.root(), "main");
        s.setAttribute(a, "lang", "en-US");
        s.setAttribute(a, "data-w", "en-US foo baz");
        s.update(0.f);
        const ComputedStyle& ca = s.computed(a);
        UI_CHECK(ca.height.value == 21.f && ca.width.value == 22.f && ca.gap == 3.f && ca.borderWidth == 4.f);
        UI_CHECK(near(ca.opacity, 0.4f) && ca.radius[0] == 5.f && !(ca.color == Color::rgb(0xff0000)));
        UI_CHECK(ca.padding.l == 0.f && ca.margin.l == 6.f);
        UI_CHECK(s.attribute(a, "lang") == std::optional<std::string_view>("en-US"));
        s.setAttribute(a, "lang", "eng");
        s.removeAttribute(a, "data-w");
        s.update(0.f);
        UI_CHECK(s.computed(a).height.value != 21.f && s.computed(a).width.value != 22.f && s.computed(a).radius[0] == 0.f);
        UI_CHECK(!s.attribute(a, "data-w").has_value());
    }
    return finish("fuse_ui_css_animations");
}
