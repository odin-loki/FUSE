// Gate fuse_ui_style_invalidation: invalidation sets. A class / attribute / state change restyles every descendant a
// selector can reach (`.a .b .c` at any depth, `.a > .b > .c` exactly two levels), later siblings and their subtrees
// for `+` / `~`, and only the :has() anchors the argument can see (`:has(> x)` parent, `:has(+ x)` previous sibling,
// `:has(x)` ancestors); unrelated changes restyle just the node. Moving a subtree restyles it.
#include "test_util.hpp"

#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;

int main() {
    UiContext ui({.viewport = {800, 600}});
    const u32 panel = ui.registry().find("panel"), label = ui.registry().find("label");
    UI_CHECK(ui.addStyleSheet(R"css(
panel { width: 1px; }
.a .deep { width: 10px; }
.p > panel > .two { width: 20px; }
.s + panel .sub { width: 30px; }
.q ~ panel { height: 7px; }
panel:has(> .hc) { height: 40px; }
panel:has(+ .nc) { height: 50px; }
panel:has(.anyc) { opacity: 0.5; }
panel.x[data-on] .attr { width: 60px; }
panel:hover .hov { width: 70px; }
.u { min-width: 3px; }
)css").has_value());
    auto W = [&](NodeHandle n) { return ui.computed(n).width.value; };
    // a chain of 6 panels with a leaf
    std::vector<NodeHandle> chain{ui.create(panel, ui.root())};
    for (int i = 0; i < 6; ++i) chain.push_back(ui.create(panel, chain.back()));
    const NodeHandle leaf = chain.back();
    ui.addClass(leaf, "deep");
    ui.update(0.f);
    UI_CHECK_EQ(W(leaf), 1.f);
    // class on the top: a leaf six levels down restyles (`.a .deep`)
    ui.addClass(chain[0], "a");
    ui.update(0.f);
    UI_CHECK_EQ(W(leaf), 10.f);
    ui.removeClass(chain[0], "a");
    ui.update(0.f);
    UI_CHECK_EQ(W(leaf), 1.f);
    ui.setAttribute(chain[1], "class", "a");
    ui.update(0.f);
    UI_CHECK_EQ(W(leaf), 10.f);
    ui.removeAttribute(chain[1], "class");
    ui.update(0.f);
    UI_CHECK_EQ(W(leaf), 1.f);
    // `.p > panel > .two`: exactly two levels
    ui.addClass(chain[3], "two");
    ui.update(0.f);
    ui.addClass(chain[1], "p");
    ui.update(0.f);
    UI_CHECK_EQ(W(chain[3]), 20.f);
    ui.removeClass(chain[1], "p");
    ui.update(0.f);
    UI_CHECK_EQ(W(chain[3]), 1.f);
    // attribute + class compound reaching a deep descendant
    ui.addClass(leaf, "attr");
    ui.addClass(chain[2], "x");
    ui.update(0.f);
    UI_CHECK_EQ(W(leaf), 1.f);
    ui.setAttribute(chain[2], "data-on", "1");
    ui.update(0.f);
    UI_CHECK_EQ(W(leaf), 60.f);
    ui.removeAttribute(chain[2], "data-on");
    ui.update(0.f);
    UI_CHECK_EQ(W(leaf), 1.f);
    ui.removeClass(leaf, "attr");
    // state on an ancestor (`panel:hover .hov`)
    ui.addClass(leaf, "hov");
    ui.update(0.f);
    ui.setState(chain[0], kStateHover, true);
    ui.update(0.f);
    UI_CHECK_EQ(W(leaf), 70.f);
    ui.setState(chain[0], kStateHover, false);
    ui.update(0.f);
    UI_CHECK_EQ(W(leaf), 1.f);
    ui.removeClass(leaf, "hov");
    // `.s + panel .sub`: the next sibling's subtree
    {
        const NodeHandle row = ui.create(panel, ui.root());
        const NodeHandle s1 = ui.create(panel, row), s2 = ui.create(panel, row);
        const NodeHandle mid = ui.create(panel, s2), sub = ui.create(panel, mid);
        ui.addClass(sub, "sub");
        const NodeHandle s3 = ui.create(panel, row);
        ui.update(0.f);
        UI_CHECK_EQ(W(sub), 1.f);
        ui.addClass(s1, "s");
        ui.update(0.f);
        UI_CHECK_EQ(W(sub), 30.f);
        ui.removeClass(s1, "s");
        ui.update(0.f);
        UI_CHECK_EQ(W(sub), 1.f);
        ui.addClass(s1, "q");
        ui.update(0.f);
        UI_CHECK(ui.computed(s3).height.value == 7.f && ui.computed(s2).height.value == 7.f);
        ui.destroy(row);
    }
    // a class used only as a subject restyles only the node (a leaf: nothing inherits), even with :has() rules around
    ui.update(0.f);
    ui.addClass(leaf, "u");
    ui.update(0.f);
    UI_CHECK_EQ(ui.stats().styled, 1u);
    UI_CHECK_EQ(ui.computed(leaf).minWidth.value, 3.f);
    ui.removeClass(leaf, "u");
    ui.update(0.f);
    UI_CHECK_EQ(ui.stats().styled, 1u);
    // :has(> .hc): only the parent becomes an anchor
    ui.addClass(chain[4], "hc");
    ui.update(0.f);
    UI_CHECK(ui.computed(chain[3]).height.value == 40.f && ui.computed(chain[2]).height.value != 40.f);
    UI_CHECK(ui.stats().styled <= 3u); // the node and its parent (+ nothing deeper)
    ui.removeClass(chain[4], "hc");
    ui.update(0.f);
    UI_CHECK(ui.computed(chain[3]).height.value != 40.f);
    // :has(+ .nc): only the previous sibling
    {
        const NodeHandle row = ui.create(panel, ui.root());
        const NodeHandle a = ui.create(panel, row), b = ui.create(panel, row), c = ui.create(panel, row);
        ui.update(0.f);
        ui.addClass(c, "nc");
        ui.update(0.f);
        UI_CHECK(ui.computed(b).height.value == 50.f && ui.computed(a).height.value != 50.f);
        UI_CHECK(ui.stats().styled <= 3u); // the node and earlier siblings only (not the parent)
        // removing c: b stops matching
        ui.destroy(c);
        ui.update(0.f);
        UI_CHECK(ui.computed(b).height.value != 50.f);
        ui.destroy(row);
    }
    // :has(.anyc): every ancestor
    ui.addClass(leaf, "anyc");
    ui.update(0.f);
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) UI_CHECK_EQ(ui.computed(chain[i]).opacity, 0.5f);
    UI_CHECK(ui.computed(leaf).opacity == 1.f);
    ui.removeClass(leaf, "anyc");
    ui.update(0.f);
    UI_CHECK_EQ(ui.computed(chain[0]).opacity, 1.f);
    // moving a subtree under `.a` restyles all of it
    {
        const NodeHandle host = ui.create(panel, ui.root());
        ui.addClass(host, "a");
        const NodeHandle t = ui.create(panel, ui.root());
        const NodeHandle t1 = ui.create(panel, t), t2 = ui.create(panel, t1);
        ui.addClass(t2, "deep");
        ui.update(0.f);
        UI_CHECK_EQ(W(t2), 1.f);
        UI_CHECK(!ui.insertBefore(t, NodeHandle{})); // invalid reference: unchanged
        ui.moveTo(t, host);
        ui.update(0.f);
        UI_CHECK_EQ(W(t2), 10.f);
    }
    {   // precise `+`: a class change reaches only the next sibling (`.m + x`) / next two (`.m2 + x + x`), not all later ones
        UiContext u2({.viewport = {800, 600}});
        const u32 pn = u2.registry().find("panel");
        UI_CHECK(u2.addStyleSheet(".m + panel { width: 5px; } .m2 + panel + panel { width: 6px; }").has_value());
        const NodeHandle host = u2.create(pn, u2.root());
        std::vector<NodeHandle> row;
        for (int i = 0; i < 20; ++i) row.push_back(u2.create(pn, host));
        u2.update(0.f);
        u2.addClass(row[0], "m");
        u2.update(0.f);
        UI_CHECK_EQ(u2.computed(row[1]).width.value, 5.f);
        UI_CHECK(u2.stats().styled <= 2u); // the node and its next sibling, not the other 18
        u2.addClass(row[5], "m2");
        u2.update(0.f);
        UI_CHECK_EQ(u2.computed(row[7]).width.value, 6.f);
        UI_CHECK(u2.stats().styled <= 3u);
        u2.removeClass(row[0], "m");
        u2.update(0.f);
        UI_CHECK(u2.computed(row[1]).width.value != 5.f);
    }
    return finish("fuse_ui_style_invalidation");
}
