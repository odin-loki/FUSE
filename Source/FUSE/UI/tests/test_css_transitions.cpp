// Gate fuse_ui_css_transitions: CSS Transitions 1 `transition` — parsing (lists, times, easing keywords,
// cubic-bezier(), steps(), shorthand property names), timing functions, tweening on hover / class changes from the
// shown value, delay, retargeting mid-flight, inherited colour, layout properties, `none` and discrete properties,
// scratch styles reused (no style-table growth per frame), node destruction mid-flight.
#include "test_util.hpp"

#include <cmath>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }
// `transition: <text>` applied to a default style (the shorthand expands to the four longhands)
bool styleOf(std::string_view text, ComputedStyle& cs) {
    std::vector<StyleDiagnostic> diags;
    auto sh = StyleSheet::parse("x { transition: " + std::string(text) + "; }", &diags);
    if (!sh || !diags.empty() || sh->rules().empty()) return false;
    cs = {};
    for (const Declaration& d : sh->rules()[0].decls) applyProp(cs, d.prop, d.value);
    return true;
}
bool parses(std::string_view text) { ComputedStyle cs; return styleOf(text, cs); }
TransitionSpec spec(std::string_view text, Prop p = Prop::Width) {
    ComputedStyle cs;
    TransitionSpec t;
    UI_CHECK(styleOf(text, cs));
    UI_CHECK(cs.transitionFor(u8(p), t));
    return t;
}
bool covers(std::string_view text, Prop p) { ComputedStyle cs; TransitionSpec t; return styleOf(text, cs) && cs.transitionFor(u8(p), t); }
} // namespace

