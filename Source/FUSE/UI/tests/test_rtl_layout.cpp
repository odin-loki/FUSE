// Gate fuse_ui_rtl_layout: `direction: rtl` (plan §4.5). The property is inherited; it mirrors Box layouts along x
// (rows run from the right edge, start-aligned children of a column sit on the right), swaps left / right of Anchor
// children, resolves padding-start / padding-end to the right / left padding and flips text-align start / end. A
// subtree can switch back with `direction: ltr`; overlay popups keep their screen position. Checked with layout dumps
// of the same tree in both directions.
#include "test_util.hpp"

#include <cmath>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
NodeHandle box(UiContext& ui, NodeHandle parent, std::string_view id, std::string_view css = {}) {
    const NodeHandle n = *ui.create("panel", parent, id);
    while (!css.empty()) { // "prop: value; prop: value"
        const std::size_t semi = css.find(';');
        const std::string_view decl = css.substr(0, semi);
        if (const std::size_t colon = decl.find(':'); colon != std::string_view::npos) {
            std::string_view name = decl.substr(0, colon), value = decl.substr(colon + 1);
            while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
            while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            UI_CHECK(ui.setCss(n, name, value));
        }
        if (semi == std::string_view::npos) break;
        css.remove_prefix(semi + 1);
    }
    return n;
}
void build(UiContext& ui) {
    const NodeHandle root = ui.root();
    const NodeHandle row = box(ui, root, "row", "flex-direction: row; width: 300px; height: 50px; gap: 10px; padding-start: 20px; padding-end: 4px");
    box(ui, row, "a", "width: 50px; height: 30px");
    box(ui, row, "b", "width: 80px; height: 30px");
    box(ui, row, "c", "flex-grow: 1; height: 30px");
    const NodeHandle anchor = box(ui, root, "anchor", "layout: anchor; width: 300px; height: 60px");
    box(ui, anchor, "l", "left: 10px; width: 40px; height: 20px");
    box(ui, anchor, "r", "right: 20px; width: 40px; height: 20px");
    box(ui, anchor, "both", "left: 10px; right: 30px; top: 30px; height: 10px");
    const NodeHandle col = box(ui, root, "col", "align-items: start; height: 40px");
    box(ui, col, "s", "width: 60px; height: 20px");
    box(ui, col, "e", "width: 60px; height: 20px; align-self: end");
    const NodeHandle ltr = box(ui, root, "ltr", "direction: ltr; flex-direction: row; height: 20px; gap: 5px");
    box(ui, ltr, "x", "width: 30px; height: 20px");
    box(ui, ltr, "y", "width: 30px; height: 20px");
    const NodeHandle t = *ui.create("label", root, "t");
    ui.setText(t, "Hi");
    ui.setCss(t, "width", "200px");
    const NodeHandle te = *ui.create("label", root, "te");
    ui.setText(te, "Hi");
    ui.setCss(te, "width", "200px");
    ui.setCss(te, "text-align", "end");
}
bool near(f32 a, f32 b) { return std::abs(a - b) <= 0.01f; }
f32 firstGlyphX(const UiContext& ui, NodeHandle n) {
    f32 x = 1e9f;
    for (const DrawCmd& c : ui.drawList().cmds())
        if (c.op == DrawOp::Glyph && c.node == n.index) x = std::min(x, c.rect.x);
    return x;
}

const char* kLtr = R"(root#root 0 0 400 300
  panel#row 0 0 300 50
    panel#a 20 0 50 30
    panel#b 80 0 80 30
    panel#c 170 0 126 30
  panel#anchor 0 50 300 60
    panel#l 10 50 40 20
    panel#r 240 50 40 20
    panel#both 10 80 260 10
  panel#col 0 110 400 40
    panel#s 0 110 60 20
    panel#e 340 130 60 20
  panel#ltr 0 150 400 20
    panel#x 0 150 30 20
    panel#y 35 150 30 20
)";
const char* kRtl = R"(root#root 0 0 400 300
  panel#row 100 0 300 50
    panel#a 330 0 50 30
    panel#b 240 0 80 30
    panel#c 104 0 126 30
  panel#anchor 100 50 300 60
    panel#l 350 50 40 20
    panel#r 120 50 40 20
    panel#both 130 80 260 10
  panel#col 0 110 400 40
    panel#s 340 110 60 20
    panel#e 0 130 60 20
  panel#ltr 0 150 400 20
    panel#x 0 150 30 20
    panel#y 35 150 30 20
)";
/// The dump without the two label lines (their height depends on the font).
std::string panelsOnly(const std::string& dump) { return dump.substr(0, dump.find("  label#t")); }
} // namespace

