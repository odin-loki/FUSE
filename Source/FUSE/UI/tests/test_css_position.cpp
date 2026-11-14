// Gate fuse_ui_css_position: CSS positioning on the layout engine.
//   * position: relative (moved by its insets, the flow keeps its slot), absolute (out of flow; against the nearest
//     positioned ancestor, else the viewport; insets, stretch between opposite insets, percentages), fixed (against
//     the viewport, painted over everything, unaffected by scrolling and ancestor clips)
//   * position: sticky under scroll: stays at its inset in the scrollport, inside its parent's content box
//   * z-index: paint order and hit testing among siblings (negative, auto, positive; positioned over in-flow)
//   * aspect-ratio (one side given, stretched cross size, absolute boxes), grid-template-columns: subgrid
//   * the layout stays allocation free in steady state with positioned nodes
// (Adapted from the gui-framework gate of the same name: its flex-wrap / align-content, row / column gap, grid
// masonry and mask-texture parts are not ported.)
#include "test_util.hpp"

#include <cmath>
#include <cstdio>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
NodeHandle make(UiContext& ui, NodeHandle p, std::string_view cls, std::string_view id = {}) {
    const NodeHandle n = ui.create(ui.registry().find("panel"), p, id);
    if (!cls.empty()) ui.addClass(n, cls);
    return n;
}
u32 pixel(UiContext& ui, int x, int y, int w, int h) {
    Image img;
    img.resize(w, h, 0xFF000000u);
    CpuRasterizer().render(ui.drawList(), img);
    return img.at(x, y) & 0xFFFFFFu;
}
} // namespace

