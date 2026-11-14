// Gate fuse_ui_style_invalidation: what a change restyles. The style pass only visits nodes marked dirty, so every
// selector that looks beyond the node itself needs the change to reach the nodes it can match:
//   * a class / state / attribute left of a descendant combinator restyles the whole subtree; left of child
//     combinators exactly as many levels as there are hops
//   * `+` / `~`: later siblings (and what the rest of the selector reaches below them)
//   * :has(): the anchors the argument can see - the parent for `:has(> x)`, earlier siblings for `:has(+ x)`,
//     ancestors for `:has(x)` - and the whole tree when the anchor is not the subject
//   * structural pseudo-classes: the siblings of an inserted / removed / moved node; moving a subtree restyles it
//   * changes no selector looks at restyle only the node and its children (the count is checked: no over-invalidation)
// (Adapted from the gui-framework gate of the same name.)
#include "test_util.hpp"

#include <cmath>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
bool near(f32 a, f32 b) { return std::fabs(a - b) < 1e-3f; }
struct Tree {
    UiContext ui{{.viewport = {400, 300}}};
    u32 panel = ui.registry().find("panel");
    NodeHandle add(NodeHandle parent, std::string_view cls = {}) {
        const NodeHandle n = ui.create(panel, parent);
        if (!cls.empty()) ui.addClass(n, cls);
        return n;
    }
    explicit Tree(const char* css) {
        std::vector<StyleDiagnostic> d;
        UI_CHECK(ui.addStyleSheet(css, &d).has_value() && d.empty());
    }
    f32 gap(NodeHandle n) { return ui.computed(n).gap; }
    u32 styled() { ui.update(0.f); return ui.stats().styled; }
};
} // namespace