int main() {
    Rect tl, tr, tel, ter;
    f32 gl = 0.f, gr = 0.f, gel = 0.f, ger = 0.f;
    {
        UiContext ui({.viewport = {400, 300}, .font = &font()});
        build(ui);
        ui.update(0.f);
        std::string dump;
        ui.dumpLayout(dump);
        if (panelsOnly(dump) != kLtr) std::fprintf(stderr, "LTR dump:\n%s", dump.c_str());
        UI_CHECK(panelsOnly(dump) == kLtr);
        UI_CHECK(!ui.computed(ui.find("a")).rtl);
        UI_CHECK(ui.computed(ui.find("row")).padding.l == 20.f && ui.computed(ui.find("row")).padding.r == 4.f);
        tl = ui.rect(ui.find("t"));
        tel = ui.rect(ui.find("te"));
        gl = firstGlyphX(ui, ui.find("t"));
        gel = firstGlyphX(ui, ui.find("te"));
    }
    {
        UiContext ui({.viewport = {400, 300}, .font = &font()});
        build(ui);
        UI_CHECK(ui.setCss(ui.root(), "direction", "rtl"));
        ui.update(0.f);
        std::string dump;
        ui.dumpLayout(dump);
        if (panelsOnly(dump) != kRtl) std::fprintf(stderr, "RTL dump:\n%s", dump.c_str());
        UI_CHECK(panelsOnly(dump) == kRtl);
        // inherited; a subtree switches back
        UI_CHECK(ui.computed(ui.find("a")).rtl && ui.computed(ui.find("both")).rtl);
        UI_CHECK(!ui.computed(ui.find("ltr")).rtl && !ui.computed(ui.find("y")).rtl);
        // padding-start / -end are the right / left padding now
        UI_CHECK(ui.computed(ui.find("row")).padding.r == 20.f && ui.computed(ui.find("row")).padding.l == 4.f);
        tr = ui.rect(ui.find("t"));
        ter = ui.rect(ui.find("te"));
        gr = firstGlyphX(ui, ui.find("t"));
        ger = firstGlyphX(ui, ui.find("te"));

        // switching the direction at run time re-lays the tree out; switching back restores the LTR dump
        ui.setCss(ui.root(), "direction", "ltr");
        ui.update(0.f);
        ui.dumpLayout(dump = {});
        UI_CHECK(panelsOnly(dump) == kLtr);
        UI_CHECK(near(firstGlyphX(ui, ui.find("t")), gl));
    }
    // every mirrored rect is the LTR rect reflected in its container: spot checks beyond the dumps
    UI_CHECK(tl.x == 0.f && tr.x == 200.f && tl.w == 200.f && tr.w == 200.f);
    UI_CHECK(tel.x == 0.f && ter.x == 200.f);
    // text: `start` hugs the left edge in LTR and the right edge in RTL; `end` the other way round
    UI_CHECK(gl < tl.x + 8.f);
    UI_CHECK(gr > tr.x + 150.f);
    UI_CHECK(gel > tel.x + 150.f);
    UI_CHECK(ger < ter.x + 8.f);

    // overlay popups are placed in screen coordinates: the same spot in both directions
    {
        UiContext ui({.viewport = {400, 300}, .font = &font()});
        w::install(ui);
        const NodeHandle owner = w::button(ui, ui.root(), "Menu", "owner");
        const NodeHandle s1 = w::popupMenu(ui, owner, "One\nTwo", {40.f, 60.f});
        ui.update(0.f);
        const Rect ltr = ui.rect(ui.firstChild(s1));
        w::closePopup(ui, s1);
        ui.setCss(ui.root(), "direction", "rtl");
        ui.update(0.f);
        const NodeHandle s2 = w::popupMenu(ui, owner, "One\nTwo", {40.f, 60.f});
        ui.update(0.f);
        UI_CHECK(ui.computed(ui.firstChild(s2)).rtl);
        UI_CHECK(ui.rect(ui.firstChild(s2)) == ltr);
        UI_CHECK(ltr.x == 40.f && ltr.y == 60.f);
    }
    return finish("fuse_ui_rtl_layout");
}
