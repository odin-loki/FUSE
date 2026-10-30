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
    // ---- movable / floating tool bars ----
    {
        const NodeHandle host = w::panel(ui, col);
        ui.setCss(host, "height", "200px");
        ui.setCss(host, "padding", "0px");
        const NodeHandle top = w::toolBarArea(ui, host, false, "tba.top");
        const NodeHandle row = w::panel(ui, host);
        ui.setCss(row, "flex-direction", "row");
        ui.setCss(row, "flex-grow", "1");
        ui.setCss(row, "padding", "0px");
        const NodeHandle left = w::toolBarArea(ui, row, true, "tba.left");
        const NodeHandle t1 = w::toolBar(ui, top, false, "tb1"), t2 = w::toolBar(ui, top, false, "tb2");
        for (NodeHandle t : {t1, t2}) {
            w::setToolBarMovable(ui, t, true);
            w::toolButton(ui, t, "A");
            w::toolBarSeparator(ui, t);
            w::toolButton(ui, t, "B");
        }
        frames(ui);
        int tl = -1, orient = -1;
        ui.connect(t1, SignalKind::Event, [&](UiContext&, NodeHandle, const SignalArgs& a) {
            if (a.text == "topLevelChanged") tl = a.flag;
            if (a.text == "orientationChanged") orient = a.flag;
        });
        auto grip = [&](NodeHandle t) { return mid(ui.rect(ui.firstChild(t))); };
        UI_CHECK(ui.rect(ui.firstChild(t1)).w > 0.f && w::toolBarOverflow(ui, t1).empty());
        // drag t1 after t2 within the top area
        const Vec2 g = grip(t1);
        const Rect r2 = ui.rect(t2);
        ui.pointerDown(g); ui.pointerMove({g.x + 12.f, g.y}); ui.pointerMove({r2.right() - 2.f, g.y}); ui.pointerUp({r2.right() - 2.f, g.y}); frames(ui);
        UI_CHECK(ui.firstChild(top) == t2 && ui.nextSibling(t2) == t1 && tl == -1);
        // into the vertical left area: turns vertical
        const Vec2 g1 = grip(t1);
        const Rect lr = ui.rect(left);
        const Vec2 in{lr.x + lr.w * 0.5f, lr.y + 20.f};
        ui.pointerDown(g1); ui.pointerMove({g1.x, g1.y + 12.f}); ui.pointerMove(in); ui.pointerUp(in); frames(ui);
        UI_CHECK(ui.parent(t1) == left && w::toolBarIsVertical(ui, t1) && orient == 1);
        UI_CHECK(ui.rect(t1).h > ui.rect(t1).w);
        // outside every area: floats under the pointer
        const Vec2 g2 = grip(t1), out{600.f, 150.f};
        ui.pointerDown(g2); ui.pointerMove({g2.x + 12.f, g2.y}); ui.pointerMove(out); ui.pointerUp(out); frames(ui);
        UI_CHECK(w::toolBarIsFloating(ui, t1) && tl == 1);
        UI_CHECK(ui.rect(t1).contains(out));
        // not floatable: stays put when dragged outside
        w::toolBarAddTo(ui, top, t1);
        frames(ui);
        UI_CHECK(!w::toolBarIsFloating(ui, t1) && tl == 0 && !w::toolBarIsVertical(ui, t1) && ui.parent(t1) == top);
        w::setToolBarFloatable(ui, t1, false);
        const Vec2 g3 = grip(t1);
        ui.pointerDown(g3); ui.pointerMove({g3.x + 12.f, g3.y}); ui.pointerMove(out); ui.pointerUp(out); frames(ui);
        UI_CHECK(!w::toolBarIsFloating(ui, t1) && ui.parent(t1) == top);
        // not movable: no grip
        w::setToolBarMovable(ui, t2, false);
        UI_CHECK(ui.text(ui.firstChild(t2)) == "A");
        ui.destroy(host);
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
        ui.pointerDown(mid(hc)); ui.pointerMove({mid(hc).x + 20.f, to.y});
        {
            Rect img; f32 mk = 0.f;
            UI_CHECK(w::tableHeaderDrag(ui, t, img, mk));
            UI_CHECK(std::abs(img.x - (hc.x + 20.f)) < 0.5f && std::abs(img.w - hc.w) < 0.5f && std::abs(img.h - hc.h) < 0.5f);
            ui.pointerMove(to);
            UI_CHECK(w::tableHeaderDrag(ui, t, img, mk) && std::abs(mk - ha.right()) < 0.5f); // after B
        }
        ui.pointerUp(to); ui.update(1.f / 60.f);
        { Rect img; f32 mk; UI_CHECK(!w::tableHeaderDrag(ui, t, img, mk)); }
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
    // ---- floating / tabbed docks + layout persistence ----
    {
        ui.setCss(col, "display", "none"); // a full-viewport dock app
        const NodeHandle host = w::panel(ui, ui.root());
        ui.setCss(host, "width", "880px");
        ui.setCss(host, "height", "600px");
        ui.setCss(host, "padding", "0px");
        const NodeHandle area = w::dockArea(ui, host, "da");
        const NodeHandle d1 = w::dockWidget(ui, area, "Outline", w::DockSide::Left, "d.outline");
        const NodeHandle d2 = w::dockWidget(ui, area, "Assets", w::DockSide::Left, "d.assets");
        const NodeHandle d3 = w::dockWidget(ui, area, "Console", w::DockSide::Bottom, "d.console");
        const NodeHandle d4 = w::dockWidget(ui, area, "Props", w::DockSide::Right, "d.props");
        frames(ui);
        const std::string fresh = w::dockSaveLayout(ui, area);
        int top = -1;
        ui.connect(d3, SignalKind::Event, [&](UiContext&, NodeHandle, const SignalArgs& a) { if (a.text == "topLevelChanged") top = a.flag; });
        // float programmatically
        w::dockFloat(ui, d3, {300, 200, 260, 140});
        frames(ui);
        UI_CHECK(w::dockIsFloating(ui, d3) && top == 1);
        const Rect fr = ui.rect(d3);
        UI_CHECK(std::abs(fr.x - 300.f) < 0.5f && std::abs(fr.y - 200.f) < 0.5f && std::abs(fr.w - 260.f) < 0.5f && std::abs(fr.h - 140.f) < 0.5f);
        UI_CHECK(!ui.computed(w::dockRegion(ui, area, w::DockSide::Bottom)).visible); // its region emptied
        // drag the floating title: it follows the pointer
        const Rect tr = ui.rect(ui.firstChild(d3));
        const Vec2 g{tr.x + 30.f, tr.y + tr.h * 0.5f};
        ui.pointerDown(g); ui.pointerMove({g.x + 40.f, g.y + 20.f}); ui.update(1.f / 60.f);
        ui.pointerMove({g.x + 60.f, g.y + 30.f}); ui.pointerUp({g.x + 60.f, g.y + 30.f}); frames(ui);
        UI_CHECK(w::dockIsFloating(ui, d3) && std::abs(ui.rect(d3).x - 360.f) < 0.5f && std::abs(ui.rect(d3).y - 230.f) < 0.5f);
        // drop it on the right band: docks right
        const Rect t2 = ui.rect(ui.firstChild(d3));
        const Vec2 g2{t2.x + 30.f, t2.y + t2.h * 0.5f};
        const Rect ar = ui.rect(area);
        ui.pointerDown(g2); ui.pointerMove({ar.right() - 60.f, g2.y}); ui.update(1.f / 60.f);
        ui.pointerMove({ar.right() - 10.f, g2.y}); ui.pointerUp({ar.right() - 10.f, g2.y}); frames(ui);
        UI_CHECK(!w::dockIsFloating(ui, d3) && top == 0);
        UI_CHECK(ui.parent(d3) == w::dockRegion(ui, area, w::DockSide::Right));
        // dragging a docked title floats it
        const Rect t3 = ui.rect(ui.firstChild(d1));
        const Vec2 g3{t3.x + 30.f, t3.y + t3.h * 0.5f};
        ui.pointerDown(g3); ui.pointerMove({g3.x + 300.f, g3.y + 200.f}); ui.update(1.f / 60.f);
        UI_CHECK(w::dockIsFloating(ui, d1));
        // ... and releasing it over Assets' title tabs it with Assets
        const Rect at = ui.rect(ui.firstChild(d2));
        ui.pointerMove(mid(at)); ui.pointerUp(mid(at)); frames(ui);
        UI_CHECK(!w::dockIsFloating(ui, d1) && w::dockTabs(ui, d2).size() == 2u && w::dockCurrentTab(ui, d2) == d1);
        UI_CHECK(ui.computed(d1).visible && !ui.computed(d2).visible);
        // the group's tab bar switches
        const NodeHandle bar = ui.firstChild(ui.parent(d2));
        UI_CHECK_EQ(std::string(ui.text(bar)), std::string("Assets\nOutline"));
        ui.setValue(bar, 0.f);
        ui.emit(bar, SignalKind::ValueChanged, {.value = 0.f});
        frames(ui);
        UI_CHECK(w::dockCurrentTab(ui, d1) == d2 && ui.computed(d2).visible && !ui.computed(d1).visible);
        // programmatic tabify on the right; save
        w::dockTabify(ui, d4, d3);
        w::dockFloat(ui, d2, {500, 100, 200, 150});
        frames(ui);
        UI_CHECK(w::dockTabs(ui, d1).empty() && ui.parent(d1) == w::dockRegion(ui, area, w::DockSide::Left)); // group dissolved
        const std::string saved = w::dockSaveLayout(ui, area);
        std::printf("dock layout v2:\n%s", saved.c_str());
        UI_CHECK(saved.rfind("fusedock 2", 0) == 0);
        UI_CHECK(saved.find("tab d.console d.props") != std::string::npos && saved.find("current d.console") != std::string::npos);
        UI_CHECK(saved.find("float d.assets left 500 100 200 150 open") != std::string::npos);
        // back to the fresh layout, then restore the saved one
        UI_CHECK(w::dockRestoreLayout(ui, area, fresh));
        frames(ui);
        UI_CHECK(!w::dockIsFloating(ui, d2) && w::dockTabs(ui, d3).empty() && ui.parent(d3) == w::dockRegion(ui, area, w::DockSide::Bottom));
        UI_CHECK(w::dockRestoreLayout(ui, area, saved));
        frames(ui);
        UI_CHECK_EQ(w::dockSaveLayout(ui, area), saved);
        UI_CHECK(w::dockIsFloating(ui, d2) && w::dockCurrentTab(ui, d4) == d3);
        // version 1 still restores; garbage does not
        UI_CHECK(w::dockRestoreLayout(ui, area, "fusedock 1\ndock d.assets left 0 open\n"));
        UI_CHECK(!w::dockRestoreLayout(ui, area, "fusedock 2\nfloat d.assets left 1 2 0 0 open\n"));
        frames(ui);
        UI_CHECK(!w::dockIsFloating(ui, d2));
        ui.destroy(host);
        ui.setCss(col, "display", "flex");
        frames(ui);
    }
    // ---- QTabBar parity: closable, movable, disabled, scroll buttons; tab widget add / remove ----
    {
        const NodeHandle tabs = w::tabWidget(ui, col, "Alpha\nBeta\nGamma", "tw");
        ui.setCss(tabs, "height", "120px");
        const NodeHandle bar = w::tabWidgetBar(ui, tabs);
        const NodeHandle pA = w::tabPage(ui, tabs, 0), pB = w::tabPage(ui, tabs, 1), pG = w::tabPage(ui, tabs, 2);
        std::vector<std::string> events;
        ui.connect(bar, SignalKind::Event, [&events](UiContext&, NodeHandle, const SignalArgs& a) { if (!a.event) events.emplace_back(a.text); });
        frames(ui);
        const f32 w0 = w::tabBarTabRect(ui, bar, 0).w;
        w::setTabsClosable(ui, bar, true);
        w::setTabsMovable(ui, bar, true);
        frames(ui);
        UI_CHECK(w::tabsClosable(ui, bar) && w::tabsMovable(ui, bar) && w::tabBarTabRect(ui, bar, 0).w > w0);
        // close box -> tabCloseRequested, current unchanged
        const Rect r1 = w::tabBarTabRect(ui, bar, 1);
        click(ui, {r1.right() - 12.f, r1.y + r1.h * 0.5f});
        UI_CHECK(!events.empty() && events.back() == "tabCloseRequested" && ui.value(bar) == 0.f);
        // drag Alpha right past Beta's middle: live move, page follows
        const Rect r0 = w::tabBarTabRect(ui, bar, 0);
        ui.pointerDown(mid(r0)); ui.update(1.f / 60.f);
        ui.pointerMove({mid(r0).x + 12.f, mid(r0).y}); ui.update(1.f / 60.f);
        ui.pointerMove({mid(r1).x + 8.f, mid(r1).y}); ui.update(1.f / 60.f);
        ui.pointerUp({mid(r1).x + 8.f, mid(r1).y}); frames(ui);
        UI_CHECK_EQ(std::string(ui.text(bar)), std::string("Beta\nAlpha\nGamma"));
        UI_CHECK(ui.value(bar) == 1.f && w::tabPage(ui, tabs, 0) == pB && w::tabPage(ui, tabs, 1) == pA && ui.computed(pA).visible);
        UI_CHECK(!events.empty() && events.back() == "tabMoved 0 1");
        // disabled tab is skipped by keys and clicks
        w::setTabEnabled(ui, bar, 2, false);
        UI_CHECK(!w::tabEnabled(ui, bar, 2));
        click(ui, mid(w::tabBarTabRect(ui, bar, 2)));
        UI_CHECK(ui.value(bar) == 1.f);
        ui.setFocus(bar);
        ui.keyDown(Key::End); ui.keyUp(Key::End); ui.update(1.f / 60.f);
        UI_CHECK(ui.value(bar) == 1.f);
        ui.keyDown(Key::Home); ui.keyUp(Key::Home); ui.update(1.f / 60.f);
        UI_CHECK(ui.value(bar) == 0.f && ui.computed(pB).visible);
        w::setTabEnabled(ui, bar, 2, true);
        // disabling the current tab moves off it
        w::setTabEnabled(ui, bar, 0, false);
        UI_CHECK(ui.value(bar) == 1.f);
        w::setTabEnabled(ui, bar, 0, true);
        // add / remove pages
        const NodeHandle pD = w::tabWidgetAddPage(ui, tabs, "Delta");
        frames(ui);
        UI_CHECK(w::tabPage(ui, tabs, 3) == pD && ui.text(bar).ends_with("\nDelta") && !ui.computed(pD).visible);
        w::tabWidgetRemovePage(ui, tabs, 1); // current (Alpha) removed -> Gamma (now 1) current
        frames(ui);
        UI_CHECK_EQ(std::string(ui.text(bar)), std::string("Beta\nGamma\nDelta"));
        UI_CHECK(ui.value(bar) == 1.f && ui.computed(pG).visible && w::tabPage(ui, tabs, 2) == pD);
        // overflow: narrow bar scrolls to keep the current tab visible
        for (int i = 0; i < 12; ++i) w::tabWidgetAddPage(ui, tabs, "Extra tab " + std::to_string(i));
        ui.setCss(tabs, "width", "300px");
        frames(ui);
        UI_CHECK_EQ(w::tabBarScroll(ui, bar), 0.f);
        ui.keyDown(Key::End); ui.keyUp(Key::End); frames(ui);
        const Rect last = w::tabBarTabRect(ui, bar, 14), bc = ui.contentRect(bar);
        UI_CHECK(w::tabBarScroll(ui, bar) > 0.f && last.right() <= bc.right() - 36.f + 0.5f && last.x >= bc.x);
        // left scroll button scrolls back
        const f32 sc = w::tabBarScroll(ui, bar);
        click(ui, {bc.right() - 27.f, bc.y + bc.h * 0.5f});
        UI_CHECK(w::tabBarScroll(ui, bar) < sc);
        ui.destroy(tabs);
        frames(ui);
    }
    return fuse::ui::test::finish("fuse_ui_widget_gaps");
}
