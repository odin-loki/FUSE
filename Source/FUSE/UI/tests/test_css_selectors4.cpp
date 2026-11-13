// Gate fuse_ui_css_selectors4: the extended selectors of the style engine.
//   * a matching table: attribute selectors (every operator, the ` i` flag, `id`, [class~=x] against the class list),
//     :first-child / :last-child / :only-child / :nth-child(an+b) / :nth-last-child, :empty (children and text),
//     the sibling combinators `+` and `~`, :not / :is / :where / :has (relative selectors), :root in a complex
//     selector, backtracking over descendant hops
//   * parsing and specificity: the most specific argument of :not / :is / :has counts, :where adds nothing,
//     forgiving lists for :is / :where, errors for the rest
//   * the cascade follows the tree: class / attribute / text / structure changes restyle what a selector reaches
// (Adapted from the gui-framework gate of the same name; its node reorder API, transition-behavior and
// animation-composition parts are not ported.)
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

/// A fixed tree; `matching(selector)` lists the ids of the nodes a selector matches, in tree order.
struct Table {
    UiContext ui{{.viewport = {400, 300}}};
    std::vector<std::pair<std::string, NodeHandle>> nodes;
    NodeHandle add(const char* cls, NodeHandle parent, const char* id, std::initializer_list<const char*> classes = {}) {
        const NodeHandle n = ui.create(ui.registry().find(cls), parent, id);
        for (const char* c : classes) ui.addClass(n, c);
        nodes.emplace_back(id, n);
        return n;
    }
    std::string matching(std::string_view selector) {
        ui.clearStyleSheets();
        std::vector<StyleDiagnostic> diags;
        const bool ok = ui.addStyleSheet(std::string(selector) + " { min-width: 77px; }", &diags).has_value();
        if (!ok || !diags.empty()) return "<bad>";
        ui.update(0.f);
        std::string out;
        for (auto& [id, n] : nodes)
            if (near(ui.computed(n).minWidth.value, 77.f)) out += (out.empty() ? "" : " ") + id;
        return out;
    }
};
} // namespace

