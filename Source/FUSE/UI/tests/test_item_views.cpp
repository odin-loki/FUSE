// fuse_ui_item_views: QTreeView scrolling, QAbstractItemView selection modes / behaviours on the tree and table views,
// multi-row drags, drag image + cursor feedback, drag hand-off to the OS (setExternalDragHandler), auto-scroll and
// auto-expand while hovering a drag.
#include "test_util.hpp"

#include <cstdio>
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
void click(UiContext& ui, Vec2 p, u8 mods = 0) {
    ui.setModifiers(mods);
    ui.pointerDown(p);
    ui.pointerUp(p);
    ui.setModifiers(0);
    ui.update(1.f / 60.f);
}
void key(UiContext& ui, Key k, u8 mods = 0) {
    ui.setModifiers(mods);
    ui.keyDown(k, mods);
    ui.setModifiers(0);
    ui.update(1.f / 60.f);
}
void drag(UiContext& ui, Vec2 a, Vec2 b, bool release = true) {
    ui.pointerDown(a);
    ui.pointerMove({a.x + 3.f, a.y + 3.f});
    ui.pointerMove({(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f + 12.f});
    ui.pointerMove(b);
    if (release) ui.pointerUp(b);
    ui.update(1.f / 60.f);
}
std::string lines(std::vector<u32> v) {
    std::string s;
    for (u32 x : v) s += std::to_string(x) + ",";
    return s;
}
} // namespace