int main() {
    // ---- parsing ----
    {
        ComputedStyle cs;
        UI_CHECK(styleOf("background-color 200ms ease-in 0.1s, opacity 1s linear", cs));
        UI_CHECK_EQ(int(cs.transitionPropertyCount), 2);
        TransitionSpec t;
        UI_CHECK(cs.transitionFor(u8(Prop::Background), t) && cs.transitionFor(u8(Prop::Background2), t));
        UI_CHECK(near(t.duration, 0.2f) && near(t.delay, 0.1f));
        UI_CHECK(t.ease.x1 == 0.42f && t.ease.x2 == 1.f);
        UI_CHECK(cs.transitionFor(u8(Prop::Opacity), t) && t.duration == 1.f);
        UI_CHECK(!cs.transitionFor(u8(Prop::Width), t));
        UI_CHECK(styleOf("none", cs) && !cs.transitionFor(u8(Prop::Width), t));
        UI_CHECK(covers("2s", Prop::Width) && covers("2s", Prop::Color));
        UI_CHECK(!covers("2s", Prop::Layout)); // discrete: not animated
        UI_CHECK(covers("padding 1s", Prop::PaddingLeft) && covers("padding 1s", Prop::PaddingBottom) && !covers("padding 1s", Prop::Width));
        UI_CHECK(spec("1s cubic-bezier(0.1, 0.7, 1.0, 0.1) width").ease.x2 == 1.f);
        const TransitionSpec st = spec("color 1s steps(4, jump-start)", Prop::Color);
        UI_CHECK(st.ease.kind == Easing::Kind::Steps && st.ease.steps == 4 && st.ease.stepPos == Easing::StepPos::JumpStart);
        UI_CHECK(!parses("width -1s"));                        // negative duration
        UI_CHECK(parses("width 1s -0.5s"));                    // negative delay is fine
        UI_CHECK(!parses("width 1s 2s 3s"));                   // three times
        UI_CHECK(!parses("not-a-prop 1s"));
        UI_CHECK(!parses("width 1s cubic-bezier(2, 0, 1, 1)")); // x outside [0, 1]
        UI_CHECK(!parses("width 1s steps(0)"));
        UI_CHECK(!parses("width 1s steps(1, jump-none)"));
        UI_CHECK(!parses("width 1s, , opacity 1s"));
        UI_CHECK(!parses("width 1s, none"));                   // none only alone
    }
    // ---- timing functions ----
    {
        const TransitionSpec lin = spec("1s linear"), ease = spec("1s ease"), easeIn = spec("1s ease-in");
        UI_CHECK(near(transitionEase(lin, 0.3f), 0.3f));
        UI_CHECK(near(transitionEase(ease, 0.f), 0.f) && near(transitionEase(ease, 1.f), 1.f));
        UI_CHECK(near(transitionEase(ease, 0.5f), 0.8024f, 2e-3f));   // reference value of ease at 0.5
        UI_CHECK(near(transitionEase(easeIn, 0.5f), 0.3153f, 2e-3f)); // ease-in at 0.5
        const TransitionSpec over = spec("1s cubic-bezier(0.3, 1.6, 0.7, 1.6)");
        UI_CHECK(transitionEase(over, 0.5f) > 1.f); // overshoot is kept
        const TransitionSpec s4 = spec("1s steps(4)"), s4s = spec("1s steps(4, start)"), s3n = spec("1s steps(3, jump-none)"),
                             s2b = spec("1s steps(2, jump-both)");
        UI_CHECK(near(transitionEase(s4, 0.f), 0.f) && near(transitionEase(s4, 0.3f), 0.25f) && near(transitionEase(s4, 0.99f), 0.75f));
        UI_CHECK(near(transitionEase(s4, 1.f), 1.f));
        UI_CHECK(near(transitionEase(s4s, 0.f), 0.25f) && near(transitionEase(s4s, 0.8f), 1.f));
        UI_CHECK(near(transitionEase(s3n, 0.f), 0.f) && near(transitionEase(s3n, 0.5f), 0.5f) && near(transitionEase(s3n, 0.9f), 1.f));
        UI_CHECK(near(transitionEase(s2b, 0.f), 1.f / 3.f) && near(transitionEase(s2b, 0.6f), 2.f / 3.f));
    }
    // ---- tweening in the context ----
    UiContext ui({.viewport = {400, 300}});
    const u32 panel = ui.registry().find("panel"), label = ui.registry().find("label");
    std::vector<StyleDiagnostic> diags;
    UI_CHECK(ui.addStyleSheet(R"css(
panel { background: #000000; opacity: 1; width: 100px; height: 40px; color: #000000;
        transition: background 1s linear, opacity 2s linear 1s, width 1s linear, color 1s linear; }
panel:hover { background: #ff0000; opacity: 0; width: 200px; color: #ffffff; }
.off { transition: none; }
.discrete { layout: grid; }
.steps { transition: background 1s steps(2); }
)css", &diags).has_value());
    UI_CHECK(diags.empty());

    const NodeHandle p = ui.create(panel, ui.root());
    const NodeHandle child = ui.create(label, p);
    ui.update(0.016f);
    UI_CHECK(ui.computed(p).background == Color::rgb(0x000000)); // first style: no transition
    UI_CHECK_EQ(ui.transitionCount(), 0u);
    UI_CHECK(!ui.needsUpdate());

    ui.setState(p, kStateHover, true);
    ui.update(0.f); // starts: shows the old values
    UI_CHECK_EQ(ui.transitionCount(), 1u);
    UI_CHECK(ui.needsUpdate());
    UI_CHECK(near(ui.computed(p).background.r, 0.f));
    ui.update(0.25f);
    UI_CHECK(near(ui.computed(p).background.r, 0.25f));
    UI_CHECK(near(ui.computed(p).opacity, 1.f));            // still in its 1 s delay
    UI_CHECK(near(ui.rect(p).w, 125.f, 0.01f));             // layout follows the tweened width
    UI_CHECK(near(ui.computed(child).color.r, 0.25f));      // inherited colour follows the parent's tween
    const u32 table = ui.styles().tableSize();
    for (int k = 0; k < 10; ++k) ui.update(0.05f);          // t = 0.75
    UI_CHECK_EQ(ui.styles().tableSize(), table);            // the scratch slot is reused every frame
    UI_CHECK(near(ui.computed(p).background.r, 0.75f));
    UI_CHECK(near(ui.computed(p).opacity, 1.f));
    ui.update(0.5f); // t = 1.25: background / width / color done, opacity 0.25 / 2 into its run
    UI_CHECK(ui.computed(p).background == Color::rgb(0xff0000));
    UI_CHECK(near(ui.rect(p).w, 200.f));
    UI_CHECK(near(ui.computed(p).opacity, 1.f - 0.125f));

    // retarget mid-flight: leave hover; opacity tweens back from the shown value
    ui.setState(p, kStateHover, false);
    ui.update(0.f);
    const f32 shown = ui.computed(p).opacity;
    UI_CHECK(near(shown, 0.875f));
    UI_CHECK(near(ui.computed(p).background.r, 1.f));
    // reversing: opacity returns to its start value, so the 2 s run shortens by how far it got (0.125): 0.25 s after
    // the 1 s delay (CSS Transitions 1 §3.3)
    ui.update(1.1f);
    UI_CHECK(ui.computed(p).background == Color::rgb(0x000000));
    UI_CHECK(near(ui.computed(p).opacity, 0.875f + 0.125f * 0.4f));
    ui.update(5.f);
    UI_CHECK(near(ui.computed(p).opacity, 1.f));
    ui.update(0.f);
    UI_CHECK_EQ(ui.transitionCount(), 0u);
    UI_CHECK(!ui.needsUpdate());

    // transition: none on the new style: immediate; a discrete property switches at once too
    ui.addClass(p, "off");
    ui.update(0.f);
    ui.setState(p, kStateHover, true);
    ui.update(0.f);
    UI_CHECK(ui.computed(p).background == Color::rgb(0xff0000));
    UI_CHECK_EQ(ui.transitionCount(), 0u);
    ui.removeClass(p, "off");
    ui.addClass(p, "discrete");
    ui.setState(p, kStateHover, false);
    ui.update(0.f);
    UI_CHECK(ui.computed(p).layout == LayoutKind::Grid);
    UI_CHECK_EQ(ui.transitionCount(), 1u); // background back to black runs; layout did not wait
    ui.update(2.f);
    ui.update(0.f);

    // steps(2): holds, then jumps
    ui.removeClass(p, "discrete");
    ui.addClass(p, "steps");
    ui.update(0.f);
    ui.setState(p, kStateHover, true);
    ui.update(0.f);
    ui.update(0.3f);
    UI_CHECK(near(ui.computed(p).background.r, 0.f));
    ui.update(0.4f);
    UI_CHECK(near(ui.computed(p).background.r, 0.5f));

    // destroying a node mid-flight stops its transition and frees the slot
    const NodeHandle q = ui.create(panel, ui.root());
    ui.update(0.f);
    ui.setState(q, kStateHover, true);
    ui.update(0.1f);
    UI_CHECK(ui.transitionCount() >= 1u);
    ui.destroy(q);
    ui.destroy(p);
    ui.update(0.1f);
    UI_CHECK_EQ(ui.transitionCount(), 0u);
    const NodeHandle r = ui.create(panel, ui.root()); // reuses the freed node: first style, no transition
    ui.setState(r, kStateHover, true);
    ui.update(0.f);
    UI_CHECK(ui.computed(r).background == Color::rgb(0xff0000));
    UI_CHECK_EQ(ui.transitionCount(), 0u);
    return finish("fuse_ui_css_transitions");
}
