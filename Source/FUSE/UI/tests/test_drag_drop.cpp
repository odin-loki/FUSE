// Gate fuse_ui_drag_drop: the core drag and drop (plan §4.7) and its adoption in the item views.
// Core: a press on a drag source becomes a drag past the threshold (no Click); the target is the nearest node that
// accepts the type (type list, filter, modal scope); DragEnter / DragMove / DragLeave / Drop / DragEnd order;
// `:drag-over` styling; Move vs Copy (Ctrl); a target refusing in its handler; Escape; the ghost in the draw list;
// external (OS) drops; touch: swipe pans, long press drags, a drag where nothing pans starts at the slop.
// Views: list reorder, list -> list move / copy, reorder-only rows cannot leave, table and grid report by signal.
#include "test_util.hpp"

#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
constexpr u32 kThing = dragType("application/x-test-thing");
struct Seen {
    u32 node;
    EventType type;
    DropAction action;
    u64 payload;
    std::string text;
};
std::vector<Seen> g_log;
int g_clicks = 0;
bool g_refuse = false;
int g_rowMoved = 0, g_dropped = 0, g_dragEnded = 0;
f32 g_from = -1.f, g_to = -1.f, g_dropIndex = -1.f, g_endRow = -1.f, g_endAction = -1.f;
bool g_endAccepted = false;
std::string g_dropText;