int main() {
    // ---- parsing ----
    {
        PropValue v;
        UI_CHECK(parsePropValue(Prop::Position, "sticky", v) && v.e == u8(PositionKind::Sticky));
        UI_CHECK(!parsePropValue(Prop::Position, "floating", v));
        UI_CHECK(parsePropValue(Prop::ZIndex, "-3", v) && v.num == -3.f && parsePropValue(Prop::ZIndex, "auto", v) && v.e == 1);
        UI_CHECK(!parsePropValue(Prop::ZIndex, "1.5", v) && !parsePropValue(Prop::ZIndex, "top", v));
        UI_CHECK(parsePropValue(Prop::AspectRatio, "16 / 9", v) && std::fabs(v.num - 16.f / 9.f) < 1e-4f);
        UI_CHECK(parsePropValue(Prop::AspectRatio, "2", v) && v.num == 2.f && parsePropValue(Prop::AspectRatio, "auto", v) && v.num == 0.f);
        UI_CHECK(!parsePropValue(Prop::AspectRatio, "0", v) && !parsePropValue(Prop::AspectRatio, "1 / 0", v) && !parsePropValue(Prop::AspectRatio, "wide", v));
        auto s = StyleSheet::parse(".a { inset: 1px 2px 3px 4px; }");
        UI_CHECK(s && s->rules()[0].decls.size() == 4);
    }

    // ---- relative / absolute ----
    {
        UiContext ui({.viewport = {400, 300}});
        UI_CHECK(ui.addStyleSheet(R"css(
panel { flex-shrink: 0; }
.box { width: 200px; height: 100px; padding: 10px; }
.item { width: 50px; height: 20px; }
.rel { position: relative; left: 7px; top: -3px; }
.relb { position: relative; right: 5px; bottom: 4px; }
.host { position: relative; }
.abs { position: absolute; width: 30px; height: 10px; }
.tl { left: 5px; top: 6px; }
.br { right: 5px; bottom: 6px; }
.fill { position: absolute; left: 10px; right: 20px; top: 30px; bottom: 40px; }
.pct { position: absolute; left: 50%; top: 25%; width: 10%; height: 20px; }
.free { position: absolute; width: 10px; height: 10px; }
)css").has_value());
        const NodeHandle box = make(ui, ui.root(), "box");
        const NodeHandle i1 = make(ui, box, "item"), i2 = make(ui, box, "item"), i3 = make(ui, box, "item");
        ui.addClass(i2, "rel");
        ui.update(0.f);
        UI_CHECK_RECT(ui.rect(box), 0, 0, 200, 100);
        UI_CHECK_RECT(ui.rect(i1), 10, 10, 50, 20);
        UI_CHECK_RECT(ui.rect(i2), 17, 27, 50, 20); // moved by left / top
        UI_CHECK_RECT(ui.rect(i3), 10, 50, 50, 20); // the flow keeps i2's slot
        ui.removeClass(i2, "rel");
        ui.addClass(i2, "relb");
        ui.update(0.f);
        UI_CHECK_RECT(ui.rect(i2), 5, 26, 50, 20);
        // absolute: out of flow; without a positioned ancestor the viewport is the containing block
        const NodeHandle a1 = make(ui, box, "abs");
        ui.addClass(a1, "br");
        ui.update(0.f);
        UI_CHECK_RECT(ui.rect(i3), 10, 50, 50, 20);
        UI_CHECK_RECT(ui.rect(box), 0, 0, 200, 100);
        UI_CHECK_RECT(ui.rect(a1), 400 - 5 - 30, 300 - 6 - 10, 30, 10);
        ui.setViewport({500, 320}); // follows the viewport although nothing in its parent changed
        ui.update(0.f);
        UI_CHECK_RECT(ui.rect(a1), 500 - 5 - 30, 320 - 6 - 10, 30, 10);
        // a positioned ancestor becomes the containing block (its padding box)
        ui.addClass(box, "host");
        ui.update(0.f);
        UI_CHECK_RECT(ui.rect(a1), 200 - 5 - 30, 100 - 6 - 10, 30, 10);
        const NodeHandle a2 = make(ui, i1, "abs"); // nested deeper: still against `box`
        ui.addClass(a2, "tl");
        const NodeHandle fill = make(ui, box, "fill"), pct = make(ui, box, "pct"), free = make(ui, i3, "free");
        ui.update(0.f);
        UI_CHECK_RECT(ui.rect(a2), 5, 6, 30, 10);
        UI_CHECK_RECT(ui.rect(fill), 10, 30, 200 - 10 - 20, 100 - 30 - 40); // stretched between the insets
        UI_CHECK_RECT(ui.rect(pct), 100, 25, 20, 20);
        UI_CHECK_RECT(ui.rect(free), 10, 50, 10, 10); // no insets: the start of its parent's content box
        UI_CHECK_RECT(ui.rect(i1), 10, 10, 50, 20);   // parents are not sized by absolute children
        // the containing block moves: its absolute descendants follow
        ui.setCss(box, "margin-left", "40px");
        ui.update(0.f);
        UI_CHECK_RECT(ui.rect(a2), 45, 6, 30, 10);
        UI_CHECK_RECT(ui.rect(fill), 50, 30, 170, 30);
        ui.update(0.f);
        UI_CHECK(!ui.needsUpdate());
    }

    // ---- fixed and sticky under scroll ----
    {
        UiContext ui({.viewport = {300, 200}});
        UI_CHECK(ui.addStyleSheet(R"css(
panel { flex-shrink: 0; }
.scroll { width: 300px; height: 200px; overflow: scroll; background: #101010; }
.row { height: 60px; background: #303030; }
.head { position: sticky; top: 0; height: 30px; background: #ff0000; }
.group { height: 240px; }
.ghead { position: sticky; top: 30px; height: 20px; background: #00ff00; }
.foot { position: sticky; bottom: 0; height: 20px; background: #0000ff; }
.fab { position: fixed; right: 10px; bottom: 10px; width: 40px; height: 40px; background: #ffff00; z-index: 5; }
)css").has_value());
        const NodeHandle sc = make(ui, ui.root(), "scroll");
        const NodeHandle head = make(ui, sc, "head");
        const NodeHandle r1 = make(ui, sc, "row");
        const NodeHandle group = make(ui, sc, "group");
        const NodeHandle gh = make(ui, group, "ghead");
        make(ui, group, "row");
        for (int k = 0; k < 6; ++k) make(ui, sc, "row");
        const NodeHandle foot = make(ui, sc, "foot");
        const NodeHandle fab = make(ui, r1, "fab"); // deep inside the scroller
        ui.update(0.f);
        UI_CHECK_RECT(ui.rect(head), 0, 0, 300, 30);
        UI_CHECK_RECT(ui.rect(r1), 0, 30, 300, 60);
        UI_CHECK_RECT(ui.rect(gh), 0, 90, 300, 20);
        UI_CHECK_RECT(ui.rect(fab), 250, 150, 40, 40);
        UI_CHECK_RECT(ui.rect(foot), 0, 180, 300, 20); // its flow position is below the fold: stuck at the bottom
        // scrolled: the header stays, rows move under it
        ui.scrollTo(sc, {0, 100});
        ui.update(0.f);
        UI_CHECK_RECT(ui.rect(head), 0, 0, 300, 30);
        UI_CHECK_RECT(ui.rect(r1), 0, -70, 300, 60);
        UI_CHECK_RECT(ui.rect(group), 0, -10, 300, 240);
        UI_CHECK_RECT(ui.rect(gh), 0, 30, 300, 20);  // stuck under the header, at its own inset
        UI_CHECK_RECT(ui.rect(fab), 250, 150, 40, 40); // fixed: not scrolled
        UI_CHECK_EQ(pixel(ui, 5, 5, 300, 200), 0x0000FFu);   // the header (red, R in the low byte) over the rows
        UI_CHECK_EQ(pixel(ui, 5, 35, 300, 200), 0x00FF00u);  // the group header
        UI_CHECK_EQ(pixel(ui, 270, 170, 300, 200), 0x00FFFFu); // the fixed button, outside its scrolled-away parent
        UI_CHECK(ui.hitTest({270, 170}) == fab && ui.hitTest({5, 5}) == head && ui.hitTest({5, 35}) == gh);
        // the group's header leaves with its group: it cannot stick outside its parent's content box
        ui.scrollTo(sc, {0, 310});
        ui.update(0.f);
        UI_CHECK_RECT(ui.rect(group), 0, -220, 300, 240);
        UI_CHECK_RECT(ui.rect(gh), 0, 0, 300, 20); // the bottom of the group: y = -220 + 240 - 20
        UI_CHECK_RECT(ui.rect(head), 0, 0, 300, 30);
        // at the end the footer is in the flow again
        const f32 maxScroll = ui.scrollExtent(sc).h - 200.f;
        ui.scrollTo(sc, {0, maxScroll});
        ui.update(0.f);
        UI_CHECK_RECT(ui.rect(foot), 0, 180, 300, 20);
        UI_CHECK(ui.scroll(sc).y == maxScroll && maxScroll > 300.f);
        // a new viewport moves the fixed node
        ui.setViewport({320, 240});
        ui.update(0.f);
        UI_CHECK_RECT(ui.rect(fab), 270, 190, 40, 40);
        // steady state with sticky / fixed nodes: scrolling re-arranges, an idle frame does nothing
        ui.update(0.f);
        UI_CHECK(!ui.needsUpdate());
        ui.update(0.016f);
        UI_CHECK(ui.stats().arranged == 0u && ui.stats().styled == 0u && !ui.stats().repainted);
    }

    // ---- z-index ----
    {
        UiContext ui({.viewport = {200, 200}});
        UI_CHECK(ui.addStyleSheet(R"css(
panel { flex-shrink: 0; }
.stage { layout: stack; width: 200px; height: 200px; background: #000000; }
.card { width: 100px; height: 100px; }
.r { background: #ff0000; } .g { background: #00ff00; } .b { background: #0000ff; }
.up { z-index: 2; } .down { z-index: -1; } .pos { position: relative; }
.inner { z-index: 99; width: 40px; height: 40px; background: #ffffff; }
)css").has_value());
        const NodeHandle stage = make(ui, ui.root(), "stage");
        const NodeHandle r = make(ui, stage, "card"), g = make(ui, stage, "card"), b = make(ui, stage, "card");
        ui.addClass(r, "r");
        ui.addClass(g, "g");
        ui.addClass(b, "b");
        auto top = [&] { return pixel(ui, 100, 100, 200, 200); };
        ui.update(0.f);
        UI_CHECK_EQ(top(), 0xFF0000u); // tree order: blue last
        UI_CHECK(ui.hitTest({100, 100}) == b);
        ui.addClass(r, "up");
        ui.update(0.f);
        UI_CHECK_EQ(top(), 0x0000FFu); // red on top
        UI_CHECK(ui.hitTest({100, 100}) == r);
        ui.addClass(b, "down");
        ui.removeClass(r, "up");
        ui.update(0.f);
        UI_CHECK_EQ(top(), 0x00FF00u); // blue went under: green is the last of the rest
        UI_CHECK(ui.hitTest({100, 100}) == g);
        ui.addClass(r, "pos"); // positioned, z-index auto: over the in-flow siblings
        ui.update(0.f);
        UI_CHECK_EQ(top(), 0x0000FFu);
        UI_CHECK(ui.hitTest({100, 100}) == r);
        // z-index orders siblings: a huge z-index inside a lower card does not climb over the card above
        const NodeHandle inner = make(ui, g, "inner");
        ui.update(0.f);
        UI_CHECK(ui.hitTest({60, 60}) == r);
        UI_CHECK_EQ(pixel(ui, 60, 60, 200, 200), 0x0000FFu);
        ui.removeClass(r, "pos");
        ui.update(0.f);
        UI_CHECK(ui.hitTest({60, 60}) == inner);
        UI_CHECK_EQ(pixel(ui, 60, 60, 200, 200), 0xFFFFFFu);
        ui.update(0.f);
        UI_CHECK(!ui.needsUpdate());
    }

    // ---- aspect-ratio, subgrid ----
    {
        UiContext ui({.viewport = {400, 400}});
        UI_CHECK(ui.addStyleSheet(R"css(
panel { flex-shrink: 0; }
.col { width: 300px; align-items: start; }
.w { width: 120px; aspect-ratio: 3 / 2; }
.h { height: 60px; aspect-ratio: 2; }
.s { aspect-ratio: 4; align-self: stretch; }
.row { flex-direction: row; height: 50px; width: 300px; }
.rs { aspect-ratio: 2; }
.host { position: relative; width: 200px; height: 200px; }
.abs { position: absolute; left: 10px; top: 10px; width: 50px; aspect-ratio: 0.5; }
.grid { layout: grid; grid-template-columns: 50px 100px 1fr; gap: 10px; width: 400px; }
.sub { layout: grid; grid-template-columns: subgrid; grid-column-span: 2; }
.cell { height: 20px; }
.tile { aspect-ratio: 2; }
)css").has_value());
        const NodeHandle col = make(ui, ui.root(), "col");
        const NodeHandle w = make(ui, col, "w"), h = make(ui, col, "h"), s = make(ui, col, "s");
        const NodeHandle row = make(ui, ui.root(), "row"), rs = make(ui, row, "rs");
        const NodeHandle host = make(ui, ui.root(), "host"), abs = make(ui, host, "abs");
        const NodeHandle grid = make(ui, ui.root(), "grid");
        const NodeHandle c0 = make(ui, grid, "cell"), sub = make(ui, grid, "sub");
        const NodeHandle s0 = make(ui, sub, "cell"), s1 = make(ui, sub, "cell");
        const NodeHandle tile = make(ui, grid, "tile");
        ui.update(0.f);
        UI_CHECK_RECT(ui.rect(w), 0, 0, 120, 80);
        UI_CHECK_RECT(ui.rect(h), 0, 80, 120, 60);
        UI_CHECK_RECT(ui.rect(s), 0, 140, 300, 75); // stretched across: the height follows
        UI_CHECK_RECT(ui.rect(rs), 0, ui.rect(row).y, 100, 50); // a row: the stretched height sets the width
        UI_CHECK_RECT(ui.rect(abs), ui.rect(host).x + 10, ui.rect(host).y + 10, 50, 100);
        // subgrid: the nested grid's columns are the parent's second and third track
        const f32 gy = ui.rect(grid).y;
        UI_CHECK_RECT(ui.rect(c0), 0, gy, 50, 20);
        UI_CHECK_RECT(ui.rect(sub), 60, gy, 340, 20);
        UI_CHECK_RECT(ui.rect(s0), 60, gy, 100, 20);
        UI_CHECK_RECT(ui.rect(s1), 170, gy, 230, 20);
        UI_CHECK_RECT(ui.rect(tile), 0, gy + 30, 50, 25); // a grid cell with aspect-ratio
        // the parent's tracks change: the subgrid follows
        ui.setCss(grid, "grid-template-columns", "80px 60px 1fr");
        ui.update(0.f);
        UI_CHECK_RECT(ui.rect(s0), 90, gy, 60, 20);
        UI_CHECK_RECT(ui.rect(s1), 160, gy, 240, 20);
    }
    return finish("fuse_ui_css_position");
}
