// fuse_ui_dock_split: the editor-facing widget API ported from claude/magical-bassi-815825 onto main's dock model
// (docs/ui/archive/PORT_magical_bassi.md §3.12): dock splits (dockSplit / DockEdge, dockOpen, dockRemove, dockSideOf,
// dockSplitOf, layout format 2) next to main's tab groups, floating docks and dock drags; tree line geometry
// (treeLineAt / treeLineRect); timeline clip selection and clip-drag reporting.
// `--gate=<name>` runs one group of checks; `--out=<png>` writes the split layout as an image.
#include "test_util.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_write_impl.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
bool near(f32 a, f32 b, f32 eps = 0.75f) { return std::abs(a - b) <= eps; }
void key(UiContext& ui, Key k, u8 mods = 0) {
    ui.keyDown(k, mods);
    ui.keyUp(k, mods);
    ui.update(1.f / 60.f);
}
struct DockApp {
    NodeHandle area;
    NodeHandle docks[6];
};
enum { Outline, Assets, Log, Console, Props, History };
const char* kDockIds[] = {"outline", "assets", "log", "console", "props", "history"};
DockApp buildDocks(UiContext& ui, NodeHandle parent, std::string_view areaId) {
    DockApp a;
    a.area = w::dockArea(ui, parent, areaId);
    ui.setCss(a.area, "flex-grow", "1");
    const w::DockSide sides[] = {w::DockSide::Left, w::DockSide::Left, w::DockSide::Bottom, w::DockSide::Bottom, w::DockSide::Right, w::DockSide::Right};
    for (int i = 0; i < 6; ++i) {
        std::string id = std::string(areaId) + "." + kDockIds[i];
        a.docks[i] = w::dockWidget(ui, a.area, kDockIds[i], sides[i], id);
        w::label(ui, w::dockBody(ui, a.docks[i]), kDockIds[i]);
    }
    w::label(ui, w::dockRegion(ui, a.area, w::DockSide::Center), "viewport");
    return a;
}
std::string renamed(std::string s, std::string_view from, std::string_view to) {
    for (std::size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
    return s;
}
NodeHandle host(UiContext& ui) {
    const NodeHandle h = w::panel(ui, ui.root());
    ui.setCss(h, "flex-grow", "1");
    ui.setCss(h, "padding", "0px");
    return h;
}
u32 countClass(const UiContext& ui, NodeHandle n, std::string_view cls) {
    u32 c = ui.hasClass(n, cls) ? 1u : 0u;
    for (NodeHandle k = ui.firstChild(n); ui.alive(k); k = ui.nextSibling(k)) c += countClass(ui, k, cls);
    return c;
}
struct Fixture {
    UiContext ui{{.viewport = {1400, 560}, .font = &font()}};
    Fixture() {
        w::install(ui);
        ui.setCss(ui.root(), "padding", "0px");
        ui.setCss(ui.root(), "flex-direction", "row");
    }
};
void shot(UiContext& ui, const char* path) {
    ui.update();
    Image img;
    render(ui, img);
    stbi_write_png(path, img.width, img.height, 4, img.pixels.data(), img.width * 4);
}

// ---- splits: nesting, same-direction panes, hiding, save / restore, collapse ---------------------------------------
void buildSplitLayout(UiContext& ui, DockApp& c) {
    w::dockSplit(ui, c.docks[Assets], c.docks[Outline], w::DockEdge::Right);
    w::dockSplit(ui, c.docks[History], c.docks[Assets], w::DockEdge::Bottom);
    w::dockSplit(ui, c.docks[Props], c.docks[Outline], w::DockEdge::Left);
    w::dockSplit(ui, c.docks[Console], c.docks[Log], w::DockEdge::Top);
    ui.update();
}
void gateSplit() {
    Fixture f;
    UiContext& ui = f.ui;
    DockApp a = buildDocks(ui, host(ui), "A");
    DockApp c = buildDocks(ui, host(ui), "C");
    ui.update();
    const std::string freshC = w::dockSaveLayout(ui, c.area);
    UI_CHECK(freshC.rfind("fusedock 1\n", 0) == 0 && freshC.find("tree") == std::string::npos); // no splits: format 1
    auto rc = [&](int i) { return ui.rect(c.docks[i]); };
    for (int i = 0; i < 6; ++i) UI_CHECK(!w::dockSplitOf(ui, c.docks[i]).valid());
    UI_CHECK(w::dockSideOf(ui, c.docks[Log]) == w::DockSide::Bottom && w::dockSideOf(ui, c.docks[Props]) == w::DockSide::Right);

    // assets beside outline (a row split on the left)
    UI_CHECK(w::dockSplit(ui, c.docks[Assets], c.docks[Outline], w::DockEdge::Right));
    ui.update();
    UI_CHECK(w::dockSplitOf(ui, c.docks[Assets]).valid() && w::dockSplitOf(ui, c.docks[Assets]) == w::dockSplitOf(ui, c.docks[Outline]));
    UI_CHECK(rc(Assets).x > rc(Outline).right() - 1.f && near(rc(Assets).y, rc(Outline).y) && near(rc(Assets).h, rc(Outline).h));
    UI_CHECK(near(rc(Assets).w, rc(Outline).w, 1.5f)); // the two panes share the side
    // history under assets: nests a column split in the row's second pane
    UI_CHECK(w::dockSplit(ui, c.docks[History], c.docks[Assets], w::DockEdge::Bottom));
    ui.update();
    const NodeHandle inner = w::dockSplitOf(ui, c.docks[History]);
    UI_CHECK(inner.valid() && inner == w::dockSplitOf(ui, c.docks[Assets]) && inner != w::dockSplitOf(ui, c.docks[Outline]));
    UI_CHECK(near(rc(History).x, rc(Assets).x) && rc(History).y > rc(Assets).bottom() - 1.f);
    UI_CHECK(w::dockSideOf(ui, c.docks[History]) == w::DockSide::Left);
    // props left of outline: same direction as outline's split, so one more pane in it (no nesting); the right side empties
    UI_CHECK(w::dockSplit(ui, c.docks[Props], c.docks[Outline], w::DockEdge::Left));
    ui.update();
    UI_CHECK(w::dockSplitOf(ui, c.docks[Props]) == w::dockSplitOf(ui, c.docks[Outline]));
    UI_CHECK(rc(Props).right() <= rc(Outline).x + 1.f && !ui.computed(w::dockRegion(ui, c.area, w::DockSide::Right)).visible);
    UI_CHECK(w::dockSideOf(ui, c.docks[Props]) == w::DockSide::Left);
    // console above log (column split at the bottom)
    UI_CHECK(w::dockSplit(ui, c.docks[Console], c.docks[Log], w::DockEdge::Top));
    ui.update();
    UI_CHECK(rc(Console).bottom() <= rc(Log).y + 1.f && near(rc(Console).x, rc(Log).x));
    // refused: itself, a dock of another area, a node that is no dock
    UI_CHECK(!w::dockSplit(ui, c.docks[Log], c.docks[Log], w::DockEdge::Left));
    UI_CHECK(!w::dockSplit(ui, c.docks[Log], a.docks[0], w::DockEdge::Left));
    UI_CHECK(!w::dockSplit(ui, c.docks[Log], c.area, w::DockEdge::Left) && !w::dockSplit(ui, NodeHandle{}, c.docks[Log], w::DockEdge::Left));
    UI_CHECK_EQ(countClass(ui, c.area, "docksplit"), 3u);

    // the nested handle resizes (keyboard); a closed dock's pane and handle hide, dockOpen brings them back in place
    const NodeHandle innerHandle = ui.nextSibling(ui.parent(c.docks[Assets]));
    UI_CHECK(ui.classOf(innerHandle) == ui.registry().find("splithandle"));
    ui.setFocus(innerHandle);
    const f32 h0 = rc(Assets).h;
    for (int i = 0; i < 2; ++i) key(ui, Key::Down);
    ui.update();
    const f32 assetsH = rc(Assets).h;
    UI_CHECK(near(assetsH, h0 + 32.f, 1.5f));
    int toggles = 0;
    ui.connect(c.docks[History], SignalKind::Toggled, [&toggles](UiContext&, NodeHandle, const SignalArgs& a) { toggles += a.flag ? 1 : 100; });
    w::dockClose(ui, c.docks[History]);
    ui.update();
    UI_CHECK(!ui.computed(innerHandle).visible && rc(Assets).h > assetsH + 20.f && toggles == 100);
    w::dockOpen(ui, c.docks[History]);
    ui.update();
    UI_CHECK(ui.computed(innerHandle).visible && near(rc(Assets).h, assetsH) && toggles == 101);
    UI_CHECK(w::dockSplitOf(ui, c.docks[History]) == inner);
    w::dockOpen(ui, c.docks[History]); // already open: no signal
    UI_CHECK(toggles == 101);
    w::dockClose(ui, c.docks[Console]);
    ui.update();

    const std::string split = w::dockSaveLayout(ui, c.area);
    std::printf("split layout:\n%s", split.c_str());
    UI_CHECK(split.rfind("fusedock 2\n", 0) == 0);
    UI_CHECK(split.find("tree left h[ p 0 d:C.props p 0 d:C.outline p 0 v[ p ") != std::string::npos);
    UI_CHECK(split.find("tree bottom v[ p 0 d:C.console p 0 d:C.log ]") != std::string::npos);
    UI_CHECK(split.find("dock C.console bottom 0 closed") != std::string::npos);
    UI_CHECK(split.find("tree right") == std::string::npos);

    // restored into a fresh area: the same blob and the same rects
    DockApp d = buildDocks(ui, host(ui), "D");
    ui.update();
    const std::string splitD = renamed(split, "C.", "D.");
    UI_CHECK(w::dockRestoreLayout(ui, d.area, splitD));
    ui.update();
    UI_CHECK_EQ(w::dockSaveLayout(ui, d.area), splitD);
    const Rect rcA = ui.rect(c.area), rdA = ui.rect(d.area);
    for (int i = 0; i < 6; ++i) {
        UI_CHECK_EQ(ui.computed(c.docks[i]).visible, ui.computed(d.docks[i]).visible);
        if (!ui.computed(c.docks[i]).visible) continue;
        const Rect x = ui.rect(c.docks[i]), y = ui.rect(d.docks[i]);
        UI_CHECK(near(x.x - rcA.x, y.x - rdA.x) && near(x.y - rcA.y, y.y - rdA.y) && near(x.w, y.w) && near(x.h, y.h));
    }
    // restoring the same blob again is stable (the splits are flattened and rebuilt, not doubled)
    UI_CHECK(w::dockRestoreLayout(ui, d.area, splitD));
    ui.update();
    UI_CHECK_EQ(w::dockSaveLayout(ui, d.area), splitD);
    UI_CHECK_EQ(countClass(ui, d.area, "docksplit"), 3u);

    // a dock leaving a two-pane split collapses it; a removed dock too
    w::dockTo(ui, d.docks[Assets], w::DockSide::Right);
    ui.update();
    UI_CHECK(w::dockSplitOf(ui, d.docks[History]) == w::dockSplitOf(ui, d.docks[Outline]) && !w::dockSplitOf(ui, d.docks[Assets]).valid());
    UI_CHECK_EQ(countClass(ui, d.area, "docksplit"), 2u);
    const NodeHandle extra = w::dockWidget(ui, d.area, "extra", w::DockSide::Bottom, "D.extra");
    UI_CHECK(w::dockSplit(ui, extra, d.docks[Log], w::DockEdge::Right, 150.f));
    ui.update();
    UI_CHECK(near(ui.rect(extra).w, 150.f, 2.f) && w::dockSplitOf(ui, extra).valid());
    UI_CHECK(w::dockSplitOf(ui, extra) != w::dockSplitOf(ui, d.docks[Console])); // a row nested in the column
    w::dockRemove(ui, extra);
    ui.update();
    UI_CHECK(!ui.alive(extra) && w::dockSplitOf(ui, d.docks[Log]) == w::dockSplitOf(ui, d.docks[Console]));
    UI_CHECK_EQ(countClass(ui, d.area, "docksplit"), 2u);
    w::dockRemove(ui, extra);        // dead handle: no-op
    w::dockRemove(ui, d.area);       // not a dock: no-op
    UI_CHECK(ui.alive(d.area));

    // malformed: nothing changes
    const std::string beforeBad = w::dockSaveLayout(ui, d.area);
    for (const char* bad : {"fusedock 2\ntree left h[ p 0 d:D.log p 0\n", "fusedock 3\n", "fusedock 1\ntree left d:D.log\n",
                            "fusedock 2\ntree left x[ ]\n", "fusedock 2\ntree left h[ p -3 d:D.log ]\n", "fusedock 2\ntree left h[ d:D.log ]\n",
                            "fusedock 2\ntree up d:D.log\n"}) {
        UI_CHECK(!w::dockRestoreLayout(ui, d.area, bad));
        ui.update();
        UI_CHECK_EQ(w::dockSaveLayout(ui, d.area), beforeBad);
    }
    // the fresh (unsplit) layout flattens every split
    const std::string freshD = renamed(freshC, "C.", "D.");
    UI_CHECK(w::dockRestoreLayout(ui, d.area, freshD));
    ui.update();
    UI_CHECK_EQ(w::dockSaveLayout(ui, d.area), freshD);
    for (int i = 0; i < 6; ++i) UI_CHECK(!w::dockSplitOf(ui, d.docks[i]).valid() && ui.computed(d.docks[i]).visible);
    UI_CHECK_EQ(countClass(ui, d.area, "docksplit"), 0u);
    // a version 1 blob (no tree lines) restores over a split layout and flattens it
    UI_CHECK(w::dockRestoreLayout(ui, d.area, splitD));
    ui.update();
    UI_CHECK(w::dockRestoreLayout(ui, d.area, "fusedock 1\nsize left 200\ndock D.outline left 0 open\ndock D.assets left 1 open\n"));
    ui.update();
    for (int i = 0; i < 6; ++i) UI_CHECK(!w::dockSplitOf(ui, d.docks[i]).valid());
    // names the tree does not know are dropped with their panes; a tree of one known dock leaves no split
    UI_CHECK(w::dockRestoreLayout(ui, d.area, "fusedock 2\ntree left h[ p 0 d:D.nothere p 0 d:D.outline p 90 d:D.gone ]\n"));
    ui.update();
    UI_CHECK(!w::dockSplitOf(ui, d.docks[Outline]).valid() && ui.parent(d.docks[Outline]) == w::dockRegion(ui, d.area, w::DockSide::Left));
    UI_CHECK(w::dockRestoreLayout(ui, d.area, "fusedock 2\ntree left h[ p 0 d:D.nothere p 0 d:D.outline p 90 d:D.assets ]\n"));
    ui.update();
    UI_CHECK(w::dockSplitOf(ui, d.docks[Outline]).valid() && near(ui.rect(d.docks[Assets]).w, 90.f, 2.f));
    UI_CHECK(ui.rect(d.docks[Outline]).x < ui.rect(d.docks[Assets]).x);
}

// ---- splits next to main's tab groups, floating docks and dock drags -------------------------------------------------
void gateSplitGroups() {
    Fixture f;
    UiContext& ui = f.ui;
    DockApp c = buildDocks(ui, host(ui), "C");
    ui.update();
    auto rc = [&](int i) { return ui.rect(c.docks[i]); };
    // Center = a tab of the target's group, in view
    UI_CHECK(w::dockSplit(ui, c.docks[Assets], c.docks[Outline], w::DockEdge::Center));
    ui.update();
    const NodeHandle strip = w::dockTabBar(ui, c.docks[Outline]);
    UI_CHECK(strip.valid() && w::dockTabBar(ui, c.docks[Assets]) == strip && w::dockCurrent(ui, c.docks[Outline]) == c.docks[Assets]);
    UI_CHECK(w::dockSplit(ui, c.docks[Outline], c.docks[Assets], w::DockEdge::Center) && w::dockCurrent(ui, c.docks[Assets]) == c.docks[Outline]);
    // splitting beside a tabbed dock moves the whole group (strip and members) into the first pane
    UI_CHECK(w::dockSplit(ui, c.docks[History], c.docks[Outline], w::DockEdge::Bottom));
    ui.update();
    const NodeHandle split = w::dockSplitOf(ui, c.docks[History]);
    UI_CHECK(split.valid() && w::dockSplitOf(ui, c.docks[Outline]) == split && w::dockSplitOf(ui, c.docks[Assets]) == split);
    UI_CHECK(ui.parent(strip) == ui.parent(c.docks[Outline]) && ui.parent(c.docks[Assets]) == ui.parent(c.docks[Outline]));
    UI_CHECK(w::dockTabBar(ui, c.docks[Assets]) == strip && w::dockCurrent(ui, c.docks[Assets]) == c.docks[Outline]);
    UI_CHECK(ui.rect(strip).bottom() <= rc(Outline).y + 1.f && rc(History).y >= rc(Outline).bottom() - 1.f);
    UI_CHECK(!ui.computed(c.docks[Assets]).visible && ui.computed(c.docks[Outline]).visible);
    // raising the other tab works inside the pane; the save / restore round trip keeps tabs and splits together
    w::dockRaise(ui, c.docks[Assets]);
    ui.update();
    UI_CHECK(ui.computed(c.docks[Assets]).visible && !ui.computed(c.docks[Outline]).visible);
    const std::string saved = w::dockSaveLayout(ui, c.area);
    std::printf("split + tabs layout:\n%s", saved.c_str());
    UI_CHECK(saved.find("tree left v[ p 0 d:C.outline d:C.assets p 0 d:C.history ]") != std::string::npos);
    UI_CHECK(saved.find("tab C.outline 0 0 back") != std::string::npos && saved.find("tab C.assets 0 1 current") != std::string::npos);
    DockApp d = buildDocks(ui, host(ui), "D");
    ui.update();
    const std::string savedD = renamed(saved, "C.", "D.");
    UI_CHECK(w::dockRestoreLayout(ui, d.area, savedD));
    ui.update();
    UI_CHECK_EQ(w::dockSaveLayout(ui, d.area), savedD);
    UI_CHECK(w::dockCurrent(ui, d.docks[Outline]) == d.docks[Assets] && w::dockSplitOf(ui, d.docks[Outline]) == w::dockSplitOf(ui, d.docks[History]));
    UI_CHECK(w::dockSplitOf(ui, d.docks[Outline]).valid());

    // a closed tab member reopens in place with dockOpen: its tab is back, the current tab stays
    w::dockClose(ui, c.docks[Outline]);
    ui.update();
    UI_CHECK(w::dockTabRect(ui, c.docks[Outline]).empty());
    w::dockOpen(ui, c.docks[Outline]);
    ui.update();
    UI_CHECK(!w::dockTabRect(ui, c.docks[Outline]).empty() && w::dockCurrent(ui, c.docks[Outline]) == c.docks[Assets]);
    UI_CHECK(w::dockSplitOf(ui, c.docks[Outline]) == split);

    // floating a dock out of a two-pane split collapses the split around what stays
    w::dockFloat(ui, c.docks[History], {500, 100, 240, 200});
    ui.update();
    UI_CHECK(w::dockFloating(ui, c.docks[History]) && !ui.alive(split) && !w::dockSplitOf(ui, c.docks[Outline]).valid());
    UI_CHECK(w::dockTabBar(ui, c.docks[Outline]) == strip && ui.parent(strip) == w::dockRegion(ui, c.area, w::DockSide::Left));
    UI_CHECK(w::dockSideOf(ui, c.docks[History]) == w::DockSide::Center);
    UI_CHECK(!w::dockSplit(ui, c.docks[Log], c.docks[History], w::DockEdge::Left)); // no splits beside a floating dock
    // a floating dock docks straight into a split (it leaves the floating state)
    UI_CHECK(w::dockSplit(ui, c.docks[History], c.docks[Log], w::DockEdge::Left, 180.f));
    ui.update();
    UI_CHECK(!w::dockFloating(ui, c.docks[History]) && !ui.hasClass(c.docks[History], "dock-floating"));
    UI_CHECK(near(rc(History).w, 180.f, 2.f) && rc(History).right() <= rc(Log).x + 1.f && w::dockSideOf(ui, c.docks[History]) == w::DockSide::Bottom);
    // (log and console were stacked in the bottom region: the split nests around log only)
    UI_CHECK(w::dockSplitOf(ui, c.docks[Log]).valid() && !w::dockSplitOf(ui, c.docks[Console]).valid());

    // dragging a split dock out and pressing Escape puts it back in its pane, with its size
    const std::string before = w::dockSaveLayout(ui, c.area);
    const Rect was = rc(History);
    const Rect title = ui.rect(ui.firstChild(c.docks[History]));
    const Vec2 grab{title.x + 30.f, title.y + title.h * 0.5f};
    ui.pointerMove(grab);
    ui.pointerDown(grab);
    ui.pointerMove({grab.x + 60.f, grab.y - 200.f});
    ui.update(1.f / 60.f);
    UI_CHECK(w::dockDragged(ui) == c.docks[History] && w::dockFloating(ui, c.docks[History]));
    UI_CHECK(!w::dockSplitOf(ui, c.docks[Log]).valid()); // the split collapsed while the dock is out
    UI_CHECK(ui.keyDown(Key::Escape));
    ui.keyUp(Key::Escape);
    ui.pointerUp({grab.x + 60.f, grab.y - 200.f});
    ui.update(1.f / 60.f);
    UI_CHECK(!w::dockFloating(ui, c.docks[History]) && w::dockSplitOf(ui, c.docks[History]).valid());
    UI_CHECK(w::dockSplitOf(ui, c.docks[History]) == w::dockSplitOf(ui, c.docks[Log]));
    UI_CHECK_EQ(w::dockSaveLayout(ui, c.area), before);
    UI_CHECK(near(rc(History).x, was.x) && near(rc(History).y, was.y) && near(rc(History).w, was.w) && near(rc(History).h, was.h));
    // dropping it on an area zone takes it out of the split for good
    ui.pointerMove(grab);
    ui.pointerDown(grab);
    ui.pointerMove({grab.x + 60.f, grab.y - 200.f});
    ui.update(1.f / 60.f);
    const Rect zone = w::dockDropIndicator(ui, c.area, w::DockDropZone::Right);
    const Vec2 zp{zone.x + zone.w * 0.5f, zone.y + zone.h * 0.5f};
    ui.pointerMove(zp);
    ui.update(1.f / 60.f);
    UI_CHECK(w::dockDropZone(ui) == w::DockDropZone::Right);
    ui.pointerUp(zp);
    ui.update(1.f / 60.f);
    UI_CHECK(w::dockSideOf(ui, c.docks[History]) == w::DockSide::Right && !w::dockSplitOf(ui, c.docks[History]).valid());
    UI_CHECK_EQ(countClass(ui, c.area, "docksplit"), 0u);
    // the Tab zone over a dock inside a split joins it there
    UI_CHECK(w::dockSplit(ui, c.docks[Console], c.docks[Log], w::DockEdge::Right));
    ui.update();
    w::dockTabify(ui, c.docks[History], c.docks[Console]);
    ui.update();
    UI_CHECK(ui.parent(c.docks[History]) == ui.parent(c.docks[Console]) && w::dockSplitOf(ui, c.docks[History]) == w::dockSplitOf(ui, c.docks[Log]));
    // removing one of the two tabbed docks dissolves the group and keeps the pane
    w::dockRemove(ui, c.docks[History]);
    ui.update();
    UI_CHECK(!ui.alive(c.docks[History]) && !w::dockTabBar(ui, c.docks[Console]).valid() && w::dockSplitOf(ui, c.docks[Console]).valid());
    UI_CHECK(ui.computed(ui.firstChild(c.docks[Console])).visible); // its title strip is back
}

// ---- splits next to OS tool windows (UiContext::setOsWindows: a floating dock is an "os-window" surface) --------------
void gateSplitToolWindows() {
    Fixture f;
    UiContext& ui = f.ui;
    using PopupSurface = UiContext::PopupSurface;
    ui.setOsWindows(true);
    DockApp c = buildDocks(ui, host(ui), "C");
    ui.update();
    auto surfaceOf = [&](NodeHandle dock) -> const PopupSurface* {
        for (const PopupSurface& ws : ui.windowSurfaces())
            if (ws.node == dock) return &ws;
        return nullptr;
    };
    UI_CHECK(w::dockSplit(ui, c.docks[Assets], c.docks[Outline], w::DockEdge::Bottom));
    UI_CHECK(w::dockSplit(ui, c.docks[History], c.docks[Assets], w::DockEdge::Center));
    UI_CHECK(w::dockSplit(ui, c.docks[Console], c.docks[Log], w::DockEdge::Right, 240.f));
    ui.update();
    UI_CHECK(ui.windowSurfaces().empty() && countClass(ui, c.area, "docksplit") == 2u);
    for (int i = 0; i < 6; ++i) UI_CHECK(!ui.hasClass(c.docks[i], "os-window"));

    // a dock floated out of a split becomes a tool window, outside the viewport if asked; the split collapses
    const Size vp = ui.viewport();
    w::dockFloat(ui, c.docks[Console], {vp.w + 40.f, -20.f, 240.f, 180.f});
    ui.update();
    UI_CHECK(w::dockFloating(ui, c.docks[Console]) && ui.hasClass(c.docks[Console], "os-window"));
    UI_CHECK(!w::dockSplitOf(ui, c.docks[Log]).valid() && countClass(ui, c.area, "docksplit") == 1u);
    const PopupSurface* ws = surfaceOf(c.docks[Console]);
    UI_CHECK(ws && ui.windowSurfaces().size() == 1);
    if (ws) {
        UI_CHECK_RECT(ws->rect, vp.w + 40.f, -20.f, 240.f, 180.f); // not clamped into the main window
        UI_CHECK(ws->draw.size() > 0);
    }
    // a tab-group member floated: the group in the split pane dissolves, the pane and the split stay
    w::dockFloat(ui, c.docks[History], {vp.w + 40.f, 200.f, 220.f, 160.f});
    ui.update();
    UI_CHECK(ui.windowSurfaces().size() == 2 && surfaceOf(c.docks[History]) && !w::dockTabBar(ui, c.docks[Assets]).valid());
    UI_CHECK(w::dockSplitOf(ui, c.docks[Assets]).valid() && w::dockSplitOf(ui, c.docks[Assets]) == w::dockSplitOf(ui, c.docks[Outline]));

    // layout format 2 carries splits and floating docks together; restoring it elsewhere makes tool windows again
    const std::string saved = w::dockSaveLayout(ui, c.area);
    UI_CHECK(saved.rfind("fusedock 2\n", 0) == 0 && saved.find("tree left v[") != std::string::npos);
    DockApp d = buildDocks(ui, host(ui), "D");
    ui.update();
    const std::string savedD = renamed(saved, "C.", "D.");
    UI_CHECK(w::dockRestoreLayout(ui, d.area, savedD));
    ui.update();
    UI_CHECK_EQ(w::dockSaveLayout(ui, d.area), savedD);
    UI_CHECK(w::dockFloating(ui, d.docks[Console]) && ui.hasClass(d.docks[Console], "os-window") && surfaceOf(d.docks[Console]));
    UI_CHECK(w::dockFloating(ui, d.docks[History]) && surfaceOf(d.docks[History]) && ui.windowSurfaces().size() == 4);
    if (const PopupSurface* dw = surfaceOf(d.docks[Console])) UI_CHECK_RECT(dw->rect, vp.w + 40.f, -20.f, 240.f, 180.f);
    UI_CHECK(w::dockSplitOf(ui, d.docks[Assets]).valid());

    // a tool window docks straight into a split (dockSplit), as a tab (Center) or to a side: its surface goes
    UI_CHECK(w::dockSplit(ui, c.docks[Console], c.docks[Log], w::DockEdge::Left, 200.f));
    ui.update();
    UI_CHECK(!w::dockFloating(ui, c.docks[Console]) && !ui.hasClass(c.docks[Console], "os-window") && !surfaceOf(c.docks[Console]));
    UI_CHECK(w::dockSplitOf(ui, c.docks[Console]).valid() && near(ui.rect(c.docks[Console]).w, 200.f, 2.f));
    UI_CHECK(w::dockSplit(ui, c.docks[History], c.docks[Assets], w::DockEdge::Center));
    ui.update();
    UI_CHECK(!ui.hasClass(c.docks[History], "os-window") && !surfaceOf(c.docks[History]) && ui.windowSurfaces().size() == 2);
    UI_CHECK(w::dockTabBar(ui, c.docks[History]).valid() && w::dockSplitOf(ui, c.docks[History]) == w::dockSplitOf(ui, c.docks[Outline]));

    // tearing a split dock off by its title makes a tool window under the pointer; Escape puts it back in its pane
    const std::string before = w::dockSaveLayout(ui, c.area);
    const Rect title = ui.rect(ui.firstChild(c.docks[Console]));
    const Vec2 grab{title.x + 30.f, title.y + title.h * 0.5f};
    ui.pointerMove(grab);
    ui.pointerDown(grab);
    ui.pointerMove({grab.x + 80.f, grab.y - 220.f});
    ui.update(1.f / 60.f);
    UI_CHECK(w::dockDragged(ui) == c.docks[Console] && w::dockFloating(ui, c.docks[Console]));
    UI_CHECK(ui.hasClass(c.docks[Console], "os-window") && surfaceOf(c.docks[Console]));
    UI_CHECK(ui.keyDown(Key::Escape));
    ui.keyUp(Key::Escape);
    ui.pointerUp({grab.x + 80.f, grab.y - 220.f});
    ui.update(1.f / 60.f);
    UI_CHECK(!w::dockFloating(ui, c.docks[Console]) && !ui.hasClass(c.docks[Console], "os-window") && !surfaceOf(c.docks[Console]));
    UI_CHECK_EQ(w::dockSaveLayout(ui, c.area), before);
    // the same drag released over an area zone docks it there (out of the split), again without a surface
    ui.pointerMove(grab);
    ui.pointerDown(grab);
    ui.pointerMove({grab.x + 80.f, grab.y - 220.f});
    ui.update(1.f / 60.f);
    UI_CHECK(surfaceOf(c.docks[Console]) != nullptr);
    const Rect zone = w::dockDropIndicator(ui, c.area, w::DockDropZone::Right);
    const Vec2 zp{zone.x + zone.w * 0.5f, zone.y + zone.h * 0.5f};
    ui.pointerMove(zp);
    ui.update(1.f / 60.f);
    UI_CHECK(w::dockDropZone(ui) == w::DockDropZone::Right);
    ui.pointerUp(zp);
    ui.update(1.f / 60.f);
    UI_CHECK(w::dockSideOf(ui, c.docks[Console]) == w::DockSide::Right && !surfaceOf(c.docks[Console]));
    UI_CHECK(!ui.hasClass(c.docks[Console], "os-window") && ui.windowSurfaces().size() == 2);

    // removing / closing a floating dock of the other area takes its tool window away
    w::dockRemove(ui, d.docks[Console]);
    ui.update();
    UI_CHECK(!ui.alive(d.docks[Console]) && ui.windowSurfaces().size() == 1);
    w::dockClose(ui, d.docks[History]);
    ui.update();
    UI_CHECK(ui.windowSurfaces().empty());
    w::dockOpen(ui, d.docks[History]); // reopens where it was: floating, a tool window again
    ui.update();
    UI_CHECK(w::dockFloating(ui, d.docks[History]) && surfaceOf(d.docks[History]) && ui.windowSurfaces().size() == 1);
    // tool windows off again: the floating dock is an in-window panel
    ui.setOsWindows(false);
    ui.update();
    UI_CHECK(ui.windowSurfaces().empty() && w::dockFloating(ui, d.docks[History]));
}

// ---- tree line geometry ------------------------------------------------------------------------------------------------
void gateTreeLine() {
    UiContext ui({.viewport = {400, 400}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "10px");
    // lines: 0 root, 1 a, 2 a1, 3 a2, 4 b
    const NodeHandle tree = w::treeView(ui, ui.root(), "root\n\ta\n\t\ta1\n\t\ta2\n\tb", "tree");
    ui.setCss(tree, "height", "200px");
    for (u32 l = 0; l < 5; ++l) w::setTreeExpanded(ui, tree, l, true);
    ui.update();
    const Rect c = ui.contentRect(tree);
    UI_CHECK_EQ(w::treeVisibleRows(ui, tree), 5u);
    const Rect r0 = w::treeLineRect(ui, tree, 0), r3 = w::treeLineRect(ui, tree, 3);
    UI_CHECK(r0.h > 10.f && near(r0.x, c.x) && near(r0.y, c.y) && near(r0.w, c.w));
    UI_CHECK(near(r3.y, c.y + 3.f * r0.h) && near(r3.h, r0.h));
    for (u32 l = 0; l < 5; ++l) {
        const Rect r = w::treeLineRect(ui, tree, l);
        UI_CHECK_EQ(w::treeLineAt(ui, tree, {r.x + 20.f, r.y + r.h * 0.5f}), i32(l));
        UI_CHECK_EQ(w::treeLineAt(ui, tree, {r.x + 1.f, r.y + 0.5f}), i32(l));
    }
    // below the last row, and outside the tree
    UI_CHECK_EQ(w::treeLineAt(ui, tree, {c.x + 20.f, c.y + 5.f * r0.h + 4.f}), -1);
    UI_CHECK_EQ(w::treeLineAt(ui, tree, {c.x - 5.f, c.y + 4.f}), -1);
    UI_CHECK_EQ(w::treeLineAt(ui, tree, {c.x + 20.f, c.bottom() + 30.f}), -1);
    UI_CHECK(w::treeLineRect(ui, tree, 99).empty());
    // collapsing "a" hides its children: their rects are empty and b moves up to row 2
    w::setTreeExpanded(ui, tree, 1, false);
    ui.update();
    UI_CHECK(w::treeLineRect(ui, tree, 2).empty() && w::treeLineRect(ui, tree, 3).empty());
    const Rect rb = w::treeLineRect(ui, tree, 4);
    UI_CHECK(near(rb.y, c.y + 2.f * r0.h));
    UI_CHECK_EQ(w::treeLineAt(ui, tree, {rb.x + 20.f, rb.y + 2.f}), 4);
    // a click at the row the helper reports selects that line
    ui.pointerMove({rb.x + 40.f, rb.y + rb.h * 0.5f});
    ui.pointerDown({rb.x + 40.f, rb.y + rb.h * 0.5f});
    ui.pointerUp({rb.x + 40.f, rb.y + rb.h * 0.5f});
    ui.update();
    UI_CHECK_EQ(i32(ui.value(tree)), 4);
    // not a tree: nothing
    const NodeHandle lbl = w::label(ui, ui.root(), "x");
    ui.update();
    UI_CHECK(w::treeLineAt(ui, lbl, {ui.rect(lbl).x + 1.f, ui.rect(lbl).y + 1.f}) == -1 && w::treeLineRect(ui, lbl, 0).empty());
    UI_CHECK(w::treeLineAt(ui, NodeHandle{}, {0, 0}) == -1);
}

// ---- timeline: clip selection, clip-drag events and report, keyboard retime -----------------------------------------
struct TlLog {
    std::vector<int> events;
};
void gateTimelineClip() {
    UiContext ui({.viewport = {800, 300}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "10px");
    const NodeHandle tl = w::timeline(ui, ui.root(), 10.f, "tl");
    ui.setCss(tl, "width", "700px");
    const u32 t0 = w::timelineAddTrack(ui, tl, "pos"), t1 = w::timelineAddTrack(ui, tl, "rot");
    const u32 c0 = w::timelineAddClip(ui, tl, t0, 1.f, 2.f, "a", Color::rgb(0x64B5F6));
    const u32 c1 = w::timelineAddClip(ui, tl, t0, 4.f, 5.f, "b", Color::rgb(0x64B5F6));
    const u32 c2 = w::timelineAddClip(ui, tl, t1, 3.f, 3.5f, "c", Color::rgb(0x81C784));
    w::timelineSetView(ui, tl, 0.f, 10.f);
    w::timelineSetSnap(ui, tl, 0.5f);
    ui.update();
    static TlLog log;
    log.events.clear();
    // (the core also offers every unconsumed input event to Event slots, with value 0: only the widget codes count)
    ui.connect(tl, SignalKind::Event, [](UiContext&, NodeHandle, const SignalArgs& a) { if (a.value != 0.f) log.events.push_back(int(a.value)); });
    const Rect c = ui.contentRect(tl);
    const f32 lane = c.w - 96.f, pps = lane / 10.f; // head 96 px, 10 s in view
    auto at = [&](f32 sec, u32 track) { return Vec2{c.x + 96.f + sec * pps, c.y + 22.f + f32(track) * 28.f + 14.f}; };
    u32 st = 9, sc = 9;
    UI_CHECK(!w::timelineSelectedClip(ui, tl, st, sc) && st == 9);
    UI_CHECK(!w::timelineClipDrag(ui, tl).active);

    // press on clip b: selected, Event 5, the report names it
    ui.pointerMove(at(4.25f, t0));
    ui.pointerDown(at(4.25f, t0));
    ui.update();
    w::TimelineClipDrag d = w::timelineClipDrag(ui, tl);
    UI_CHECK(d.active && d.track == t0 && d.clip == c1 && near(d.downSec, 4.25f, 0.02f) && near(d.pointerSec, 4.25f, 0.02f) && near(d.fromStart, 4.f, 1e-4f));
    UI_CHECK(w::timelineSelectedClip(ui, tl, st, sc) && st == t0 && sc == c1);
    UI_CHECK(log.events == std::vector<int>{5});
    // move by +1.1 s: snapped to 0.5 -> start 5.0; Event 6; the pointer time is unsnapped
    ui.pointerMove(at(5.35f, t0));
    ui.update();
    d = w::timelineClipDrag(ui, tl);
    UI_CHECK(d.active && near(d.pointerSec, 5.35f, 0.02f) && near(d.downSec, 4.25f, 0.02f) && near(d.fromStart, 4.f, 1e-4f));
    UI_CHECK(near(w::timelineClip(ui, tl, t0, c1).x, 5.f, 1e-3f) && near(w::timelineClip(ui, tl, t0, c1).y, 6.f, 1e-3f));
    UI_CHECK(log.events == (std::vector<int>{5, 6}));
    // Shift: free move
    ui.setModifiers(kModShift);
    ui.pointerMove(at(5.6f, t0));
    ui.setModifiers(0);
    ui.update();
    UI_CHECK(near(w::timelineClip(ui, tl, t0, c1).x, 5.35f, 0.03f));
    // release: Event 7, inactive, the report still names the clip
    ui.pointerUp(at(5.6f, t0));
    ui.update();
    d = w::timelineClipDrag(ui, tl);
    UI_CHECK(!d.active && d.track == t0 && d.clip == c1);
    UI_CHECK(log.events == (std::vector<int>{5, 6, 6, 7}));
    UI_CHECK(near(ui.value(tl), 0.f, 1e-4f)); // a clip drag does not scrub

    // a press on the ruler scrubs and emits no clip event
    log.events.clear();
    ui.pointerMove({at(2.f, t0).x, c.y + 8.f});
    ui.pointerDown({at(2.f, t0).x, c.y + 8.f});
    ui.pointerUp({at(2.f, t0).x, c.y + 8.f});
    ui.update();
    UI_CHECK(near(ui.value(tl), 2.f, 0.01f) && log.events.empty() && !w::timelineClipDrag(ui, tl).active);
    UI_CHECK(w::timelineSelectedClip(ui, tl, st, sc) && sc == c1); // the selection stays

    // timelineSelectClip, as a press does; out of range clears
    w::timelineSelectClip(ui, tl, i32(t1), i32(c2));
    UI_CHECK(w::timelineSelectedClip(ui, tl, st, sc) && st == t1 && sc == c2);
    w::timelineSelectClip(ui, tl, 7, 0);
    UI_CHECK(!w::timelineSelectedClip(ui, tl, st, sc));
    w::timelineSelectClip(ui, tl, i32(t0), i32(c0));
    ui.setFocus(tl);
    // Ctrl+Right moves the selected clip one snap step and emits Event 2; the report names it, inactive
    key(ui, Key::Right, kModCtrl);
    UI_CHECK(near(w::timelineClip(ui, tl, t0, c0).x, 1.5f, 1e-3f) && log.events == std::vector<int>{2});
    d = w::timelineClipDrag(ui, tl);
    UI_CHECK(!d.active && d.track == t0 && d.clip == c0);
    key(ui, Key::Left, kModCtrl | kModShift); // a second back, clamped at 0.5
    UI_CHECK(near(w::timelineClip(ui, tl, t0, c0).x, 0.5f, 1e-3f) && log.events.size() == 2);
    UI_CHECK(near(ui.value(tl), 2.f, 0.01f)); // Ctrl+arrows do not scrub
    // Ctrl+Down / Up walk the clips in (track, start) order: a (0.5) -> b (5.35) -> c (track 1) -> stays
    key(ui, Key::Down, kModCtrl);
    UI_CHECK(w::timelineSelectedClip(ui, tl, st, sc) && st == t0 && sc == c1);
    key(ui, Key::Down, kModCtrl);
    UI_CHECK(w::timelineSelectedClip(ui, tl, st, sc) && st == t1 && sc == c2);
    key(ui, Key::Down, kModCtrl);
    UI_CHECK(w::timelineSelectedClip(ui, tl, st, sc) && st == t1 && sc == c2);
    key(ui, Key::Up, kModCtrl);
    UI_CHECK(w::timelineSelectedClip(ui, tl, st, sc) && st == t0 && sc == c1);
    // plain arrows still scrub; Escape clears the selection
    key(ui, Key::Right);
    UI_CHECK(near(ui.value(tl), 2.5f, 0.01f));
    UI_CHECK(ui.keyDown(Key::Escape));
    ui.keyUp(Key::Escape);
    UI_CHECK(!w::timelineSelectedClip(ui, tl, st, sc));
    // without a selection Ctrl+Right falls through to the scrub
    log.events.resize(0);
    key(ui, Key::Right, kModCtrl);
    UI_CHECK(log.events.empty() && near(ui.value(tl), 3.f, 0.01f));
    // not a timeline
    UI_CHECK(!w::timelineClipDrag(ui, ui.root()).active && !w::timelineSelectedClip(ui, ui.root(), st, sc));
}

int demo(const char* path) {
    Fixture f;
    DockApp c = buildDocks(f.ui, host(f.ui), "C");
    f.ui.update();
    buildSplitLayout(f.ui, c);
    w::dockSplit(f.ui, c.docks[Assets], c.docks[History], w::DockEdge::Center);
    shot(f.ui, path);
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    struct Gate { const char* name; void (*fn)(); };
    static const Gate kGates[] = {{"split", &gateSplit}, {"split_groups", &gateSplitGroups}, {"tool_windows", &gateSplitToolWindows},
                                  {"tree_line", &gateTreeLine}, {"timeline_clip", &gateTimelineClip}};
    const char* only = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--gate=", 7) == 0) only = argv[i] + 7;
        else if (std::strncmp(argv[i], "--out=", 6) == 0) return demo(argv[i] + 6);
    }
    bool ran = false;
    for (const Gate& g : kGates)
        if (!only || std::strcmp(only, g.name) == 0) {
            g.fn();
            ran = true;
        }
    if (!ran) {
        std::fprintf(stderr, "unknown gate %s\n", only);
        return 2;
    }
    return finish("fuse_ui_dock_split");
}
