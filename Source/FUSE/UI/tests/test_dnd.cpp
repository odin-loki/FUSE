// fuse_ui_dnd: in-app drag and drop parity (QDrag / QMimeData / QDropEvent): start distance, MIME payload, proposed
// action with modifiers, enter / move / leave / drop, refusal, Escape cancel; QTreeView / QTableView InternalMove with
// drop indicators, subtree moves, own-subtree refusal; table / tree drag out as text/plain to another target.
#include "test_util.hpp"

#include <string>
#include <vector>

using namespace fuse::ui;
namespace w = fuse::ui::widgets;

namespace {
Rect partRect(const UiContext& ui, NodeHandle owner, A11yRole role, std::string_view name) {
    std::vector<A11ySnapshotNode> v;
    ui.snapshotA11y(v);
    for (const auto& n : v) if (n.node == owner && n.part && n.role == role && n.name == name) return n.bounds;
    return {};
}
Vec2 mid(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
void drag(UiContext& ui, Vec2 a, Vec2 b, u8 mods = 0, bool release = true) {
    ui.setModifiers(mods);
    ui.pointerDown(a);
    ui.pointerMove({a.x + 3.f, a.y + 3.f});
    ui.pointerMove({(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f + 12.f});
    ui.pointerMove(b);
    if (release) ui.pointerUp(b);
    ui.setModifiers(0);
    ui.update(1.f / 60.f);
}
} // namespace

int main() {
    UiContext ui({.viewport = {800, 600}, .font = &fuse::ui::test::font()});
    w::install(ui);
    const NodeHandle row = w::panel(ui, ui.root());
    ui.setCss(row, "flex-direction", "row");
    const NodeHandle src = w::panel(ui, row), dst = w::panel(ui, row), other = w::panel(ui, row);
    for (NodeHandle p : {src, dst, other}) { ui.setCss(p, "width", "120px"); ui.setCss(p, "height", "60px"); }
    ui.update(1.f / 60.f);

    // generic source / target
    int starts = 0, enters = 0, leaves = 0, drops = 0;
    u8 finished = 0xFF, lastProposed = 0;
    std::string got;
    w::setDragSource(ui, src, [&](UiContext&, NodeHandle, Vec2, w::DragPayload& p) {
        ++starts; p.set("text/plain", "hello"); p.set("application/x-n", "7"); return true;
    }, [&](UiContext&, NodeHandle, u8 a) { finished = a; });
    bool refuse = false;
    w::setDropTarget(ui, dst, [&](UiContext&, NodeHandle, const w::DropEvent& e) -> u8 {
        if (e.phase == w::DropPhase::Enter) ++enters;
        if (e.phase == w::DropPhase::Leave) { ++leaves; return 0; }
        lastProposed = e.proposed;
        if (refuse || !e.payload->has("text/plain")) return w::DropIgnore;
        if (e.phase == w::DropPhase::Drop) { ++drops; got = std::string(e.payload->get("text/plain")); }
        return e.proposed;
    });
    const Vec2 ps = mid(ui.rect(src)), pd = mid(ui.rect(dst)), po = mid(ui.rect(other));
    // below the start distance: no drag
    ui.pointerDown(ps); ui.pointerMove({ps.x + 5.f, ps.y}); ui.pointerUp({ps.x + 5.f, ps.y});
    UI_CHECK(starts == 0 && !w::dragActive(ui));
    drag(ui, ps, pd);
    UI_CHECK(starts == 1 && enters == 1 && drops == 1 && got == "hello" && finished == w::DropMove && lastProposed == w::DropMove);
    drag(ui, ps, pd, kModCtrl);
    UI_CHECK(drops == 2 && finished == w::DropCopy);
    // drop outside any target: ignored
    drag(ui, ps, po);
    UI_CHECK(drops == 2 && finished == w::DropIgnore);
    // refused target: no drop
    refuse = true;
    drag(ui, ps, pd);
    UI_CHECK(drops == 2 && finished == w::DropIgnore);
    refuse = false;
    // leave + cancel
    drag(ui, ps, pd, 0, false);
    UI_CHECK(w::dragActive(ui) && w::dragTarget(ui) == dst && w::dragAction(ui) == w::DropMove && w::dragPayload(ui)->get("application/x-n") == "7");
    ui.pointerMove(po);
    UI_CHECK(leaves >= 1 && !w::dragTarget(ui).valid());
    w::cancelDrag(ui);
    UI_CHECK(!w::dragActive(ui) && finished == w::DropIgnore);
    ui.pointerUp(po);
    UI_CHECK(drops == 2);
    // allowed actions limit the proposal: Copy-only source -> Copy
    w::setDragSource(ui, src, [&](UiContext&, NodeHandle, Vec2, w::DragPayload& p) { p.set("text/plain", "c"); p.actions = w::DropCopy; return true; },
                     [&](UiContext&, NodeHandle, u8 a) { finished = a; });
    drag(ui, ps, pd);
    UI_CHECK(finished == w::DropCopy && got == "c");

    // tree InternalMove
    ui.destroy(row);
    const NodeHandle col = w::panel(ui, ui.root());
    const NodeHandle tree = w::treeView(ui, col, "a\n\ta1\n\ta2\nb\n\tb1\nc", "tree");
    ui.setCss(tree, "height", "200px");
    u32 mFrom = 99, mTo = 99;
    w::setTreeDragDrop(ui, tree, {.internalMove = true, .moved = [&](UiContext&, NodeHandle, u32 f, u32 t) { mFrom = f; mTo = t; }});
    ui.update(1.f / 60.f);
    auto rowR = [&](std::string_view n) { return partRect(ui, tree, A11yRole::Row, n); };
    // drag "c" onto "a" (middle): becomes a's last child
    drag(ui, mid(rowR("c")), mid(rowR("a")));
    UI_CHECK_EQ(std::string(ui.text(tree)), std::string("a\n\ta1\n\ta2\n\tc\nb\n\tb1"));
    UI_CHECK(mFrom == 5 && mTo == 3 && ui.value(tree) == 3.f);
    // drag "b" (with b1) above "a"
    {
        const Rect a = rowR("a");
        drag(ui, mid(rowR("b")), {a.x + a.w * 0.5f, a.y + 2.f});
    }
    UI_CHECK_EQ(std::string(ui.text(tree)), std::string("b\n\tb1\na\n\ta1\n\ta2\n\tc"));
    // drag "a" into its own child a1: refused, indicator none, text unchanged
    {
        const std::string before(ui.text(tree));
        drag(ui, mid(rowR("a")), mid(rowR("a1")), 0, false);
        Rect ind;
        UI_CHECK(w::dragActive(ui) && w::dragAction(ui) == w::DropIgnore && !w::dropIndicator(ui, tree, ind));
        ui.pointerUp(mid(rowR("a1")));
        UI_CHECK_EQ(std::string(ui.text(tree)), before);
    }
    // indicator while hovering below "b1", then Escape cancels
    {
        const Rect b1 = rowR("b1");
        ui.setFocus(tree);
        drag(ui, mid(rowR("c")), {b1.x + 40.f, b1.bottom() - 2.f}, 0, false);
        Rect ind;
        UI_CHECK(w::dropIndicator(ui, tree, ind) && ind.h <= 2.f && ind.x > b1.x + 10.f);
        ui.keyDown(Key::Escape, 0);
        UI_CHECK(!w::dragActive(ui) && !w::dropIndicator(ui, tree, ind));
        ui.pointerUp({b1.x + 40.f, b1.bottom() - 2.f});
        UI_CHECK_EQ(std::string(ui.text(tree)), std::string("b\n\tb1\na\n\ta1\n\ta2\n\tc"));
    }
    // a collapsed row keeps its state when moved: collapse "a", move it above "b"
    w::setTreeExpanded(ui, tree, 2, false);
    ui.update(1.f / 60.f);
    {
        const Rect b = rowR("b");
        drag(ui, mid(rowR("a")), {b.x + 30.f, b.y + 2.f});
    }
    UI_CHECK_EQ(std::string(ui.text(tree)), std::string("a\n\ta1\n\ta2\n\tc\nb\n\tb1"));
    UI_CHECK(!w::treeExpanded(ui, tree, 0) && w::treeExpanded(ui, tree, 4));

    // table InternalMove + drag out as text/plain
    static w::StringTableModel tm("Name\tSize");
    tm.addRow("r0\t0"); tm.addRow("r1\t1"); tm.addRow("r2\t2"); tm.addRow("r3\t3");
    const NodeHandle tbl = w::tableView(ui, col, &tm.model(), "tbl");
    const NodeHandle sink = w::panel(ui, col);
    ui.setCss(sink, "height", "40px");
    std::string sunk;
    w::setDropTarget(ui, sink, [&](UiContext&, NodeHandle, const w::DropEvent& e) -> u8 {
        if (e.phase == w::DropPhase::Drop) sunk = std::string(e.payload->get("text/plain"));
        return e.payload->has("text/plain") ? e.proposed : u8(w::DropIgnore);
    });
    w::setTableDragDrop(ui, tbl, {.drag = true, .internalMove = true, .moveRows = [&](UiContext& c, NodeHandle t, const std::vector<u32>& f, u32 to) {
        tm.moveRows(f, to); w::tableModelChanged(c, t);
    }});
    ui.update(1.f / 60.f);
    auto cellR = [&](std::string_view n) { return partRect(ui, tbl, A11yRole::GridCell, n); };
    // r0 to the gap below r2 -> r1 r2 r0 r3
    drag(ui, mid(cellR("r0")), {cellR("r2").x + 10.f, cellR("r2").bottom() - 2.f});
    UI_CHECK(tm.cell(0, 0) == "r1" && tm.cell(1, 0) == "r2" && tm.cell(2, 0) == "r0" && tm.cell(3, 0) == "r3");
    UI_CHECK(ui.value(tbl) == 2.f);
    // r3 to the top gap
    drag(ui, mid(cellR("r3")), {cellR("r1").x + 10.f, cellR("r1").y + 2.f});
    UI_CHECK(tm.cell(0, 0) == "r3" && tm.cell(1, 0) == "r1");
    // drop at its own place: no change
    drag(ui, mid(cellR("r1")), {cellR("r1").x + 10.f, cellR("r1").y + 3.f});
    UI_CHECK(tm.cell(1, 0) == "r1");
    // out to another target: row text, tab-joined
    drag(ui, mid(cellR("r2")), mid(ui.rect(sink)));
    UI_CHECK_EQ(sunk, std::string("r2\t2"));
    // header and column resize still work (no drag from the header)
    UI_CHECK(!w::dragActive(ui));
    return fuse::ui::test::finish("fuse_ui_dnd");
}