int main() {
    // ---- descendant and child chains ----
    {
        Tree t(".a .c { gap: 1px; } .x > .y > .z { gap: 2px; } .p:hover .q { gap: 3px; } [data-m=on] .r { gap: 4px; }");
        const NodeHandle top = t.add(t.ui.root());
        const NodeHandle l1 = t.add(top), l2 = t.add(l1), l3 = t.add(l2, "c");
        const NodeHandle y = t.add(top, "y"), z = t.add(y, "z"), zDeep = t.add(z, "z");
        const NodeHandle q = t.add(l2, "q"), r = t.add(l3, "r");
        for (int k = 0; k < 20; ++k) t.add(t.add(t.ui.root())); // bystanders
        t.styled();
        UI_CHECK(near(t.gap(l3), 0.f) && near(t.gap(z), 0.f));
        // `.a .c`: three levels down
        t.ui.addClass(top, "a");
        const u32 n1 = t.styled();
        UI_CHECK(near(t.gap(l3), 1.f));
        UI_CHECK(n1 >= 9 && n1 <= 9); // top and its whole subtree (9 nodes), none of the bystanders
        t.ui.removeClass(top, "a");
        t.styled();
        UI_CHECK(near(t.gap(l3), 0.f));
        // `.x > .y > .z`: exactly two levels
        t.ui.addClass(top, "x");
        const u32 n2 = t.styled();
        UI_CHECK(near(t.gap(z), 2.f) && near(t.gap(zDeep), 0.f));
        UI_CHECK(n2 <= 6); // top, its children, its grandchildren - not the third level
        // a state left of a descendant combinator
        t.ui.addClass(top, "p");
        t.styled();
        t.ui.setState(top, kStateHover, true);
        t.styled();
        UI_CHECK(near(t.gap(q), 3.f));
        t.ui.setState(top, kStateHover, false);
        t.styled();
        UI_CHECK(near(t.gap(q), 0.f));
        // an attribute left of a descendant combinator
        t.ui.setAttribute(top, "data-m", "on");
        t.styled();
        UI_CHECK(near(t.gap(r), 4.f));
        t.ui.setAttribute(top, "data-m", "off");
        t.styled();
        UI_CHECK(near(t.gap(r), 0.f));
        // a class no selector looks at: the node and its children only
        t.ui.addClass(top, "unrelated");
        UI_CHECK_EQ(t.styled(), 3u); // top, l1, y
        t.ui.setState(l1, kStatePressed, true);
        UI_CHECK_EQ(t.styled(), 2u); // l1, l2
    }
    // ---- siblings ----
    {
        Tree t(".on + .n { gap: 1px; } .on ~ .l { gap: 2px; } .on ~ .w .d { gap: 3px; } .s:checked + .n { gap: 4px; }");
        const NodeHandle box = t.add(t.ui.root());
        const NodeHandle a = t.add(box), b = t.add(box, "n"), c = t.add(box, "l"), w = t.add(box, "w");
        const NodeHandle d = t.add(t.add(w), "d");
        const NodeHandle before = t.add(t.ui.root(), "l");
        t.styled();
        t.ui.addClass(a, "on");
        t.styled();
        UI_CHECK(near(t.gap(b), 1.f) && near(t.gap(c), 2.f) && near(t.gap(d), 3.f) && near(t.gap(before), 0.f));
        t.ui.removeClass(a, "on");
        t.styled();
        UI_CHECK(near(t.gap(b), 0.f) && near(t.gap(c), 0.f) && near(t.gap(d), 0.f));
        t.ui.addClass(a, "s");
        t.styled();
        t.ui.setChecked(a, true);
        t.styled();
        UI_CHECK(near(t.gap(b), 4.f));
        // structure: a node inserted in front changes who is adjacent
        t.ui.setChecked(a, false);
        t.ui.addClass(a, "on");
        t.styled();
        UI_CHECK(near(t.gap(b), 1.f));
        const NodeHandle wedge = t.add(box);
        t.ui.moveTo(wedge, box, b);
        t.styled();
        UI_CHECK(near(t.gap(b), 0.f) && near(t.gap(c), 2.f));
        t.ui.destroy(wedge);
        t.styled();
        UI_CHECK(near(t.gap(b), 1.f));
    }
    // ---- :has() ----
    {
        Tree t(".card:has(> .bad) { gap: 1px; } .row:has(+ .sel) { gap: 2px; } .group:has(.deep) { gap: 3px; } "
               ".form:has(.field:focus) { gap: 4px; }");
        const NodeHandle group = t.add(t.ui.root(), "group");
        const NodeHandle card = t.add(group, "card"), kid = t.add(card), grandkid = t.add(kid);
        const NodeHandle r1 = t.add(group, "row"), r2 = t.add(group, "row");
        const NodeHandle form = t.add(t.ui.root(), "form"), field = t.add(t.add(form), "field");
        t.styled();
        t.ui.addClass(kid, "bad");
        t.styled();
        UI_CHECK(near(t.gap(card), 1.f));
        t.ui.removeClass(kid, "bad");
        t.ui.addClass(grandkid, "bad"); // not a child: `>` does not see it
        t.styled();
        UI_CHECK(near(t.gap(card), 0.f));
        t.ui.addClass(r2, "sel");
        t.styled();
        UI_CHECK(near(t.gap(r1), 2.f) && near(t.gap(r2), 0.f));
        t.ui.addClass(grandkid, "deep");
        t.styled();
        UI_CHECK(near(t.gap(group), 3.f));
        t.ui.destroy(grandkid);
        t.styled();
        UI_CHECK(near(t.gap(group), 0.f));
        const NodeHandle late = t.add(kid, "deep"); // a new node with the class
        t.styled();
        UI_CHECK(near(t.gap(group), 3.f));
        t.ui.moveTo(late, form); // moved out: the old ancestors lose it
        t.styled();
        UI_CHECK(near(t.gap(group), 0.f));
        t.ui.setState(field, kStateFocus, true);
        t.styled();
        UI_CHECK(near(t.gap(form), 4.f));
        t.ui.setState(field, kStateFocus, false);
        t.styled();
        UI_CHECK(near(t.gap(form), 0.f));
    }
    // ---- :has() on a compound that is not the subject ----
    {
        Tree t(".page:has(.flag) .leaf { gap: 5px; }");
        const NodeHandle page = t.add(t.ui.root(), "page");
        const NodeHandle leaf = t.add(t.add(t.add(page)), "leaf");
        const NodeHandle other = t.add(page);
        t.styled();
        t.ui.addClass(other, "flag");
        t.styled();
        UI_CHECK(near(t.gap(leaf), 5.f));
        t.ui.removeClass(other, "flag");
        t.styled();
        UI_CHECK(near(t.gap(leaf), 0.f));
    }
    // ---- structure ----
    {
        Tree t(".it:nth-child(odd) { gap: 1px; } .it:last-child { gap: 2px; } .list:empty { gap: 3px; } .it:first-child .in { gap: 4px; } "
               ".zone .moved { gap: 6px; }");
        const NodeHandle list = t.add(t.ui.root(), "list");
        t.styled();
        UI_CHECK(near(t.gap(list), 3.f));
        std::vector<NodeHandle> it;
        for (int k = 0; k < 4; ++k) it.push_back(t.add(list, "it"));
        const NodeHandle in0 = t.add(it[0], "in"), in1 = t.add(it[1], "in");
        t.styled();
        UI_CHECK(near(t.gap(list), 0.f));
        UI_CHECK(near(t.gap(it[0]), 1.f) && near(t.gap(it[1]), 0.f) && near(t.gap(it[2]), 1.f) && near(t.gap(it[3]), 2.f));
        UI_CHECK(near(t.gap(in0), 4.f) && near(t.gap(in1), 0.f));
        t.ui.destroy(it[0]); // everything shifts: odd / even swap, a new first child (and what hangs below it)
        t.styled();
        UI_CHECK(near(t.gap(it[1]), 1.f) && near(t.gap(it[2]), 0.f) && near(t.gap(it[3]), 2.f) && near(t.gap(in1), 4.f));
        t.ui.moveTo(it[3], list, it[1]); // to the front
        t.styled();
        UI_CHECK(near(t.gap(it[3]), 1.f) && near(t.gap(it[1]), 0.f) && near(t.gap(it[2]), 2.f) && near(t.gap(in1), 0.f));
        // a moved subtree is restyled under its new ancestors
        const NodeHandle zone = t.add(t.ui.root(), "zone"), carrier = t.add(t.ui.root()), moved = t.add(t.add(carrier), "moved");
        t.styled();
        UI_CHECK(near(t.gap(moved), 0.f));
        t.ui.moveTo(carrier, zone);
        t.styled();
        UI_CHECK(near(t.gap(moved), 6.f));
        t.ui.moveTo(carrier, t.ui.root());
        t.styled();
        UI_CHECK(near(t.gap(moved), 0.f));
        for (NodeHandle n : {it[1], it[2], it[3]}) t.ui.destroy(n);
        t.styled();
        UI_CHECK(near(t.gap(list), 3.f));
    }
    // ---- sheets without such selectors cost nothing extra ----
    {
        Tree t("panel { gap: 1px; } .k { gap: 2px; }");
        UI_CHECK_EQ(t.ui.styles().features(), 0u);
        const NodeHandle a = t.add(t.ui.root()), b = t.add(a);
        t.add(b);
        t.styled();
        t.ui.addClass(a, "k");
        UI_CHECK_EQ(t.styled(), 2u); // a and its child, as before
        t.add(t.ui.root());
        UI_CHECK_EQ(t.styled(), 1u); // a new node styles itself only
    }
    return finish("fuse_ui_style_invalidation");
}