int main() {
    // ---- parsing and specificity ----
    UI_CHECK_EQ(spec(":not(#a, .b)"), 1u << 16); // the most specific argument
    UI_CHECK_EQ(spec(":is(a, .b.c)"), 2u << 8);
    UI_CHECK_EQ(spec("a:where(#x, .y)"), 1u); // :where adds nothing
    UI_CHECK_EQ(spec("a:has(> .b)"), (1u << 8) | 1u);
    UI_CHECK_EQ(spec(":empty"), 1u << 8);
    UI_CHECK_EQ(spec("a[x=y]:nth-child(2n+1)"), (2u << 8) | 1u);
    UI_CHECK_EQ(spec("#i .c > t + u ~ v"), (1u << 16) | (1u << 8) | 3u);
    UI_CHECK(!bad(":is(a, :bogus(), .c)")); // forgiving
    UI_CHECK(!bad(":where()"));
    UI_CHECK(bad(":not(a, :bogus())") && bad(":has()") && bad(":not(") && bad(":is(a") && bad(":not()"));
    UI_CHECK(!bad("a:not(:is(.x, .y) > b)") && !bad("a:has(+ b, ~ c .d)"));
    UI_CHECK(bad("a >") && bad("> a") && bad("a + + b") && bad("a::before") && bad("[x") && bad("[=y]") && bad("a[x=]"));
    UI_CHECK(bad(":nth-child(n n)") && bad(":nth-child()") && bad(":nth-child(2n+)") && !bad(":nth-child( -n + 3 )"));
    UI_CHECK(!bad("[data-x]") && !bad("[data-x='a b' i]") && !bad("[data-x^=\"a\"]") && !bad("a[b|=c][d$=e][f*=g]"));

    // ---- matching table ----
    {
        Table t;
        const NodeHandle root = t.ui.root();
        const NodeHandle a = t.add("panel", root, "a", {"box"});
        t.add("label", a, "a1", {"item", "first"});
        const NodeHandle a2 = t.add("label", a, "a2", {"item", "hot"});
        t.add("label", a, "a3", {"item"});
        t.add("label", a, "a4", {"item", "last"});
        const NodeHandle b = t.add("panel", root, "b", {"box", "alt"});
        const NodeHandle b1 = t.add("panel", b, "b1", {"inner"});
        t.add("label", b1, "b1x", {"deep"});
        t.add("label", b, "b2", {});
        const NodeHandle c = t.add("panel", root, "c", {});
        t.ui.setText(a2, "text");
        t.ui.setAttribute(a, "data-kind", "warning big");
        t.ui.setAttribute(b, "data-kind", "info");
        t.ui.setAttribute(b, "lang", "en-GB");
        t.ui.setAttribute(c, "data-kind", "Warning");
        struct Row { const char* selector; const char* want; };
        const Row rows[] = {
            // attributes
            {"[data-kind]", "a b c"},
            {"[data-kind=info]", "b"},
            {"[data-kind='warning big']", "a"},
            {"[data-kind~=big]", "a"},
            {"[data-kind~=warning]", "a"},
            {"[data-kind^=warn]", "a"},
            {"[data-kind^=warn i]", "a c"},
            {"[data-kind$=ing]", "c"},
            {"[data-kind*=nin]", "a c"},
            {"[lang|=en]", "b"},
            {"[lang|=e]", ""},
            {"[id=b1]", "b1"},
            {"[id^=a]", "a a1 a2 a3 a4"},
            {"[class~=hot]", "a2"},
            {"panel[data-kind]:not([lang])", "a c"},
            // structure
            {"label:first-child", "a1 b1x"},
            {"label:last-child", "a4 b1x b2"},
            {"label:only-child", "b1x"},
            {".item:nth-child(2)", "a2"},
            {".item:nth-child(odd)", "a1 a3"},
            {".item:nth-child(even)", "a2 a4"},
            {".item:nth-child(2n+1)", "a1 a3"},
            {".item:nth-child(n+3)", "a3 a4"},
            {".item:nth-child(-n+2)", "a1 a2"},
            {".item:nth-last-child(1)", "a4"},
            {".item:nth-last-child(-n+2)", "a3 a4"},
            {"label:empty", "a1 a3 a4 b1x b2"}, // a2 has text
            {"panel:empty", "c"},
            // siblings
            {".first + .item", "a2"},
            {".hot ~ .item", "a3 a4"},
            {".first ~ .last", "a4"},
            {".box + .box > .inner", "b1"},
            {".box ~ panel", "b c"},
            {".inner + label", "b2"},
            // logical
            {".item:not(.hot)", "a1 a3 a4"},
            {".item:not(.first, .last)", "a2 a3"},
            {":is(.first, .last)", "a1 a4"},
            {"panel:is(.alt, #c)", "b c"},
            {":where(.inner) > label", "b1x"},
            {".box:has(.deep)", "b"},
            {"panel:has(> .deep)", "b1"},
            {".box:has(> .hot)", "a"},
            {".item:has(+ .hot)", "a1"},
            {".item:has(~ .last)", "a1 a2 a3"},
            {".box:not(:has(.hot))", "b"},
            {"panel:has(.hot, .deep)", "a b b1"},
            {".box:has(> .inner > .deep)", "b"},
            // descendant / child chains, backtracking over the nearest ancestor
            {".box label", "a1 a2 a3 a4 b1x b2"},
            {".box > label", "a1 a2 a3 a4 b2"},
            {".alt > .inner label", "b1x"},
            {"root > panel label.deep", "b1x"},
            {":root > .alt", "b"},
            {":root .deep", "b1x"},
            {":root", "root"},
            {"*:not(panel):not(label)", "root"},
        };
        t.nodes.insert(t.nodes.begin(), {"root", root});
        for (const Row& r : rows) {
            const std::string got = t.matching(r.selector);
            if (got != r.want) {
                std::fprintf(stderr, "selector `%s`: matched \"%s\", want \"%s\"\n", r.selector, got.c_str(), r.want);
                UI_CHECK(false);
            }
        }
    }

    // ---- the cascade follows the tree ----
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
[data-state=open] > label { max-height: 60px; }
label:first-child { width: 2px; }
label:last-child { width: 30px; }
label.x ~ label { letter-spacing: 3px; }
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
    ui.addClass(p, "tok");
    ui.update(0.f);
    UI_CHECK(near(cs(p).maxWidth.value, 50.f));
    ui.removeClass(p, "tok");
    ui.update(0.f);
    UI_CHECK(!near(cs(p).maxWidth.value, 50.f));
    // :empty reacts to children and to text; :has(> label.hot) to a child's class
    const NodeHandle l1 = ui.create(label, p);
    const NodeHandle l2 = ui.create(label, p);
    ui.update(0.f);
    UI_CHECK(!near(cs(p).minWidth.value, 9.f));
    UI_CHECK(near(cs(l1).minHeight.value, 6.f));
    ui.setText(l1, "hi");
    ui.update(0.f);
    UI_CHECK(!near(cs(l1).minHeight.value, 6.f) && near(cs(l2).minHeight.value, 6.f));
    ui.setText(l1, "");
    ui.update(0.f);
    UI_CHECK(near(cs(l1).minHeight.value, 6.f));
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
    // attributes
    std::string_view got;
    UI_CHECK(!ui.attribute(p, "data-state", got));
    ui.setAttribute(p, "data-state", "open");
    ui.update(0.f);
    UI_CHECK(ui.attribute(p, "data-state", got) && got == "open");
    UI_CHECK(near(cs(l1).maxHeight.value, 60.f) && near(cs(l2).maxHeight.value, 60.f));
    ui.setAttribute(p, "data-state", "closed");
    ui.update(0.f);
    UI_CHECK(!near(cs(l1).maxHeight.value, 60.f));
    ui.setAttribute(p, "data-state", "open");
    ui.removeAttribute(p, "data-state");
    ui.update(0.f);
    UI_CHECK(!near(cs(l1).maxHeight.value, 60.f) && !ui.attribute(p, "data-state", got));
    // structure: first / last child follow insertions, removals and moves
    UI_CHECK(near(cs(l1).width.value, 2.f) && near(cs(l2).width.value, 30.f));
    const NodeHandle l3 = ui.create(label, p);
    ui.update(0.f);
    UI_CHECK(near(cs(l3).width.value, 30.f) && !near(cs(l2).width.value, 30.f));
    ui.moveTo(l3, p, l1); // now the first child
    ui.update(0.f);
    UI_CHECK(near(cs(l3).width.value, 2.f) && !near(cs(l1).width.value, 2.f) && near(cs(l2).width.value, 30.f));
    ui.destroy(l2);
    ui.update(0.f);
    UI_CHECK(near(cs(l1).width.value, 30.f));
    // later siblings restyle when an earlier one gets the class
    UI_CHECK(!near(cs(l1).letterSpacing, 3.f));
    ui.addClass(l3, "x");
    ui.update(0.f);
    UI_CHECK(near(cs(l1).letterSpacing, 3.f) && !near(cs(l3).letterSpacing, 3.f));
    return finish("fuse_ui_css_selectors4");
}