int main() {
    UiContext ui({.viewport = {800, 600}, .font = &fuse::ui::test::font()});
    w::install(ui);
    const NodeHandle col = w::panel(ui, ui.root());
    ui.setCss(col, "width", "600px");

    // ---- tree scrolling ----
    std::string big;
    for (int i = 0; i < 40; ++i) big += (i ? "\n" : "") + std::string("item ") + std::to_string(i);
    const NodeHandle tree = w::treeView(ui, col, big, "big");
    ui.setCss(tree, "height", "120px");
    ui.update(1.f / 60.f);
    ui.setFocus(tree);
    UI_CHECK(w::treeFirstVisible(ui, tree) == 0u);
    ui.wheel(mid(ui.rect(tree)), {0.f, -2.f});
    ui.update(1.f / 60.f);
    UI_CHECK(w::treeFirstVisible(ui, tree) == 6u);
    UI_CHECK(partRect(ui, tree, A11yRole::GridCell, "item 6").h > 0.f && partRect(ui, tree, A11yRole::GridCell, "item 0").h == 0.f);
    key(ui, Key::End);
    UI_CHECK(ui.value(tree) == 39.f && w::treeFirstVisible(ui, tree) > 30u);
    key(ui, Key::Home);
    UI_CHECK(ui.value(tree) == 0.f && w::treeFirstVisible(ui, tree) == 0u);
    for (int i = 0; i < 12; ++i) key(ui, Key::Down);
    UI_CHECK(ui.value(tree) == 12.f && w::treeFirstVisible(ui, tree) > 0u);
    UI_CHECK(partRect(ui, tree, A11yRole::GridCell, "item 12").h > 0.f);
    { // scrollbar thumb drag to the bottom
        const Rect r = ui.rect(tree);
        ui.pointerDown({r.right() - 4.f, r.y + 6.f});
        ui.pointerMove({r.right() - 4.f, r.bottom() + 40.f});
        ui.pointerUp({r.right() - 4.f, r.bottom() + 40.f});
        ui.update(1.f / 60.f);
        UI_CHECK(partRect(ui, tree, A11yRole::GridCell, "item 39").h > 0.f);
    }
    w::setTreeFirstVisible(ui, tree, 0);
    ui.update(1.f / 60.f);
    UI_CHECK(w::treeFirstVisible(ui, tree) == 0u);

    // ---- tree selection modes ----
    int selChanges = 0;
    ui.connect(tree, SignalKind::SelectionChanged, [&](UiContext&, NodeHandle, const SignalArgs&) { ++selChanges; });
    auto cell = [&](NodeHandle v, std::string_view n) { return mid(partRect(ui, v, A11yRole::GridCell, n)); };
    click(ui, cell(tree, "item 2"));
    UI_CHECK(lines(w::selectedRows(ui, tree)) == "2," && selChanges == 1); // Single: current = selection
    w::setItemSelectionMode(ui, tree, w::SelectionMode::Extended);
    UI_CHECK(w::itemSelectionMode(ui, tree) == w::SelectionMode::Extended && lines(w::selectedRows(ui, tree)) == "2,");
    click(ui, cell(tree, "item 5"), kModShift);
    UI_CHECK_EQ(lines(w::selectedRows(ui, tree)), std::string("2,3,4,5,"));
    click(ui, cell(tree, "item 3"), kModCtrl);
    UI_CHECK_EQ(lines(w::selectedRows(ui, tree)), std::string("2,4,5,"));
    click(ui, cell(tree, "item 1"));
    UI_CHECK_EQ(lines(w::selectedRows(ui, tree)), std::string("1,"));
    key(ui, Key::Down, kModShift);
    key(ui, Key::Down, kModShift);
    UI_CHECK_EQ(lines(w::selectedRows(ui, tree)), std::string("1,2,3,"));
    key(ui, Key::Down, kModCtrl); // move only
    UI_CHECK(ui.value(tree) == 4.f && lines(w::selectedRows(ui, tree)) == "1,2,3,");
    key(ui, Key::Space, kModCtrl);
    UI_CHECK_EQ(lines(w::selectedRows(ui, tree)), std::string("1,2,3,4,"));
    key(ui, Key::A, kModCtrl);
    UI_CHECK(w::selectedRows(ui, tree).size() == 40u);
    w::clearItemSelection(ui, tree);
    UI_CHECK(w::selectedRows(ui, tree).empty());
    w::setItemSelectionMode(ui, tree, w::SelectionMode::Multi);
    click(ui, cell(tree, "item 1"));
    click(ui, cell(tree, "item 3"));
    UI_CHECK_EQ(lines(w::selectedRows(ui, tree)), std::string("1,3,"));
    click(ui, cell(tree, "item 1"));
    UI_CHECK_EQ(lines(w::selectedRows(ui, tree)), std::string("3,"));
    w::setRowSelected(ui, tree, 7, true);
    UI_CHECK(w::itemSelected(ui, tree, 7) && w::itemSelected(ui, tree, 3) && !w::itemSelected(ui, tree, 1));
    { // the a11y rows mirror the selection
        std::vector<A11ySnapshotNode> v;
        ui.snapshotA11y(v);
        bool s3 = false, s1 = true;
        for (const auto& n : v) if (n.node == tree && n.role == A11yRole::Row) {
            if (n.name == "item 3") s3 = n.state.selected;
            if (n.name == "item 1") s1 = n.state.selected;
        }
        UI_CHECK(s3 && !s1);
    }
    w::setItemSelectionMode(ui, tree, w::SelectionMode::Contiguous);
    click(ui, cell(tree, "item 2"));
    click(ui, cell(tree, "item 4"), kModShift);
    click(ui, cell(tree, "item 5"), kModCtrl | kModShift); // no Ctrl toggling in Contiguous: a fresh range
    UI_CHECK_EQ(lines(w::selectedRows(ui, tree)), std::string("2,3,4,5,"));
    w::setItemSelectionMode(ui, tree, w::SelectionMode::None);
    click(ui, cell(tree, "item 2"));
    UI_CHECK(w::selectedRows(ui, tree).empty());

    // ---- table selection behaviours ----
    static w::StringTableModel tm("A\tB\tC");
    for (int i = 0; i < 30; ++i) tm.addRow("a" + std::to_string(i) + "\tb" + std::to_string(i) + "\tc" + std::to_string(i));
    const NodeHandle tbl = w::tableView(ui, col, &tm.model(), "tbl");
    ui.setCss(tbl, "height", "140px");
    ui.update(1.f / 60.f);
    ui.setFocus(tbl);
    int tblChanges = 0;
    ui.connect(tbl, SignalKind::SelectionChanged, [&](UiContext&, NodeHandle, const SignalArgs&) { ++tblChanges; });
    w::setItemSelectionMode(ui, tbl, w::SelectionMode::Extended);
    w::setItemSelectionBehavior(ui, tbl, w::SelectionBehavior::Items);
    click(ui, cell(tbl, "b1"));
    UI_CHECK(w::tableCurrentColumn(ui, tbl) == 1u && w::selectedItems(ui, tbl).size() == 1u && w::itemSelected(ui, tbl, 1, 1));
    click(ui, cell(tbl, "c2"), kModShift);
    UI_CHECK(w::selectedItems(ui, tbl).size() == 4u && w::selectedRows(ui, tbl).empty());
    key(ui, Key::Left, kModShift);
    UI_CHECK(w::tableCurrentColumn(ui, tbl) == 1u && w::selectedItems(ui, tbl).size() == 2u);
    key(ui, Key::Left);
    UI_CHECK(w::tableCurrentColumn(ui, tbl) == 0u && w::itemSelected(ui, tbl, 2, 0) && w::selectedItems(ui, tbl).size() == 1u);
    w::setRowSelected(ui, tbl, 4, true);
    UI_CHECK_EQ(lines(w::selectedRows(ui, tbl)), std::string("4,"));
    w::setItemSelectionBehavior(ui, tbl, w::SelectionBehavior::Columns);
    click(ui, cell(tbl, "b3"));
    UI_CHECK(w::itemSelected(ui, tbl, 0, 1) && w::itemSelected(ui, tbl, 29, 1) && !w::itemSelected(ui, tbl, 0, 0));
    w::setItemSelectionBehavior(ui, tbl, w::SelectionBehavior::Rows);
    click(ui, cell(tbl, "a1"));
    click(ui, cell(tbl, "a3"), kModShift);
    UI_CHECK_EQ(lines(w::selectedRows(ui, tbl)), std::string("1,2,3,"));
    UI_CHECK(tblChanges > 0);

    // ---- multi-row table drag (InternalMove) + drag image + cursor ----
    w::setTableDragDrop(ui, tbl, {.drag = true, .internalMove = true, .moveRows = [&](UiContext& c, NodeHandle t, const std::vector<u32>& r, u32 to) {
        tm.moveRows(r, to); w::tableModelChanged(c, t);
    }});
    click(ui, cell(tbl, "a0"));
    click(ui, cell(tbl, "a2"), kModCtrl);
    UI_CHECK_EQ(lines(w::selectedRows(ui, tbl)), std::string("0,2,"));
    { // drag a0 + a2 below a4
        const Rect a4 = partRect(ui, tbl, A11yRole::GridCell, "a4");
        drag(ui, cell(tbl, "a2"), {a4.x + 10.f, a4.bottom() - 2.f}, false);
        UI_CHECK(w::dragActive(ui) && w::dragPayload(ui)->label == "2 rows");
        const NodeHandle img = w::dragImage(ui);
        UI_CHECK(ui.alive(img) && ui.text(img) == "2 rows");
        UI_CHECK(ui.cursorShape() == CursorShape::Move);
        ui.pointerUp({a4.x + 10.f, a4.bottom() - 2.f});
        ui.update(1.f / 60.f);
        UI_CHECK(!w::dragActive(ui) && !ui.alive(img) && ui.cursorShape() == CursorShape::Default);
    }
    UI_CHECK(tm.cell(0, 0) == "a1" && tm.cell(1, 0) == "a3" && tm.cell(2, 0) == "a4" && tm.cell(3, 0) == "a0" && tm.cell(4, 0) == "a2");
    UI_CHECK_EQ(lines(w::selectedRows(ui, tbl)), std::string("3,4,"));
    // a click (no drag) on a selected row narrows the selection on release
    click(ui, cell(tbl, "a0"));
    UI_CHECK_EQ(lines(w::selectedRows(ui, tbl)), std::string("3,"));
    { // refused drop shows the forbidden cursor
        const NodeHandle no = w::panel(ui, col);
        ui.setCss(no, "height", "30px");
        w::setDropTarget(ui, no, [](UiContext&, NodeHandle, const w::DropEvent&) -> u8 { return w::DropIgnore; });
        ui.update(1.f / 60.f);
        drag(ui, cell(tbl, "a0"), mid(ui.rect(no)), false);
        UI_CHECK(w::dragActive(ui) && ui.cursorShape() == CursorShape::Forbidden && ui.hasClass(w::dragImage(ui), "drag-refused"));
        ui.keyDown(Key::Escape, 0);
        ui.pointerUp(mid(ui.rect(no)));
        ui.update(1.f / 60.f);
        UI_CHECK(!w::dragActive(ui) && ui.cursorShape() == CursorShape::Default);
    }
    // ---- auto-scroll while hovering the bottom edge ----
    {
        const u32 before = ui.value(tbl) >= 0.f ? 0u : 0u;
        (void)before;
        const Rect r = ui.rect(tbl);
        std::vector<A11ySnapshotNode> v;
        auto firstRow = [&] {
            v.clear();
            ui.snapshotA11y(v);
            for (const auto& n : v) if (n.node == tbl && n.role == A11yRole::Row && n.part != 1) return n.rowIndex;
            return -1;
        };
        const i32 f0 = firstRow();
        drag(ui, cell(tbl, "a0"), {r.x + 40.f, r.bottom() - 6.f}, false);
        for (int i = 0; i < 20; ++i) ui.update(1.f / 60.f);
        UI_CHECK(firstRow() > f0);
        ui.pointerMove({r.x + 40.f, r.y + r.h * 0.5f}); // out of the margin: stops
        const i32 f1 = firstRow();
        for (int i = 0; i < 20; ++i) ui.update(1.f / 60.f);
        UI_CHECK(firstRow() == f1);
        ui.keyDown(Key::Escape, 0);
        ui.pointerUp({r.x + 40.f, r.y + r.h * 0.5f});
        ui.update(1.f / 60.f);
    }

    // ---- tree: multi-row drag + auto-expand ----
    const NodeHandle t2 = w::treeView(ui, col, "a\n\ta1\nb\n\tb1\nc\nd", "t2");
    ui.setCss(t2, "height", "160px");
    w::setTreeExpanded(ui, t2, 2, false); // b collapsed
    w::setTreeDragDrop(ui, t2, {.internalMove = true, .autoExpandDelay = 0.2f});
    w::setItemSelectionMode(ui, t2, w::SelectionMode::Extended);
    ui.update(1.f / 60.f);
    click(ui, cell(t2, "c"));
    click(ui, cell(t2, "d"), kModCtrl);
    {
        const Rect b = partRect(ui, t2, A11yRole::GridCell, "b");
        drag(ui, cell(t2, "c"), mid(b), false); // hover onto collapsed b
        UI_CHECK(!w::treeExpanded(ui, t2, 2));
        for (int i = 0; i < 20; ++i) ui.update(1.f / 60.f);
        UI_CHECK(w::treeExpanded(ui, t2, 2)); // auto-expanded after the delay
        ui.pointerUp(mid(b));
        ui.update(1.f / 60.f);
    }
    UI_CHECK_EQ(std::string(ui.text(t2)), std::string("a\n\ta1\nb\n\tb1\n\tc\n\td"));
    UI_CHECK_EQ(lines(w::selectedRows(ui, t2)), std::string("4,5,"));

    // ---- OS drag hand-off ----
    std::string handed;
    int extStarts = 0;
    w::setExternalDragHandler(ui, [&](UiContext&, const w::DragPayload& p) { ++extStarts; handed = std::string(p.get("text/plain")); return true; });
    u8 doneAction = 0xFF;
    const NodeHandle srcp = w::panel(ui, col);
    ui.setCss(srcp, "height", "30px");
    ui.update(1.f / 60.f);
    w::setDragSource(ui, srcp, [](UiContext&, NodeHandle, Vec2, w::DragPayload& p) { p.set("text/plain", "out"); return true; },
                     [&](UiContext&, NodeHandle, u8 a) { doneAction = a; });
    {
        const Vec2 a = mid(ui.rect(srcp));
        ui.pointerDown(a);
        ui.pointerMove({a.x + 20.f, a.y});
        UI_CHECK(w::dragActive(ui) && !w::externalDragActive(ui));
        ui.pointerMove({-5.f, a.y}); // leaves the window
        UI_CHECK(extStarts == 1 && handed == "out" && w::externalDragActive(ui) && !ui.alive(w::dragImage(ui)));
        ui.pointerUp({-5.f, a.y}); // the OS owns it now
        UI_CHECK(w::dragActive(ui) && doneAction == 0xFF);
        w::finishExternalDrag(ui, w::DropCopy);
        UI_CHECK(!w::dragActive(ui) && doneAction == w::DropCopy && ui.cursorShape() == CursorShape::Default);
    }
    w::setExternalDragHandler(ui, {});
    {
        const Vec2 a = mid(ui.rect(srcp));
        ui.pointerDown(a);
        ui.pointerMove({a.x + 20.f, a.y});
        ui.pointerMove({-5.f, a.y});
        UI_CHECK(w::dragActive(ui) && !w::externalDragActive(ui));
        ui.pointerUp({-5.f, a.y});
        UI_CHECK(!w::dragActive(ui) && doneAction == w::DropIgnore);
    }

    // ---- rubber band + horizontal scroll ----
    {
        static w::StringTableModel hm("A\tB\tC");
        for (int i = 0; i < 4; ++i) hm.addRow("h" + std::to_string(i) + "\tm" + std::to_string(i) + "\tz" + std::to_string(i));
        const NodeHandle ht = w::tableView(ui, col, &hm.model(), "ht");
        ui.setCss(ht, "height", "200px");
        ui.setCss(ht, "width", "200px");
        w::setItemSelectionMode(ui, ht, w::SelectionMode::Extended);
        ui.update(1.f / 60.f);
        const Rect r = ui.contentRect(ht), h1 = partRect(ui, ht, A11yRole::GridCell, "h1"), h3 = partRect(ui, ht, A11yRole::GridCell, "h3");
        // band from the empty space below the rows up into h1..h3
        const Vec2 a{r.x + 30.f, h3.bottom() + 20.f};
        UI_CHECK(a.y < r.bottom() - 10.f);
        ui.pointerDown(a);
        ui.pointerMove({a.x + 5.f, h3.y + 3.f});
        ui.pointerMove({a.x + 10.f, h1.y + 3.f});
        UI_CHECK_EQ(lines(w::selectedRows(ui, ht)), std::string("1,2,3,"));
        ui.pointerUp({a.x + 10.f, h1.y + 3.f});
        ui.update(1.f / 60.f);
        // Ctrl band adds row 0's cell-free strip; plain band elsewhere replaces
        ui.setModifiers(kModCtrl);
        ui.pointerDown(a);
        ui.setModifiers(0);
        ui.pointerMove({a.x, partRect(ui, ht, A11yRole::GridCell, "h0").y + 2.f});
        UI_CHECK_EQ(lines(w::selectedRows(ui, ht)), std::string("0,1,2,3,"));
        ui.pointerUp({a.x, h1.y});
        click(ui, a);
        UI_CHECK(w::selectedRows(ui, ht).empty());
        // shift+wheel / horizontal wheel scroll sideways; the bar costs a row's worth of height only if needed
        const f32 z0 = partRect(ui, ht, A11yRole::GridCell, "z0").x;
        ui.setModifiers(kModShift);
        ui.wheel(mid(ui.rect(ht)), {0.f, -2.f});
        ui.setModifiers(0);
        ui.update(1.f / 60.f);
        const f32 z1 = partRect(ui, ht, A11yRole::GridCell, "z0").x;
        UI_CHECK(z1 < z0 && w::tableFirstVisible(ui, ht) == 0u);
        ui.wheel(mid(ui.rect(ht)), {4.f, 0.f}); // back to the start
        ui.update(1.f / 60.f);
        UI_CHECK(partRect(ui, ht, A11yRole::GridCell, "z0").x == z0);
        // keyboard: the current cell is kept in view
        w::setItemSelectionBehavior(ui, ht, w::SelectionBehavior::Items);
        ui.setFocus(ht);
        click(ui, cell(ht, "h0"));
        key(ui, Key::Right);
        key(ui, Key::Right);
        ui.update(1.f / 60.f);
        const Rect z = partRect(ui, ht, A11yRole::GridCell, "z0");
        UI_CHECK(w::tableCurrentColumn(ui, ht) == 2u && z.right() <= r.right() + 0.5f && z.x >= r.x - 0.5f);
        key(ui, Key::Home);
        ui.update(1.f / 60.f);
        UI_CHECK(partRect(ui, ht, A11yRole::GridCell, "h0").x == r.x);
    }
    // ---- h-scroll re-clamped when columns shrink ----
    {
        const NodeHandle ht = ui.find("ht");
        UI_CHECK(ui.alive(ht));
        const Rect r = ui.contentRect(ht);
        ui.wheel(mid(ui.rect(ht)), {-20.f, 0.f}); // fully right
        ui.update(1.f / 60.f);
        UI_CHECK(partRect(ui, ht, A11yRole::GridCell, "h0").x < r.x);
        for (u32 k = 0; k < 3; ++k) w::setTableColumnWidth(ui, ht, k, 50.f); // 150 px fits: no h-scroll left
        ui.update(1.f / 60.f);
        UI_CHECK(partRect(ui, ht, A11yRole::GridCell, "h0").x == r.x);
    }
    // ---- tree rubber band + Shift+PageDown in Multi ----
    {
        const NodeHandle tb = w::treeView(ui, col, "r0\nr1\n\tr1a\nr2\nr3", "tband");
        ui.setCss(tb, "height", "200px");
        w::setItemSelectionMode(ui, tb, w::SelectionMode::Extended);
        ui.update(1.f / 60.f);
        const Rect r1 = partRect(ui, tb, A11yRole::GridCell, "r1"), r3 = partRect(ui, tb, A11yRole::GridCell, "r3");
        const Vec2 a{r3.x + 10.f, r3.bottom() + 8.f};
        UI_CHECK(a.y < ui.contentRect(tb).bottom());
        ui.pointerDown(a);
        ui.pointerMove({a.x + 3.f, r3.y + 2.f});
        ui.pointerMove({a.x + 6.f, r1.y + 2.f});
        UI_CHECK_EQ(lines(w::selectedRows(ui, tb)), std::string("1,2,3,4,"));
        ui.pointerUp({a.x + 6.f, r1.y + 2.f});
        ui.update(1.f / 60.f);
        w::setItemSelectionMode(ui, tb, w::SelectionMode::Multi);
        w::clearItemSelection(ui, tb);
        ui.setFocus(tb);
        click(ui, cell(tb, "r0")); // toggles r0 on, anchor r0
        key(ui, Key::PageDown, kModShift);
        UI_CHECK_EQ(lines(w::selectedRows(ui, tb)), std::string("0,1,2,3,4,"));
        key(ui, Key::Up); // Multi: plain arrows move only
        UI_CHECK_EQ(lines(w::selectedRows(ui, tb)), std::string("0,1,2,3,4,"));
    }
    // ---- pixmap drag image at pos - hotspot ----
    {
        const NodeHandle ps = w::panel(ui, col);
        ui.setCss(ps, "height", "30px");
        ui.update(1.f / 60.f);
        w::setDragSource(ui, ps, [](UiContext&, NodeHandle, Vec2, w::DragPayload& p) {
            p.set("text/plain", "pix"); p.imageIndex = 3; p.imageW = 32.f; p.imageH = 24.f; p.hotX = 8.f; p.hotY = 6.f; return true; });
        const Vec2 a = mid(ui.rect(ps));
        ui.pointerDown(a);
        ui.pointerMove({a.x + 20.f, a.y});
        ui.pointerMove({a.x + 40.f, a.y + 5.f});
        ui.update(1.f / 60.f);
        const NodeHandle img = w::dragImage(ui);
        UI_CHECK(ui.alive(img) && ui.hasClass(img, "drag-image"));
        const Rect ir = ui.rect(img);
        UI_CHECK(std::abs(ir.x - (a.x + 40.f - 8.f)) < 1.f && std::abs(ir.y - (a.y + 5.f - 6.f)) < 1.f && std::abs(ir.w - 32.f) < 1.f && std::abs(ir.h - 24.f) < 1.f);
        w::cancelDrag(ui);
        ui.pointerUp({a.x + 40.f, a.y + 5.f});
        ui.update(1.f / 60.f);
        UI_CHECK(!ui.alive(w::dragImage(ui)) && !w::dragActive(ui));
    }
    return fuse::ui::test::finish("fuse_ui_item_views");
}