Vec2 centre(const Rect& r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
bool dragEvent(EventType t) {
    return t == EventType::DragEnter || t == EventType::DragMove || t == EventType::DragLeave || t == EventType::Drop || t == EventType::DragEnd;
}
void logSlot(UiContext& ui, NodeHandle n, const SignalArgs& a) {
    if (!a.event || !dragEvent(a.event->type)) return;
    if (g_refuse && (a.event->type == EventType::Drop || a.event->type == EventType::DragMove)) ui.setDropAction(DropAction::None);
    g_log.push_back({n.index, a.event->type, a.event->dropAction, a.event->drag ? a.event->drag->payload[0] : 0, a.event->drag ? a.event->drag->text : std::string()});
}
int count(NodeHandle n, EventType t) {
    int c = 0;
    for (const Seen& s : g_log) c += s.node == n.index && s.type == t;
    return c;
}
bool thingSource(UiContext&, NodeHandle, Vec2, DragData& out, DragImage&) {
    out.type = kThing;
    out.payload[0] = 42;
    out.text = "Item";
    return true;
}
bool onlyFortyTwo(UiContext&, NodeHandle, const DragData& d, Vec2) { return d.payload[0] == 42; }

/// Mouse drag from `a` to `b` in a few steps; releases unless `release` is false.
void drag(UiContext& ui, Vec2 a, Vec2 b, bool release = true) {
    ui.pointerMove(a);
    ui.pointerDown(a);
    ui.update(1.f / 60.f);
    for (int i = 1; i <= 4; ++i) {
        ui.pointerMove({a.x + (b.x - a.x) * f32(i) / 4.f, a.y + (b.y - a.y) * f32(i) / 4.f});
        ui.update(1.f / 60.f);
    }
    if (release) {
        ui.pointerUp(b);
        ui.update(1.f / 60.f);
    }
}

void coreGate() {
    UiContext ui({.viewport = {640, 400}, .font = &font()});
    w::install(ui);
    UI_CHECK(ui.addStyleSheet("#bin:drag-over { background: #ff0000; } .box { width: 100px; height: 60px; background: #202020; }").has_value());
    ui.setCss(ui.root(), "flex-direction", "row");
    ui.setCss(ui.root(), "gap", "20px");
    ui.setCss(ui.root(), "padding", "20px");
    ui.setCss(ui.root(), "align-items", "start");
    const NodeHandle src = w::panel(ui, ui.root(), "src"), bin = w::panel(ui, ui.root(), "bin"), other = w::panel(ui, ui.root(), "other");
    for (NodeHandle n : {src, bin, other}) {
        ui.addClass(n, "box");
        ui.connect(n, SignalKind::Event, &logSlot);
    }
    ui.connect(src, SignalKind::Clicked, [](UiContext&, NodeHandle, const SignalArgs&) { ++g_clicks; });
    ui.setDragSource(src, &thingSource);
    ui.setDropTypes(bin, {kThing, kDragFiles});
    ui.setDropTypes(other, {dragType("something/else")});
    ui.update(0.f);
    const Vec2 ps = centre(ui.rect(src)), pb = centre(ui.rect(bin)), po = centre(ui.rect(other));
    const Color idle = ui.computed(bin).background;
    const std::size_t idleCmds = ui.drawList().size();

    // a plain click still clicks
    ui.pointerMove(ps); ui.pointerDown(ps); ui.pointerUp(ps); ui.update(0.f);
    UI_CHECK_EQ(g_clicks, 1);

    // below the threshold: still a press; past it: a drag (the press ends without a Click)
    ui.pointerMove(ps); ui.pointerDown(ps);
    ui.pointerMove({ps.x + 2.f, ps.y});
    UI_CHECK(!ui.dragging() && ui.pressed() == src);
    ui.pointerMove({ps.x + 12.f, ps.y});
    UI_CHECK(ui.dragging() && !ui.pressed().valid());
    UI_CHECK(ui.dragData() && ui.dragData()->payload[0] == 42 && ui.dragData()->source == src);
    UI_CHECK(!ui.dragTarget().valid() && ui.dropAction() == DropAction::None); // the source itself accepts nothing
    ui.update(0.f);
    UI_CHECK(ui.drawList().size() > idleCmds); // the ghost chip
    // over the bin: target, :drag-over style, Enter then Move
    ui.pointerMove(pb);
    ui.update(0.f);
    UI_CHECK(ui.dragTarget() == bin && (ui.states(bin) & kStateDragOver));
    UI_CHECK(ui.dropAction() == DropAction::Move);
    UI_CHECK(ui.computed(bin).background == Color::rgb(0xFF0000));
    UI_CHECK_EQ(count(bin, EventType::DragEnter), 1);
    UI_CHECK_EQ(count(bin, EventType::DragMove), 1);
    UI_CHECK(g_log.size() >= 2 && g_log[g_log.size() - 2].type == EventType::DragEnter && g_log.back().type == EventType::DragMove);
    // over a node that takes another type: the bin is left, nothing is the target
    ui.pointerMove(po);
    ui.update(0.f);
    UI_CHECK(!ui.dragTarget().valid() && !(ui.states(bin) & kStateDragOver));
    UI_CHECK_EQ(count(bin, EventType::DragLeave), 1);
    UI_CHECK(ui.computed(bin).background == idle);
    UI_CHECK_EQ(count(other, EventType::DragEnter), 0);
    // back and drop
    ui.pointerMove(pb);
    ui.pointerUp(pb);
    ui.update(0.f);
    UI_CHECK(!ui.dragging());
    UI_CHECK_EQ(count(bin, EventType::Drop), 1);
    UI_CHECK(ui.lastDropAction() == DropAction::Move && ui.lastDropTarget() == bin);
    UI_CHECK_EQ(count(src, EventType::DragEnd), 1);
    UI_CHECK(g_log.back().node == src.index && g_log.back().type == EventType::DragEnd && g_log.back().action == DropAction::Move);
    UI_CHECK(g_log[g_log.size() - 2].type == EventType::Drop && g_log[g_log.size() - 2].payload == 42 && g_log[g_log.size() - 2].text == "Item");
    UI_CHECK(!(ui.states(bin) & kStateDragOver));
    UI_CHECK_EQ(g_clicks, 1); // the drag was not a click
    UI_CHECK(ui.computed(bin).background == idle);
    UI_CHECK_EQ(ui.drawList().size(), idleCmds); // ghost gone
    ui.update(0.f);
    UI_CHECK(!ui.needsUpdate());

    // Ctrl: copy
    g_log.clear();
    ui.setModifiers(kModCtrl);
    drag(ui, ps, pb);
    UI_CHECK(ui.lastDropAction() == DropAction::Copy);
    UI_CHECK(g_log.back().type == EventType::DragEnd && g_log.back().action == DropAction::Copy);
    ui.setModifiers(0);

    // the target refuses in its handler: no Drop result, the source hears None
    g_log.clear();
    g_refuse = true;
    drag(ui, ps, pb);
    g_refuse = false;
    UI_CHECK(ui.lastDropAction() == DropAction::None && !ui.lastDropTarget().valid());
    UI_CHECK_EQ(count(bin, EventType::Drop), 0);
    UI_CHECK(g_log.back().type == EventType::DragEnd && g_log.back().action == DropAction::None);

    // released over nothing
    g_log.clear();
    drag(ui, ps, {ps.x, ps.y + 150.f});
    UI_CHECK(ui.lastDropAction() == DropAction::None);
    UI_CHECK_EQ(count(src, EventType::DragEnd), 1);

    // Escape cancels: the target is left, the source hears None, the later release is a plain release
    g_log.clear();
    drag(ui, ps, pb, false);
    UI_CHECK(ui.dragging() && ui.dragTarget() == bin);
    UI_CHECK(ui.keyDown(Key::Escape));
    UI_CHECK(!ui.dragging() && !(ui.states(bin) & kStateDragOver));
    UI_CHECK_EQ(count(bin, EventType::DragLeave), 1);
    UI_CHECK_EQ(count(bin, EventType::Drop), 0);
    UI_CHECK(g_log.back().type == EventType::DragEnd && g_log.back().action == DropAction::None);
    ui.pointerUp(pb);
    ui.update(0.f);
    UI_CHECK_EQ(count(bin, EventType::Drop), 0);
    UI_CHECK_EQ(g_clicks, 1);

    // a filter decides per data; a modal scope blocks targets outside it
    ui.setDropTypes(other, {});
    ui.setDropFilter(other, &onlyFortyTwo);
    drag(ui, ps, po);
    UI_CHECK(ui.lastDropAction() == DropAction::Move && ui.lastDropTarget() == other);
    ui.setModal(src);
    UI_CHECK(ui.beginDrag(src, DragData{.type = kThing}));
    ui.pointerMove(pb);
    UI_CHECK(!ui.dragTarget().valid());
    ui.pointerUp(pb);
    UI_CHECK(ui.lastDropAction() == DropAction::None);
    ui.setModal({});

    // beginDrag with a node image: the ghost is a copy of the node's draw commands
    ui.pointerMove(ps);
    ui.update(0.f);
    const std::size_t before = ui.drawList().size();
    DragImage img;
    img.node = src;
    img.hotspot = {10.f, 10.f};
    UI_CHECK(ui.beginDrag(src, DragData{.type = kThing}, img));
    UI_CHECK(!ui.beginDrag(src, DragData{.type = kThing})); // one drag at a time
    ui.pointerMove({300.f, 300.f});
    ui.update(0.f);
    UI_CHECK(ui.drawList().size() > before);
    bool ghostAt = false;
    for (const DrawCmd& c : ui.drawList().cmds()) ghostAt = ghostAt || (c.node == src.index && c.rect.x == 290.f && c.rect.y == 290.f);
    UI_CHECK(ghostAt);
    UI_CHECK(ui.damage().w == 640.f && ui.damage().h == 400.f); // the overlay repaints the window
    ui.cancelDrag();
    ui.update(0.f);
    UI_CHECK_EQ(ui.drawList().size(), before);

    // external (OS) drops: hover feedback, drop, leave; a node that does not take files refuses
    g_log.clear();
    DragData files;
    files.type = kDragFiles;
    files.text = "/tmp/a.txt\n/tmp/b.txt";
    UI_CHECK(ui.dragExternalMove(pb, files));
    UI_CHECK(ui.states(bin) & kStateDragOver);
    UI_CHECK(!ui.dragExternalMove(ps, files));
    UI_CHECK(!(ui.states(bin) & kStateDragOver));
    ui.dragExternalLeave();
    UI_CHECK(!ui.dragging());
    UI_CHECK(ui.dropExternal(pb, files) == DropAction::Move);
    UI_CHECK(g_log.back().node == bin.index && g_log.back().type == EventType::Drop && g_log.back().text == "/tmp/a.txt\n/tmp/b.txt");
    UI_CHECK(ui.dropExternal(ps, files) == DropAction::None);
    UI_CHECK(!ui.dragging() && !(ui.states(bin) & kStateDragOver));
    ui.update(0.f);
    UI_CHECK(!ui.needsUpdate());
}

void touchGate() {
    UiContext ui({.viewport = {400, 600}, .font = &font()});
    w::install(ui);
    UI_CHECK(ui.addStyleSheet(".chip { height: 60px; flex-shrink: 0; background: #303030; } .box { height: 80px; background: #202020; }").has_value());
    const NodeHandle page = w::scrollArea(ui, ui.root(), "page");
    ui.setCss(page, "height", "200px");
    NodeHandle chips[6];
    for (int i = 0; i < 6; ++i) {
        chips[i] = w::panel(ui, page);
        ui.addClass(chips[i], "chip");
        ui.setDragSource(chips[i], &thingSource);
    }
    const NodeHandle bin = w::panel(ui, ui.root(), "bin");
    ui.addClass(bin, "box");
    ui.setDropTypes(bin, {kThing});
    const NodeHandle loose = w::panel(ui, ui.root(), "loose"); // a drag source with nothing to pan around it
    ui.addClass(loose, "box");
    ui.setDragSource(loose, &thingSource);
    ui.update(0.f);
    const Vec2 pc = centre(ui.rect(chips[1])), pb = centre(ui.rect(bin)), pl = centre(ui.rect(loose));

    // a swipe on a drag source inside a scroll area pans it
    double t = 1.0;
    ui.touchDown(pc, t);
    for (int i = 1; i <= 5; ++i) ui.touchMove({pc.x, pc.y - 8.f * f32(i)}, t += 0.016);
    UI_CHECK(ui.touchPanning() && !ui.dragging());
    ui.update(0.016f);
    UI_CHECK_EQ(ui.scroll(page).y, 40.f);
    ui.touchUp({pc.x, pc.y - 40.f}, t += 0.5); // at rest: no fling
    ui.update(0.016f);
    UI_CHECK(!ui.flinging());
    ui.scrollTo(page, {0.f, 0.f});
    ui.update(0.016f);

    // a long press picks the chip up; the finger then carries the drag and the release drops it
    ui.touchDown(pc, t += 1.0);
    ui.update(0.3f);
    UI_CHECK(!ui.dragging() && ui.needsUpdate()); // the long-press timer keeps the context awake
    UI_CHECK(ui.nextWakeSeconds() > 0.15f && ui.nextWakeSeconds() < 0.25f);
    ui.update(0.25f);
    UI_CHECK(ui.dragging() && !ui.pressed().valid());
    ui.touchMove({pc.x, pc.y + 30.f}, t += 0.1);
    UI_CHECK(!ui.touchPanning());
    ui.touchMove(pb, t += 0.1);
    UI_CHECK(ui.dragTarget() == bin);
    UI_CHECK_EQ(ui.scroll(page).y, 0.f);
    ui.touchUp(pb, t += 0.1);
    UI_CHECK(!ui.dragging() && ui.lastDropAction() == DropAction::Move && ui.lastDropTarget() == bin);
    ui.update(0.016f);
    UI_CHECK(!ui.needsUpdate());

    // touchCancel during a carried drag cancels it
    ui.touchDown(pc, t += 1.0);
    ui.update(0.6f);
    UI_CHECK(ui.dragging());
    ui.touchCancel();
    UI_CHECK(!ui.dragging() && ui.lastDropAction() == DropAction::None);

    // where nothing can pan, leaving the slop starts the drag
    ui.update(0.016f);
    ui.touchDown(pl, t += 1.0);
    ui.touchMove({pl.x + 5.f, pl.y}, t += 0.016);
    UI_CHECK(!ui.dragging()); // inside the slop
    ui.touchMove({pl.x + 20.f, pl.y}, t += 0.016);
    UI_CHECK(ui.dragging());
    ui.touchMove(pb, t += 0.016);
    ui.touchUp(pb, t += 0.016);
    UI_CHECK(ui.lastDropAction() == DropAction::Move && ui.lastDropTarget() == bin);
}

void connectViewSignals(UiContext& ui, NodeHandle n) {
    ui.connect(n, SignalKind::RowMoved, [](UiContext&, NodeHandle, const SignalArgs& a) { ++g_rowMoved; g_from = a.value; g_to = a.value2; });
    ui.connect(n, SignalKind::Dropped, [](UiContext&, NodeHandle, const SignalArgs& a) {
        ++g_dropped;
        g_dropIndex = a.value;
        g_dropText = std::string(a.text);
    });
    ui.connect(n, SignalKind::DragEnded, [](UiContext&, NodeHandle, const SignalArgs& a) {
        ++g_dragEnded;
        g_endRow = a.value;
        g_endAccepted = a.flag;
        g_endAction = a.value2;
    });
}
Vec2 rowPoint(UiContext& ui, NodeHandle list, u32 row, f32 fy) {
    const Rect r = w::listRowRect(ui, list, row);
    return {r.x + r.w * 0.5f, r.y + r.h * fy};
}

void viewGate() {
    UiContext ui({.viewport = {900, 420}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "flex-direction", "row");
    ui.setCss(ui.root(), "gap", "20px");
    ui.setCss(ui.root(), "padding", "10px");
    ui.setCss(ui.root(), "align-items", "start");
    const NodeHandle la = w::listView(ui, ui.root(), "A\nB\nC\nD", -1, "la");
    const NodeHandle lb = w::listView(ui, ui.root(), "X\nY", -1, "lb");
    const NodeHandle lc = w::listView(ui, ui.root(), "P\nQ\nR", -1, "lc"); // reorder only
    w::StringTableModel model("Name");
    for (const char* r : {"one", "two", "three", "four"}) model.addRow(r);
    const NodeHandle tb = w::modelListView(ui, ui.root(), &model.model(), "tb");
    const NodeHandle grid = w::gridView(ui, ui.root(), &model.model(), 96.f, 72.f, "grid");
    ui.setCss(grid, "width", "230px");
    ui.setCss(lb, "height", "140px");
    w::setListDragDrop(ui, la, w::kViewDragDrop);
    w::setListDragDrop(ui, lb, w::kViewDragDrop);
    w::setListDragDrop(ui, lc, w::kViewDragReorder);
    w::setTableDragDrop(ui, tb, w::kViewDragDrop);
    w::setGridDragDrop(ui, grid, w::kViewAcceptDrops);
    for (NodeHandle n : {la, lb, lc, tb, grid}) connectViewSignals(ui, n);
    ui.update(0.f);

    // reorder: drag "A" below "C"; the insertion line follows the pointer
    drag(ui, rowPoint(ui, la, 0, 0.5f), rowPoint(ui, la, 2, 0.8f), false);
    UI_CHECK(ui.dragging() && ui.dragTarget() == la);
    UI_CHECK_EQ(w::listDropIndex(ui, la, rowPoint(ui, la, 2, 0.8f)), 3u);
    UI_CHECK_EQ(w::listDropIndex(ui, la, rowPoint(ui, la, 2, 0.2f)), 2u);
    bool line = false;
    const Rect r3 = w::listRowRect(ui, la, 3);
    for (const DrawCmd& c : ui.drawList().cmds()) line = line || (c.op == DrawOp::Rect && c.rect.h == 2.f && c.rect.y == r3.y - 1.f && c.rect.w == r3.w);
    UI_CHECK(line);
    ui.pointerUp(rowPoint(ui, la, 2, 0.8f));
    ui.update(0.f);
    UI_CHECK(ui.text(la) == "B\nC\nA\nD");
    UI_CHECK_EQ(g_rowMoved, 1);
    UI_CHECK(g_from == 0.f && g_to == 2.f);
    UI_CHECK_EQ(ui.value(la), 2.f); // the moved row stays selected
    UI_CHECK_EQ(g_dragEnded, 0);    // a reorder is reported by RowMoved alone
    // dropping a row on itself changes nothing
    drag(ui, rowPoint(ui, la, 1, 0.5f), rowPoint(ui, la, 1, 0.9f));
    UI_CHECK(ui.text(la) == "B\nC\nA\nD");
    UI_CHECK_EQ(g_rowMoved, 1);
    // upwards: "D" to the top
    drag(ui, rowPoint(ui, la, 3, 0.5f), rowPoint(ui, la, 0, 0.1f));
    UI_CHECK(ui.text(la) == "D\nB\nC\nA");
    UI_CHECK(g_from == 3.f && g_to == 0.f);

    // list -> list: "B" moves between "X" and "Y"
    drag(ui, rowPoint(ui, la, 1, 0.5f), rowPoint(ui, lb, 1, 0.2f));
    UI_CHECK(ui.text(lb) == "X\nB\nY");
    UI_CHECK(ui.text(la) == "D\nC\nA");
    UI_CHECK_EQ(g_dropped, 1);
    UI_CHECK(g_dropIndex == 1.f && g_dropText == "B");
    UI_CHECK_EQ(g_dragEnded, 1);
    UI_CHECK(g_endRow == 1.f && g_endAccepted && g_endAction == f32(u8(DropAction::Move)));
    UI_CHECK_EQ(ui.value(lb), 1.f);
    // below the last row appends; Ctrl copies (the source keeps its row)
    ui.setModifiers(kModCtrl);
    const Rect rb = ui.contentRect(lb);
    drag(ui, rowPoint(ui, la, 0, 0.5f), {rb.x + 20.f, rb.bottom() - 4.f});
    ui.setModifiers(0);
    UI_CHECK(ui.text(lb) == "X\nB\nY\nD");
    UI_CHECK(ui.text(la) == "D\nC\nA");
    UI_CHECK(g_endAction == f32(u8(DropAction::Copy)));

    // reorder-only rows cannot leave their view, and the view takes no foreign rows
    const int dropped = g_dropped;
    drag(ui, rowPoint(ui, lc, 0, 0.5f), rowPoint(ui, lb, 0, 0.5f));
    UI_CHECK(ui.lastDropAction() == DropAction::None);
    UI_CHECK(ui.text(lc) == "P\nQ\nR" && ui.text(lb) == "X\nB\nY\nD");
    drag(ui, rowPoint(ui, la, 0, 0.5f), rowPoint(ui, lc, 0, 0.5f));
    UI_CHECK(ui.lastDropAction() == DropAction::None && ui.text(lc) == "P\nQ\nR" && ui.text(la) == "D\nC\nA");
    UI_CHECK_EQ(g_dropped, dropped);
    drag(ui, rowPoint(ui, lc, 2, 0.5f), rowPoint(ui, lc, 0, 0.2f));
    UI_CHECK(ui.text(lc) == "R\nP\nQ");

    // model views report and leave the model to the application
    const int moved = g_rowMoved;
    drag(ui, centre(w::tableRowRect(ui, tb, 0)), {centre(w::tableRowRect(ui, tb, 2)).x, w::tableRowRect(ui, tb, 2).bottom() - 3.f});
    UI_CHECK_EQ(g_rowMoved, moved + 1);
    UI_CHECK(g_from == 0.f && g_to == 2.f);
    UI_CHECK(model.cell(0, 0) == "one"); // untouched
    drag(ui, centre(w::tableRowRect(ui, tb, 3)), rowPoint(ui, la, 0, 0.2f)); // table row -> string list
    UI_CHECK(ui.text(la) == "four\nD\nC\nA");
    UI_CHECK(g_endRow == 3.f && g_endAccepted);
    // a list row onto the grid: between tile 0 and tile 1
    const Rect t1 = w::gridItemRect(ui, grid, 1);
    drag(ui, rowPoint(ui, la, 1, 0.5f), {t1.x + 10.f, t1.y + 30.f});
    UI_CHECK(g_dropIndex == 1.f && g_dropText == "D");
    UI_CHECK(ui.text(la) == "four\nC\nA"); // moved out of the list
    UI_CHECK_EQ(w::gridItemAt(ui, grid, {t1.x + 10.f, t1.y + 30.f}), 1);

    // flags 0 turns dragging off again
    w::setListDragDrop(ui, la, 0);
    drag(ui, rowPoint(ui, la, 0, 0.5f), rowPoint(ui, la, 2, 0.8f));
    UI_CHECK(ui.text(la) == "four\nC\nA");
    ui.update(1.f); // the model view's cell tooltip timer (armed by the hover before a drag) runs out
    ui.update(0.f);
    UI_CHECK(!ui.needsUpdate());
}

// Row drags on the model / view table (header, scrolled rows, wide columns with a horizontal bar, movable columns and
// extended selection): the row helpers see the scrolled geometry, and a row drag takes nothing from the view's own
// press-and-drag interactions (header reorder, scroll bars) or from Shift / Ctrl selection.
void tableGate() {
    UiContext ui({.viewport = {420, 300}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "10px");
    ui.setCss(ui.root(), "gap", "10px");
    w::StringTableModel model("Name\tKind\tSize");
    for (int i = 0; i < 40; ++i) model.addRow("row " + std::to_string(i) + "\tkind\t" + std::to_string(i));
    const NodeHandle tb = w::tableView(ui, ui.root(), &model.model(), "tb");
    ui.setCss(tb, "height", "160px");
    const NodeHandle plain = w::tableView(ui, ui.root(), &model.model(), "plain"); // no drag flags
    ui.setCss(plain, "height", "90px");
    for (u32 c = 0; c < 3; ++c) w::setTableColumnWidth(ui, tb, c, 200.f); // 600 px of columns in a 400 px view
    w::setViewColumnsMovable(ui, tb, true);
    w::setViewSelectionMode(ui, tb, w::SelectionMode::Extended);
    w::setTableDragDrop(ui, tb, w::kViewDragReorder);
    connectViewSignals(ui, tb);
    const int moved = g_rowMoved;
    ui.update(0.f);
    UI_CHECK(w::tableModel(ui, tb) == &model.model());

    // geometry: rows start below the header, follow the vertical scroll and span the view whatever the horizontal one
    const Rect c = ui.contentRect(tb);
    w::setViewFirstVisible(ui, tb, 5);
    w::setViewScrollX(ui, tb, 120.f);
    ui.update(0.f);
    UI_CHECK(w::viewScrollX(ui, tb) == 120.f);
    const Rect r5 = w::tableRowRect(ui, tb, 5), r7 = w::tableRowRect(ui, tb, 7);
    UI_CHECK(r5.y > c.y && r5.x == c.x && r5.w == c.w);
    UI_CHECK(r7.y == r5.y + 2.f * r5.h);
    UI_CHECK(w::tableRowRect(ui, tb, 0).bottom() <= r5.y - 4.f * r5.h); // scrolled out: above the body
    UI_CHECK_EQ(w::tableRowAt(ui, tb, centre(r7)), 7);
    UI_CHECK_EQ(w::tableRowAt(ui, tb, {c.x + 30.f, c.y + 4.f}), -1);          // header
    UI_CHECK_EQ(w::tableRowAt(ui, tb, {c.right() - 3.f, centre(r7).y}), -1);  // vertical scroll bar
    UI_CHECK_EQ(w::tableRowAt(ui, tb, {c.x + 30.f, c.bottom() - 3.f}), -1);   // horizontal scroll bar
    UI_CHECK_EQ(w::tableRowAt(ui, tb, {c.x - 3.f, centre(r7).y}), -1);

    // reorder: row 6 below row 8 (reported; the model is the application's)
    const Vec2 from{c.x + 60.f, centre(w::tableRowRect(ui, tb, 6)).y};
    drag(ui, from, {c.x + 60.f, w::tableRowRect(ui, tb, 8).bottom() - 3.f}, false);
    UI_CHECK(ui.dragging() && ui.dragTarget() == tb);
    ui.pointerUp({c.x + 60.f, w::tableRowRect(ui, tb, 8).bottom() - 3.f});
    ui.update(0.f);
    UI_CHECK_EQ(g_rowMoved, moved + 1);
    UI_CHECK(g_from == 6.f && g_to == 8.f);
    UI_CHECK_EQ(w::tableFirstVisible(ui, tb), 5u);
    UI_CHECK(model.cell(6, 0) == "row 6");

    // Shift / Ctrl clicks still select (a click is not a drag)
    auto click = [&](Vec2 p, u8 mods) {
        ui.update(1.f); // never a double click
        ui.setModifiers(mods);
        ui.pointerMove(p);
        ui.pointerDown(p);
        ui.pointerUp(p);
        ui.setModifiers(0);
        ui.update(0.f);
    };
    w::SelectionModel* sel = w::viewSelectionModel(ui, tb);
    UI_CHECK(sel != nullptr);
    click(centre(w::tableRowRect(ui, tb, 5)), 0);
    click(centre(w::tableRowRect(ui, tb, 8)), kModShift);
    UI_CHECK_EQ(sel->selectedRows(), u64(4));
    click(centre(w::tableRowRect(ui, tb, 6)), kModCtrl);
    UI_CHECK_EQ(sel->selectedRows(), u64(3));
    UI_CHECK(!sel->rowSelected(6) && sel->rowSelected(5) && sel->rowSelected(7) && sel->rowSelected(8));
    UI_CHECK(!ui.dragging());

    // the view's own drags stay its own: a header drag reorders columns, the scroll bars scroll
    w::setViewScrollX(ui, tb, 0.f);
    ui.update(1.f);
    const u32 col0 = w::viewColumnAt(ui, tb, 0);
    drag(ui, {c.x + 100.f, c.y + 6.f}, {c.x + 330.f, c.y + 6.f}, false);
    UI_CHECK(!ui.dragging());
    ui.pointerUp({c.x + 330.f, c.y + 6.f});
    ui.update(0.f);
    UI_CHECK(w::viewColumnAt(ui, tb, 0) != col0 && w::viewColumnAt(ui, tb, 1) == col0);
    const u32 first = w::tableFirstVisible(ui, tb);
    drag(ui, {c.right() - 3.f, c.y + 40.f}, {c.right() - 3.f, c.y + 110.f}, false);
    UI_CHECK(!ui.dragging());
    ui.pointerUp({c.right() - 3.f, c.y + 110.f});
    UI_CHECK(w::tableFirstVisible(ui, tb) > first);
    drag(ui, {c.x + 20.f, c.bottom() - 3.f}, {c.x + 120.f, c.bottom() - 3.f}, false);
    UI_CHECK(!ui.dragging());
    ui.pointerUp({c.x + 120.f, c.bottom() - 3.f});
    UI_CHECK(w::viewScrollX(ui, tb) > 0.f);
    UI_CHECK_EQ(g_rowMoved, moved + 1);

    // a view without the flag: press + drag on a row is no drag
    ui.update(1.f);
    drag(ui, centre(w::tableRowRect(ui, plain, 0)), centre(w::tableRowRect(ui, plain, 2)), false);
    UI_CHECK(!ui.dragging());
    ui.pointerUp(centre(w::tableRowRect(ui, plain, 2)));
    UI_CHECK_EQ(ui.value(plain), 0.f);
    UI_CHECK_EQ(g_rowMoved, moved + 1);
}
} // namespace

int main() {
    coreGate();
    touchGate();
    viewGate();
    tableGate();
    return finish("fuse_ui_drag_drop");
}
