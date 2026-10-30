// Gate fuse_ui_css_selectors4: Selectors 4 logical pseudo-classes (:not / :is / :where / :has, forgiving lists and
// specificity), :empty, [class~=x] against the live class list, invalidation on tree / class / text changes, the node
// reorder API, transition-behavior: allow-discrete and animation-composition.
#include "test_util.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool bad(std::string_view sel) {
    std::vector<StyleDiagnostic> d;
    auto sh = StyleSheet::parse(std::string(sel) + " { width: 1px; }", &d);
    return !d.empty() || !sh || sh->rules().empty();
}
u32 spec(std::string_view sel) {
    auto sh = StyleSheet::parse(std::string(sel) + " { width: 1px; }");
    return sh && !sh->rules().empty() ? sh->rules()[0].specificity : ~0u;
}
} // namespace

int main() {
    // ---- parsing and specificity ----
    UI_CHECK_EQ(spec(":not(#a, .b)"), 1u << 16);       // max of the arguments
    UI_CHECK_EQ(spec(":is(a, .b.c)"), 2u << 8);
    UI_CHECK_EQ(spec("a:where(#x, .y)"), 1u);          // :where adds nothing
    UI_CHECK_EQ(spec("a:has(> .b)"), (1u << 8) | 1u);
    UI_CHECK_EQ(spec(":empty"), 1u << 8);
    UI_CHECK(!bad(":is(a, :bogus(), .c)"));           // forgiving
    UI_CHECK(!bad(":where()"));
    UI_CHECK(bad(":not(a, :bogus())") && bad(":has()") && bad(":not(") && bad(":is(a"));
    UI_CHECK(!bad("a:not(:is(.x, .y) > b)") && !bad("a:has(+ b, ~ c .d)"));
    {
        auto sh = StyleSheet::parse("a:has(b) {} c {}");
        UI_CHECK(sh && sh->relational());
        auto sh2 = StyleSheet::parse("a:is(b) {}");
        UI_CHECK(sh2 && !sh2->relational());
    }

    UiContext ui({.viewport = {800, 600}});
    const u32 panel = ui.registry().find("panel"), label = ui.registry().find("label");
    std::vector<StyleDiagnostic> diags;
    UI_CHECK(ui.addStyleSheet(R"css(
panel { width: 1px; height: 1px; }
panel:not(.on) { height: 2px; }
panel:is(.a, .b) { width: 3px; }
:where(panel.w) { width: 4px; }
panel.w { width: 5px; }
panel:has(> label.hot) { opacity: 0.5; }
panel:has(.deep) { gap: 7px; }
label:has(+ label.next) { height: 8px; }
panel:empty { min-width: 9px; }
label:empty { min-height: 6px; }
[class~=tok] { max-width: 50px; }
[class~="z"] { max-height: 60px; }
)css", &diags).has_value());
    UI_CHECK(diags.empty());
    auto cs = [&](NodeHandle n) { return ui.computed(n); };
    const NodeHandle p = ui.create(panel, ui.root());
    ui.update(0.f);
    UI_CHECK(near(cs(p).height.value, 2.f) && near(cs(p).width.value, 1.f));
    UI_CHECK(near(cs(p).minWidth.value, 9.f)); // :empty
    ui.addClass(p, "on");
    ui.addClass(p, "b");
    ui.update(0.f);
    UI_CHECK(near(cs(p).height.value, 1.f) && near(cs(p).width.value, 3.f));
    ui.addClass(p, "w");
    ui.update(0.f);
    UI_CHECK(near(cs(p).width.value, 5.f)); // :where loses to a plain class
    // [class~=]
    ui.addClass(p, "tok");
    ui.update(0.f);
    UI_CHECK(near(cs(p).maxWidth.value, 50.f));
    ui.removeClass(p, "tok");
    ui.update(0.f);
    UI_CHECK(!near(cs(p).maxWidth.value, 50.f));
    ui.setAttribute(p, "class", "z q");
    ui.update(0.f);
    UI_CHECK(near(cs(p).maxHeight.value, 60.f) && ui.hasClass(p, "q") && !ui.hasClass(p, "on"));
    UI_CHECK(ui.attribute(p, "class") && *ui.attribute(p, "class") == "z q");
    // :has(> label.hot) reacts to a child's class; :empty to children and text
    const NodeHandle l1 = ui.create(label, p);
    const NodeHandle l2 = ui.create(label, p);
    ui.update(0.f);
    UI_CHECK(!near(cs(p).minWidth.value, 9.f));
    UI_CHECK(near(cs(l1).minHeight.value, 6.f));
    ui.setText(l1, "hi");
    ui.update(0.f);
    UI_CHECK(!near(cs(l1).minHeight.value, 6.f) && near(cs(l2).minHeight.value, 6.f));
    UI_CHECK(!near(cs(p).opacity, 0.5f));
    ui.addClass(l2, "hot");
    ui.update(0.f);
    UI_CHECK(near(cs(p).opacity, 0.5f));
    ui.removeClass(l2, "hot");
    ui.update(0.f);
    UI_CHECK(near(cs(p).opacity, 1.f));
    // :has(+ x): the previous sibling restyles when the next one changes
    ui.addClass(l2, "next");
    ui.update(0.f);
    UI_CHECK(near(cs(l1).height.value, 8.f) && !near(cs(l2).height.value, 8.f));
    // descendant :has through a grandchild
    const NodeHandle inner = ui.create(panel, l2);
    const NodeHandle g = ui.create(label, inner);
    ui.update(0.f);
    UI_CHECK(!near(cs(p).gap, 7.f));
    ui.addClass(g, "deep");
    ui.update(0.f);
    UI_CHECK(near(cs(p).gap, 7.f) && near(cs(inner).gap, 7.f));
    ui.destroy(g);
    ui.update(0.f);
    UI_CHECK(!near(cs(p).gap, 7.f));

    // ---- reorder API ----
    {
        const NodeHandle box = ui.create(panel, ui.root());
        std::vector<NodeHandle> k;
        for (int i = 0; i < 4; ++i) k.push_back(ui.create(label, box));
        auto order = [&] {
            std::string s;
            for (NodeHandle c = ui.firstChild(box); c; c = ui.nextSibling(c))
                for (int i = 0; i < 4; ++i)
                    if (k[std::size_t(i)] == c) s += char('a' + i);
            return s;
        };
        UI_CHECK_EQ(order(), std::string("abcd"));
        UI_CHECK(ui.lastChild(box) == k[3] && ui.prevSibling(k[1]) == k[0] && ui.childAt(box, 2) == k[2]);
        UI_CHECK_EQ(ui.childIndex(k[3]), 3);
        UI_CHECK(ui.insertBefore(k[3], k[0]));
        UI_CHECK_EQ(order(), std::string("dabc"));
        UI_CHECK(ui.insertAfter(k[3], k[2]));
        UI_CHECK_EQ(order(), std::string("abcd"));
        UI_CHECK(ui.setChildIndex(k[0], 2));
        UI_CHECK_EQ(order(), std::string("bcad"));
        UI_CHECK(ui.swapNodes(k[1], k[3]));
        UI_CHECK_EQ(order(), std::string("dcab"));
        const NodeHandle perm[] = {k[0], k[1], k[2], k[3]};
        UI_CHECK(ui.reorderChildren(box, perm));
        UI_CHECK_EQ(order(), std::string("abcd"));
        const NodeHandle badPerm[] = {k[0], k[0], k[2], k[3]};
        UI_CHECK(!ui.reorderChildren(box, badPerm));
        UI_CHECK(!ui.insertBefore(box, k[0])); // a node can't go inside its own subtree
        ui.sortChildren(box, [&](NodeHandle x, NodeHandle y) { return ui.childIndex(x) > ui.childIndex(y); });
        UI_CHECK_EQ(order(), std::string("dcba"));
        ui.destroy(box);
    }
    // structural selectors follow reordering
    {
        UiContext s({.viewport = {200, 200}});
        UI_CHECK(s.addStyleSheet("label { width: 1px; } label:first-child { width: 2px; } panel:has(> label.x:last-child) { width: 30px; }").has_value());
        const NodeHandle b = s.create(panel, s.root());
        const NodeHandle x = s.create(label, b), y = s.create(label, b);
        s.addClass(x, "x");
        s.update(0.f);
        UI_CHECK(near(s.computed(x).width.value, 2.f) && near(s.computed(y).width.value, 1.f));
        UI_CHECK(!near(s.computed(b).width.value, 30.f));
        UI_CHECK(s.swapNodes(x, y));
        s.update(0.f);
        UI_CHECK(near(s.computed(y).width.value, 2.f) && near(s.computed(x).width.value, 1.f));
        UI_CHECK(near(s.computed(b).width.value, 30.f));
    }

    // ---- transition-behavior: allow-discrete ----
    {
        UiContext s({.viewport = {200, 200}});
        UI_CHECK(s.addStyleSheet(R"css(
panel { opacity: 1; layout: box; transition: opacity 1s linear, display 1s allow-discrete, layout 1s; }
panel.gone { display: none; opacity: 0; layout: grid; }
panel.d2 { transition: all 1s linear allow-discrete; }
)css").has_value());
        const NodeHandle n = s.create(panel, s.root());
        s.update(0.f);
        s.addClass(n, "gone");
        s.update(0.f);
        s.update(0.25f);
        UI_CHECK(s.computed(n).visible);                 // display stays shown through the transition
        UI_CHECK(near(s.computed(n).opacity, 0.75f));
        UI_CHECK(s.computed(n).layout == LayoutKind::Grid); // layout: normal behaviour, no transition
        s.update(0.5f);
        UI_CHECK(s.computed(n).visible);
        s.update(0.5f);
        UI_CHECK(!s.computed(n).visible);
        // back to shown: visible immediately
        s.removeClass(n, "gone");
        s.update(0.f);
        s.update(0.1f);
        UI_CHECK(s.computed(n).visible);
        // all + allow-discrete: enums flip at the midpoint
        const NodeHandle m = s.create(panel, s.root());
        s.addClass(m, "d2");
        s.update(0.f);
        s.addClass(m, "gone");
        s.update(0.f);
        s.update(0.4f);
        UI_CHECK(s.computed(m).layout == LayoutKind::Box);
        s.update(0.2f);
        UI_CHECK(s.computed(m).layout == LayoutKind::Grid);
        TransitionSpec t;
        UI_CHECK(s.computed(m).transitionFor(u8(Prop::Layout), t) && t.allowDiscrete);
        PropValue v;
        UI_CHECK(parsePropValue(Prop::TransitionBehavior, "normal, allow-discrete", v));
        UI_CHECK(!parsePropValue(Prop::TransitionBehavior, "discrete", v));
    }

    // ---- animation-composition ----
    {
        UiContext s({.viewport = {400, 200}});
        UI_CHECK(s.addStyleSheet(R"css(
@keyframes w { from { width: 10px; } to { width: 20px; } }
@keyframes o { from { opacity: 0.25; } to { opacity: 0.25; } }
panel { width: 100px; opacity: 0.5; }
.r { animation: w 1s linear; }
.add { animation: w 1s linear; animation-composition: add; }
.acc { animation: w 1s linear, o 1s linear; animation-composition: accumulate; }
.stack { animation: w 1s linear, w 1s linear; animation-composition: replace, add; }
)css").has_value());
        PropValue v;
        UI_CHECK(parsePropValue(Prop::AnimationComposition, "replace, add, accumulate", v));
        UI_CHECK(!parsePropValue(Prop::AnimationComposition, "multiply", v));
        auto at = [&](const char* cls, f32 t) {
            const NodeHandle n = s.create(panel, s.root());
            s.addClass(n, cls);
            s.update(0.f);
            s.update(t);
            const ComputedStyle c = s.computed(n);
            s.destroy(n);
            return c;
        };
        UI_CHECK(near(at("r", 0.5f).width.value, 15.f));
        UI_CHECK(near(at("add", 0.5f).width.value, 115.f));
        const ComputedStyle a = at("acc", 0.5f);
        UI_CHECK(near(a.width.value, 115.f) && near(a.opacity, 0.75f));
        UI_CHECK(near(at("stack", 0.5f).width.value, 30.f)); // second adds onto the first
    }
    return finish("fuse_ui_css_selectors4");
}
