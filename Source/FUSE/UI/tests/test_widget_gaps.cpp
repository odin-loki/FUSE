// fuse_ui_widget_gaps: QToolBar (overflow extension menu, separators, vertical), QHeaderView section moving / hiding
// on the table view, SortFilterProxy filters (all columns, predicate), comparators and source mapping, QSplitter
// sizes / collapsing.
#include "test_util.hpp"

#include <algorithm>
#include <cctype>
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
std::string headerOrder(const UiContext& ui, NodeHandle owner) {
    std::vector<A11ySnapshotNode> v;
    ui.snapshotA11y(v);
    std::string o;
    for (const auto& n : v) if (n.node == owner && n.role == A11yRole::ColumnHeader) o += n.name;
    return o;
}
Vec2 mid(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
void click(UiContext& ui, Vec2 p, u8 mods = 0) {
    ui.setModifiers(mods);
    ui.pointerDown(p);
    ui.pointerUp(p);
    ui.setModifiers(0);
    ui.update(1.f / 60.f);
}
void frames(UiContext& ui, int n = 3) { for (int i = 0; i < n; ++i) ui.update(1.f / 60.f); }
} // namespace

int main() {
    UiContext ui({.viewport = {900, 700}, .font = &fuse::ui::test::font()});
    w::install(ui);
    const NodeHandle col = w::panel(ui, ui.root());
    ui.setCss(col, "width", "880px");

    // ---- tool bar overflow ----
    {
        const NodeHandle bar = w::toolBar(ui, col, false, "tb");
        ui.setCss(bar, "width", "220px");
        std::vector<NodeHandle> items;
        int clicked = -1;
        for (int i = 0; i < 8; ++i) {
            if (i == 3) w::toolBarSeparator(ui, bar);
            const NodeHandle b = w::toolButton(ui, bar, "Tool" + std::to_string(i));
            ui.connect(b, SignalKind::Clicked, [&clicked, i](UiContext&, NodeHandle, const SignalArgs&) { clicked = i; });
            items.push_back(b);
        }
        frames(ui);
        const auto over = w::toolBarOverflow(ui, bar);
        const NodeHandle ext = w::toolBarExtension(ui, bar);
        UI_CHECK(!over.empty() && ui.computed(ext).visible && ui.lastChild(bar) == ext);
        const Rect br = ui.contentRect(bar);
        for (NodeHandle b : items)
            if (ui.computed(b).visible) UI_CHECK(ui.rect(b).right() <= br.right() + 0.5f);
        UI_CHECK(ui.rect(ext).right() <= br.right() + 0.5f);
        UI_CHECK(over.back() == items.back() && !ui.computed(items.back()).visible && ui.computed(items[0]).visible);
        // the menu lists the hidden items; picking one clicks it
        click(ui, mid(ui.rect(ext)));
        ui.emit(ext, SignalKind::Submitted, {.value = f32(over.size() - 1)});
        UI_CHECK(clicked == 7);
        frames(ui);
        // wide enough: everything shows, no extension
        ui.setCss(bar, "width", "860px");
        frames(ui);
        UI_CHECK(w::toolBarOverflow(ui, bar).empty() && !ui.computed(ext).visible);
        for (NodeHandle b : items) UI_CHECK(ui.computed(b).visible);
        // stable: no relayout churn once settled
        ui.update(1.f / 60.f);
        UI_CHECK(!ui.needsUpdate());
        // vertical
        const NodeHandle vb = w::toolBar(ui, col, true, "vtb");
        ui.setCss(vb, "height", "90px");
        ui.setCss(vb, "width", "80px");
        for (int i = 0; i < 6; ++i) w::toolButton(ui, vb, "V" + std::to_string(i));
        frames(ui);
        UI_CHECK(!w::toolBarOverflow(ui, vb).empty());
        UI_CHECK(ui.rect(w::toolBarExtension(ui, vb)).bottom() <= ui.contentRect(vb).bottom() + 0.5f);
        ui.destroy(vb);
        ui.destroy(bar);
        frames(ui);
    }
    // ---- header section move / hide ----
    {
        static w::StringTableModel tm("A\tB\tC");
        for (int i = 0; i < 5; ++i) tm.addRow("a" + std::to_string(i) + "\tb" + std::to_string(i) + "\tc" + std::to_string(i));
        const NodeHandle t = w::tableView(ui, col, &tm.model(), "ht");
        ui.setCss(t, "height", "160px");
        frames(ui);
        w::setItemSelectionBehavior(ui, t, w::SelectionBehavior::Items);
        int moved = -1, sorted = -1;
        ui.connect(t, SignalKind::Event, [&](UiContext&, NodeHandle, const SignalArgs& a) { if (a.text == "sectionMoved") moved = int(a.value); });
        ui.connect(t, SignalKind::Toggled, [&](UiContext&, NodeHandle, const SignalArgs& a) { sorted = int(a.value); });
        UI_CHECK_EQ(headerOrder(ui, t), std::string("ABC"));
        w::tableMoveColumn(ui, t, 2, 0);
        frames(ui);
        UI_CHECK_EQ(headerOrder(ui, t), std::string("CAB"));
        UI_CHECK(moved == 2 && w::tableLogicalColumn(ui, t, 0) == 2u && w::tableVisualColumn(ui, t, 0) == 1u);
        // cells follow the header; clicking a cell reports its logical column
        UI_CHECK(partRect(ui, t, A11yRole::GridCell, "c0").x < partRect(ui, t, A11yRole::GridCell, "a0").x);
        click(ui, mid(partRect(ui, t, A11yRole::GridCell, "c1")));
        UI_CHECK(w::tableCurrentColumn(ui, t) == 2u);
        // Right follows the visual order: C -> A
        ui.setFocus(t);
        ui.keyDown(Key::Right); ui.keyUp(Key::Right); ui.update(1.f / 60.f);
        UI_CHECK(w::tableCurrentColumn(ui, t) == 0u);
        // not movable: a header drag sorts on press, nothing moves
        const Rect hc = partRect(ui, t, A11yRole::ColumnHeader, "C"), ha = partRect(ui, t, A11yRole::ColumnHeader, "B");
        ui.pointerDown(mid(hc)); ui.pointerMove(mid(ha)); ui.pointerUp(mid(ha)); ui.update(1.f / 60.f);
        UI_CHECK(sorted == 2 && headerOrder(ui, t) == "CAB");
        // movable: drag C past B's middle -> A B C
        w::setTableColumnsMovable(ui, t, true);
        sorted = -1;
        const Vec2 to{ha.right() - 4.f - 2.f, ha.y + ha.h * 0.5f};
        ui.pointerDown(mid(hc)); ui.pointerMove({mid(hc).x + 20.f, to.y}); ui.pointerMove(to); ui.pointerUp(to); ui.update(1.f / 60.f);
        frames(ui);
        UI_CHECK_EQ(headerOrder(ui, t), std::string("ABC"));
        UI_CHECK(sorted == -1);
        // a click on a movable header still sorts (on release)
        click(ui, mid(partRect(ui, t, A11yRole::ColumnHeader, "B")));
        UI_CHECK(sorted == 1);
        // hide B: gone from the header, C slides left, Left/Right skip it, restoring brings its width back
        const f32 bw = partRect(ui, t, A11yRole::ColumnHeader, "B").w;
        const f32 cx = partRect(ui, t, A11yRole::ColumnHeader, "C").x;
        w::setTableColumnHidden(ui, t, 1, true);
        frames(ui);
        UI_CHECK(w::tableColumnHidden(ui, t, 1) && headerOrder(ui, t) == "AC");
        UI_CHECK(std::abs(partRect(ui, t, A11yRole::ColumnHeader, "C").x - (cx - bw)) < 0.5f);
        click(ui, mid(partRect(ui, t, A11yRole::GridCell, "a2")));
        ui.keyDown(Key::Right); ui.keyUp(Key::Right); ui.update(1.f / 60.f);
        UI_CHECK(w::tableCurrentColumn(ui, t) == 2u);
        w::setTableColumnHidden(ui, t, 1, false);
        frames(ui);
        UI_CHECK(headerOrder(ui, t) == "ABC" && std::abs(partRect(ui, t, A11yRole::ColumnHeader, "B").w - bw) < 0.5f);
        ui.destroy(t);
        frames(ui);
    }
    // ---- sort / filter proxy ----
    {
        static w::StringTableModel src("Name\tSize");
        for (const char* r : {"alpha\t10", "Beta\t2", "gamma\t33", "delta\t4", "Epsilon\t5"}) src.addRow(r);
        w::SortFilterProxy p(&src.model());
        std::string scratch;
        auto names = [&] {
            std::string o;
            for (u32 r = 0; r < p.rows(); ++r) o += std::string(p.model().data(p.model().user, r, 0, scratch)) + ",";
            return o;
        };
        UI_CHECK(p.proxyRow(3) == 3);
        p.setFilter(w::SortFilterProxy::kAllColumns, "3"); // "gamma 33" by size
        UI_CHECK_EQ(names(), std::string("gamma,"));
        p.setFilter(w::SortFilterProxy::kAllColumns, "TA"); // case-insensitive, any column: beta, delta
        UI_CHECK_EQ(names(), std::string("Beta,delta,"));
        UI_CHECK(p.proxyRow(3) == 1 && p.proxyRow(0) == -1 && p.sourceRow(1) == 3u);
        p.setFilter(0, "");
        // predicate: size > 4
        p.setRowFilter([](const void*, const w::ItemModel& m, u32 r) {
            std::string sc;
            return std::stoi(std::string(m.data(m.user, r, 1, sc))) > 4;
        });
        UI_CHECK_EQ(names(), std::string("alpha,gamma,Epsilon,"));
        p.setRowFilter(nullptr);
        // default string order is byte order (uppercase first); a comparator makes it case-insensitive
        p.sortBy(0, true);
        UI_CHECK_EQ(names(), std::string("Beta,Epsilon,alpha,delta,gamma,"));
        p.setLessThan([](const void*, std::string_view a, std::string_view b) {
            return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) { return std::tolower(x) < std::tolower(y); });
        });
        UI_CHECK_EQ(names(), std::string("alpha,Beta,delta,Epsilon,gamma,"));
        p.sortBy(0, false);
        UI_CHECK_EQ(names(), std::string("gamma,Epsilon,delta,Beta,alpha,"));
        UI_CHECK(p.sortColumn() == 0 && !p.sortAscending() && p.proxyRow(0) == 4);
        p.setLessThan(nullptr);
        p.sortBy(1, true); // numeric
        UI_CHECK_EQ(names(), std::string("Beta,delta,Epsilon,alpha,gamma,"));
        p.sortBy(-1, true);
        UI_CHECK(p.proxyRow(2) == 2);
    }
    // ---- splitter sizes / collapsing ----
    {
        const NodeHandle sp = w::splitter(ui, col, false, "sp");
        ui.setCss(sp, "width", "600px");
        ui.setCss(sp, "height", "100px");
        const NodeHandle a = w::splitterPane(ui, sp, 150.f), b = w::splitterPane(ui, sp);
        w::label(ui, a, "left pane");
        w::label(ui, b, "right");
        frames(ui);
        auto sz = w::splitterSizes(ui, sp);
        UI_CHECK(sz.size() == 2u && sz[0] == 150.f && sz[1] == 600.f - 150.f - 6.f);
        const f32 set[] = {300.f, -1.f};
        w::setSplitterSizes(ui, sp, set);
        frames(ui);
        sz = w::splitterSizes(ui, sp);
        UI_CHECK(sz[0] == 300.f && sz[1] == 294.f);
        const NodeHandle h = ui.nextSibling(a);
        const Rect hr = ui.rect(h);
        // not collapsible: clamps at the minimum
        ui.pointerDown(mid(hr)); ui.pointerMove({ui.rect(sp).x + 3.f, mid(hr).y}); ui.pointerUp({ui.rect(sp).x + 3.f, mid(hr).y});
        frames(ui);
        UI_CHECK(w::splitterSizes(ui, sp)[0] == 24.f);
        w::setSplitterCollapsible(ui, sp, true);
        const Rect hr2 = ui.rect(h);
        ui.pointerDown(mid(hr2)); ui.pointerMove({ui.rect(sp).x + 3.f, mid(hr2).y}); ui.pointerUp({ui.rect(sp).x + 3.f, mid(hr2).y});
        frames(ui);
        sz = w::splitterSizes(ui, sp);
        UI_CHECK(sz[0] == 0.f && sz[1] == 594.f);
        // and the other side
        const Rect hr3 = ui.rect(h);
        const f32 rx = ui.rect(sp).right() - 2.f;
        ui.pointerDown(mid(hr3)); ui.pointerMove({rx, mid(hr3).y}); ui.pointerUp({rx, mid(hr3).y});
        frames(ui);
        sz = w::splitterSizes(ui, sp);
        UI_CHECK(sz[0] == 594.f && sz[1] == 0.f);
        ui.destroy(sp);
        frames(ui);
    }
    return fuse::ui::test::finish("fuse_ui_widget_gaps");
}
