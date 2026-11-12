// fuse_ui_dock_drag: dock drag and drop - tearing a dock off by its title bar or its tab, drop zones with a landing
// preview, docking / tabbing on release, Escape / touch-cancel restoring the origin, mouse and touch.
// `--gate=<name>` runs one group (ctest fuse_ui_dock_drag_<name>); `--out=<png>` writes a drag in progress.
#include "test_util.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
bool inside(const UiContext& ui, NodeHandle n, NodeHandle ancestor) {
    for (; ui.alive(n); n = ui.parent(n))
        if (n == ancestor) return true;
    return false;
}
struct Scene {
    UiContext ui{{.viewport = {900, 640}, .font = &font()}};
    NodeHandle area, outline, history, props, centre, left, right, bottom;
    Scene() {
        w::install(ui);
        area = w::dockArea(ui, ui.root(), "area");
        outline = w::dockWidget(ui, area, "Outline", w::DockSide::Left, "outline");
        history = w::dockWidget(ui, area, "History", w::DockSide::Left, "history");
        props = w::dockWidget(ui, area, "Properties", w::DockSide::Right, "props");
        w::button(ui, w::dockBody(ui, outline), "Expand", "expand");
        w::label(ui, w::dockBody(ui, history), "history body");
        w::label(ui, w::dockBody(ui, props), "props body");
        centre = w::dockRegion(ui, area, w::DockSide::Center);
        left = w::dockRegion(ui, area, w::DockSide::Left);
        right = w::dockRegion(ui, area, w::DockSide::Right);
        bottom = w::dockRegion(ui, area, w::DockSide::Bottom);
        w::label(ui, centre, "centre");
        ui.update();
    }
    Vec2 titleGrab(NodeHandle dock) const {
        const Rect t = ui.rect(ui.firstChild(dock));
        return {t.x + 30.f, t.y + t.h * 0.5f};
    }
    u32 dropLayers() const {
        u32 n = 0;
        for (NodeHandle c = ui.firstChild(w::overlayLayer(const_cast<UiContext&>(ui))); ui.alive(c); c = ui.nextSibling(c))
            n += ui.classOf(c) == ui.registry().find("dockdrop");
        return n;
    }
    void move(Vec2 p) {
        ui.pointerMove(p);
        ui.update(1.f / 60.f);
    }
    void press(Vec2 p) {
        ui.pointerMove(p);
        ui.pointerDown(p);
        ui.update(1.f / 60.f);
    }
    void release(Vec2 p) {
        ui.pointerUp(p);
        ui.update(1.f / 60.f);
    }
    /// Press at `from`, drag to `to` in two steps (the drag stays in progress).
    void dragTo(Vec2 from, Vec2 to) {
        press(from);
        move({(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f});
        move(to);
    }
};

// ---- tearing a docked dock off by its title bar ---------------------------------------------------------------------
void gateTearOff() {
    Scene s;
    UiContext& ui = s.ui;
    const Vec2 g = s.titleGrab(s.outline);
    const Rect docked = ui.rect(s.outline);
    s.press(g);
    UI_CHECK(!w::dockDragged(ui).valid());
    s.move({g.x + 5.f, g.y + 3.f}); // inside the threshold: nothing happens
    UI_CHECK(!w::dockFloating(ui, s.outline) && !w::dockDragged(ui).valid() && ui.parent(s.outline) == s.left);
    UI_CHECK_EQ(s.dropLayers(), 0u);
    const Vec2 p1{g.x + 120.f, g.y + 200.f};
    s.move(p1); // past it: the dock floats under the pointer
    UI_CHECK(w::dockFloating(ui, s.outline) && w::dockDragged(ui) == s.outline);
    UI_CHECK(ui.parent(s.outline) == w::overlayLayer(ui) && ui.parent(s.history) == s.left);
    UI_CHECK_EQ(s.dropLayers(), 1u);
    Rect fr = w::dockFloatRect(ui, s.outline);
    UI_CHECK(fr.w >= 160.f && fr.w <= 420.f && fr.h >= 120.f && fr.h <= 320.f);
    UI_CHECK(ui.rect(ui.firstChild(s.outline)).contains(p1)); // the title bar is still under the pointer
    UI_CHECK(ui.focused() == ui.firstChild(s.outline));       // so Escape reaches the drag
    UI_CHECK(w::dockDropZone(ui) == w::DockDropZone::None && w::dockDropPreview(ui).empty());
    // it follows the pointer
    const Vec2 p2{p1.x + 60.f, p1.y - 40.f};
    s.move(p2);
    const Rect fr2 = w::dockFloatRect(ui, s.outline);
    UI_CHECK_RECT(fr2, fr.x + 60.f, fr.y - 40.f, fr.w, fr.h);
    UI_CHECK_RECT(ui.rect(s.outline), fr2.x, fr2.y, fr2.w, fr2.h);
    // released away from every indicator: it stays floating, the drop layer goes, the focus leaves the title
    s.release(p2);
    UI_CHECK(w::dockFloating(ui, s.outline) && !w::dockDragged(ui).valid());
    UI_CHECK_EQ(s.dropLayers(), 0u);
    UI_CHECK_RECT(w::dockFloatRect(ui, s.outline), fr2.x, fr2.y, fr2.w, fr2.h);
    UI_CHECK(ui.focused() != ui.firstChild(s.outline) && !ui.hasClass(s.outline, "dock-closed"));
    std::vector<NodeHandle> order;
    ui.tabOrder(order);
    for (NodeHandle n : order) UI_CHECK(n != ui.firstChild(s.outline)); // focusable only while dragged
    // the history dock took the whole left region
    UI_CHECK(ui.rect(s.history).h > docked.h * 1.5f);
    // a press without a drag on a title still does nothing; a click on the close box still closes
    s.press(s.titleGrab(s.history));
    s.release(s.titleGrab(s.history));
    UI_CHECK(!w::dockFloating(ui, s.history));
    const Rect ht = ui.rect(ui.firstChild(s.history));
    s.press({ht.right() - ht.h * 0.5f, ht.y + ht.h * 0.5f});
    s.move({ht.right() - ht.h * 0.5f - 40.f, ht.y + ht.h * 0.5f + 60.f}); // a drag from the close box is not a dock drag
    UI_CHECK(!w::dockFloating(ui, s.history) && !w::dockDragged(ui).valid());
    s.release({ht.right() - ht.h * 0.5f - 40.f, ht.y + ht.h * 0.5f + 60.f});
    UI_CHECK(!ui.hasClass(s.history, "dock-closed"));
}

// ---- drop zones -----------------------------------------------------------------------------------------------------
void gateZones() {
    Scene s;
    UiContext& ui = s.ui;
    using Z = w::DockDropZone;
    // tear the outline off and visit every area indicator
    const Vec2 g = s.titleGrab(s.outline);
    s.dragTo(g, {450.f, 200.f});
    UI_CHECK(w::dockDragged(ui) == s.outline && w::dockDropZone(ui) == Z::None);
    UI_CHECK(!ui.computed(s.bottom).visible);
    const Rect a = ui.rect(s.area);
    const Rect il = w::dockDropIndicator(ui, s.area, Z::Left), ir = w::dockDropIndicator(ui, s.area, Z::Right);
    const Rect ib = w::dockDropIndicator(ui, s.area, Z::Bottom), ic = w::dockDropIndicator(ui, s.area, Z::Center);
    UI_CHECK(il.w == 36.f && il.x > a.x && il.right() < a.x + 60.f && std::abs(ctr(il).y - ctr(a).y) <= 1.f);
    UI_CHECK(ir.right() < a.right() && ir.x > a.right() - 60.f && ib.bottom() < a.bottom() && ib.y > a.bottom() - 60.f);
    UI_CHECK(std::abs(ctr(ic).x - ctr(ui.rect(s.centre)).x) <= 1.f);
    s.move(ctr(ib));
    UI_CHECK(w::dockDropZone(ui) == Z::Bottom);
    const Rect pb = w::dockDropPreview(ui); // the bottom region is hidden: the strip it will take
    UI_CHECK(pb.x == a.x && pb.w == a.w && std::abs(pb.bottom() - a.bottom()) <= 1.f && pb.h >= 100.f && pb.h <= a.h * 0.5f);
    s.move(ctr(ic));
    UI_CHECK(w::dockDropZone(ui) == Z::Center);
    UI_CHECK_RECT(w::dockDropPreview(ui), ui.rect(s.centre).x, ui.rect(s.centre).y, ui.rect(s.centre).w, ui.rect(s.centre).h);
    s.move({ctr(ic).x + 60.f, ctr(ic).y}); // off the indicator: no zone
    UI_CHECK(w::dockDropZone(ui) == Z::None && w::dockDropPreview(ui).empty());
    s.move(ctr(ir));
    // the right indicator sits on the Properties dock: unless the tab indicator covers it, the side zone wins there
    UI_CHECK(w::dockDropZone(ui) == Z::Right && !w::dockDropTarget(ui).valid());
    UI_CHECK_RECT(w::dockDropPreview(ui), ui.rect(s.right).x, ui.rect(s.right).y, ui.rect(s.right).w, ui.rect(s.right).h);
    // the drop layer never takes the pointer
    UI_CHECK(inside(ui, ui.hitTest(ctr(ir)), s.outline) || inside(ui, ui.hitTest(ctr(ir)), s.props));
    // release on Right: docked there, after Properties
    s.release(ctr(ir));
    UI_CHECK(!w::dockFloating(ui, s.outline) && ui.parent(s.outline) == s.right && !w::dockDragged(ui).valid());
    UI_CHECK(ui.nextSibling(s.props) == s.outline && ui.computed(s.outline).visible);
    UI_CHECK(inside(ui, ui.hitTest(ctr(ui.rect(s.outline))), s.outline));
    UI_CHECK_EQ(s.dropLayers(), 0u);

    // Bottom: the hidden region shows with the dock in it
    s.dragTo(s.titleGrab(s.history), ctr(w::dockDropIndicator(ui, s.area, Z::Bottom)));
    UI_CHECK(w::dockDropZone(ui) == Z::Bottom);
    s.release(ctr(w::dockDropIndicator(ui, s.area, Z::Bottom)));
    UI_CHECK(ui.parent(s.history) == s.bottom && ui.computed(s.bottom).visible && !ui.computed(s.left).visible);
    UI_CHECK(ui.rect(s.history).w > 600.f);
    // Left (empty and hidden now): the preview is the strip it will take, the dock lands in the region
    s.dragTo(s.titleGrab(s.outline), ctr(w::dockDropIndicator(ui, s.area, Z::Left)));
    UI_CHECK(w::dockDropZone(ui) == Z::Left);
    const Rect pl = w::dockDropPreview(ui);
    UI_CHECK(pl.x == ui.rect(s.area).x && pl.w >= 100.f && pl.w <= ui.rect(s.area).w * 0.5f);
    s.release(ctr(w::dockDropIndicator(ui, s.area, Z::Left)));
    UI_CHECK(ui.parent(s.outline) == s.left && ui.computed(s.left).visible);
    UI_CHECK(std::abs(ui.rect(s.left).w - pl.w) <= 8.f);
    // Center
    s.dragTo(s.titleGrab(s.props), {450.f, 120.f});
    s.move(ctr(w::dockDropIndicator(ui, s.area, Z::Center))); // (the centre grew when the right region emptied)
    UI_CHECK(w::dockDropZone(ui) == Z::Center);
    s.release(ctr(w::dockDropIndicator(ui, s.area, Z::Center)));
    UI_CHECK(ui.parent(s.props) == s.centre && !w::dockFloating(ui, s.props) && !ui.computed(s.right).visible);

    // a floating dock is dragged by its title as before and docks through the same zones
    w::dockFloat(ui, s.props, {320, 120, 240, 180});
    ui.update();
    s.press(s.titleGrab(s.props));
    s.move({s.titleGrab(s.props).x + 4.f, s.titleGrab(s.props).y}); // moves at once (no threshold for a floating dock)
    UI_CHECK_RECT(w::dockFloatRect(ui, s.props), 324, 120, 240, 180);
    UI_CHECK(!w::dockDragged(ui).valid()); // but the zones only appear past it
    s.move(ctr(w::dockDropIndicator(ui, s.area, Z::Right)));
    UI_CHECK(w::dockDragged(ui) == s.props && w::dockDropZone(ui) == Z::Right);
    s.release(ctr(w::dockDropIndicator(ui, s.area, Z::Right)));
    UI_CHECK(ui.parent(s.props) == s.right && !w::dockFloating(ui, s.props));
    // outside the area there are no zones at all
    w::dockFloat(ui, s.props, {320, 120, 240, 180});
    ui.setCss(s.area, "margin-top", "80px");
    ui.update();
    s.dragTo(s.titleGrab(s.props), {450.f, 20.f});
    UI_CHECK(w::dockDragged(ui) == s.props && w::dockDropZone(ui) == Z::None && !ui.rect(s.area).contains({450.f, 20.f}));
    s.release({450.f, 20.f});
    UI_CHECK(w::dockFloating(ui, s.props));
}

// ---- tab zone, tab tear-off -----------------------------------------------------------------------------------------
void gateTabs() {
    Scene s;
    UiContext& ui = s.ui;
    using Z = w::DockDropZone;
    // drop History on "tab with Properties"
    s.dragTo(s.titleGrab(s.history), {450.f, 300.f});
    const Rect it = w::dockDropIndicator(ui, s.area, Z::Tab, s.props);
    UI_CHECK(it.w == 36.f && std::abs(ctr(it).x - ctr(ui.rect(s.props)).x) <= 1.f);
    s.move(ctr(it));
    UI_CHECK(w::dockDropZone(ui) == Z::Tab && w::dockDropTarget(ui) == s.props);
    UI_CHECK_RECT(w::dockDropPreview(ui), ui.rect(s.props).x, ui.rect(s.props).y, ui.rect(s.props).w, ui.rect(s.props).h);
    s.release(ctr(it));
    const NodeHandle strip = w::dockTabBar(ui, s.props);
    UI_CHECK(strip.valid() && w::dockTabBar(ui, s.history) == strip && !w::dockFloating(ui, s.history));
    UI_CHECK(w::dockCurrent(ui, s.props) == s.history && ui.computed(s.history).visible && !ui.computed(s.props).visible);
    UI_CHECK(ui.parent(s.history) == s.right);
    // a third tab: the tab indicator of a tabbed dock covers dock + strip
    s.dragTo(s.titleGrab(s.outline), {450.f, 300.f});
    const Rect it2 = w::dockDropIndicator(ui, s.area, Z::Tab, s.history);
    s.move(ctr(it2));
    UI_CHECK(w::dockDropZone(ui) == Z::Tab && w::dockDropTarget(ui) == s.history);
    UI_CHECK(w::dockDropPreview(ui).y == ui.rect(strip).y && w::dockDropPreview(ui).bottom() == ui.rect(s.history).bottom());
    s.release(ctr(it2));
    UI_CHECK(w::dockTabBar(ui, s.outline) == strip && w::dockCurrent(ui, s.props) == s.outline);
    UI_CHECK(!ui.computed(s.left).visible); // both left docks moved away
    const std::string tabbed = w::dockSaveLayout(ui, s.area);
    UI_CHECK(tabbed.find("tab props 0 0 back") != std::string::npos && tabbed.find("tab history 0 1 back") != std::string::npos);
    UI_CHECK(tabbed.find("tab outline 0 2 current") != std::string::npos);

    // a click on a tab still raises it (press + release without a drag)
    s.press(ctr(w::dockTabRect(ui, s.props)));
    s.release(ctr(w::dockTabRect(ui, s.props)));
    UI_CHECK(w::dockCurrent(ui, s.outline) == s.props && !w::dockDragged(ui).valid());
    const std::string before = w::dockSaveLayout(ui, s.area);

    // dragging the History tab (a back tab) out of the strip tears that dock off; Escape puts it back
    const Vec2 tg = ctr(w::dockTabRect(ui, s.history));
    s.press(tg);
    s.move({tg.x + 4.f, tg.y + 2.f});
    UI_CHECK(!w::dockFloating(ui, s.history));
    s.move({tg.x - 300.f, tg.y + 150.f});
    UI_CHECK(w::dockFloating(ui, s.history) && w::dockDragged(ui) == s.history && !w::dockTabBar(ui, s.history).valid());
    UI_CHECK(w::dockTabBar(ui, s.props) == strip && w::dockCurrent(ui, s.props) == s.props); // the other two stay tabbed
    UI_CHECK(ui.rect(ui.firstChild(s.history)).contains({tg.x - 300.f, tg.y + 150.f}));
    UI_CHECK(ui.computed(ui.firstChild(s.history)).visible); // it has its own title bar again
    ui.keyDown(Key::Escape);
    ui.keyUp(Key::Escape);
    ui.update(1.f / 60.f);
    UI_CHECK(!w::dockDragged(ui).valid() && !w::dockFloating(ui, s.history) && w::dockTabBar(ui, s.history) == strip);
    UI_CHECK_EQ(w::dockSaveLayout(ui, s.area), before); // same region, tab order and current tab
    UI_CHECK_EQ(s.dropLayers(), 0u);
    s.move({tg.x - 200.f, tg.y + 100.f}); // the button is still down: nothing follows the pointer any more
    UI_CHECK(!w::dockFloating(ui, s.history) && !w::dockDragged(ui).valid());
    s.release({tg.x - 200.f, tg.y + 100.f});
    UI_CHECK_EQ(w::dockSaveLayout(ui, s.area), before);

    // tearing the current tab off and leaving it floating: the next tab takes over
    const Vec2 pg = ctr(w::dockTabRect(ui, s.props));
    s.dragTo(pg, {300.f, 260.f});
    UI_CHECK(w::dockFloating(ui, s.props) && w::dockTabBar(ui, s.outline) == strip && w::dockCurrent(ui, s.outline) != s.props);
    s.release({300.f, 260.f});
    UI_CHECK(w::dockFloating(ui, s.props) && ui.alive(strip));
    // a group of two: tearing one off dissolves it; the strip (which holds the press) goes with the release
    const Vec2 og = ctr(w::dockTabRect(ui, s.outline));
    s.dragTo(og, {300.f, 420.f});
    UI_CHECK(w::dockFloating(ui, s.outline) && !w::dockTabBar(ui, s.history).valid());
    UI_CHECK(ui.computed(ui.firstChild(s.history)).visible && ui.computed(s.history).visible);
    s.release({300.f, 420.f});
    UI_CHECK(!ui.alive(strip) && w::dockFloating(ui, s.outline) && ui.parent(s.history) == s.right);
    // ... and Escape on such a drag rebuilds the group with the same order
    w::dockTo(ui, s.outline, w::DockSide::Right);
    w::dockTo(ui, s.props, w::DockSide::Left);
    w::dockTabify(ui, s.history, s.outline); // tabs: outline, history (current: outline)
    ui.update();
    const std::string pair = w::dockSaveLayout(ui, s.area);
    const Vec2 g2 = ctr(w::dockTabRect(ui, s.outline));
    s.dragTo(g2, {420.f, 300.f});
    UI_CHECK(w::dockFloating(ui, s.outline) && !w::dockTabBar(ui, s.history).valid());
    UI_CHECK(w::dockCancelDrag(ui) && !w::dockCancelDrag(ui));
    ui.update();
    if (w::dockSaveLayout(ui, s.area) != pair) std::fprintf(stderr, "want:\n%s\ngot:\n%s\n", pair.c_str(), w::dockSaveLayout(ui, s.area).c_str());
    UI_CHECK_EQ(w::dockSaveLayout(ui, s.area), pair);
    UI_CHECK(w::dockCurrent(ui, s.history) == s.outline);
    s.release({420.f, 300.f});
    UI_CHECK_EQ(w::dockSaveLayout(ui, s.area), pair);
    // a floating dock offers no tab zone (it holds no tabs)
    w::dockFloat(ui, s.props, {300, 200, 240, 200});
    ui.update();
    UI_CHECK(w::dockDropIndicator(ui, s.area, Z::None).empty());
    const NodeHandle log = w::dockWidget(ui, s.area, "Log", w::DockSide::Bottom, "log");
    ui.update();
    s.dragTo(s.titleGrab(log), ctr(ui.rect(s.props)));
    UI_CHECK(w::dockDragged(ui) == log && w::dockDropZone(ui) != Z::Tab && !w::dockDropTarget(ui).valid());
    w::dockCancelDrag(ui);
    s.release(ctr(ui.rect(s.props)));
    UI_CHECK(ui.parent(log) == s.bottom);
}

// ---- Escape restores the origin -------------------------------------------------------------------------------------
void gateEscape() {
    Scene s;
    UiContext& ui = s.ui;
    const std::string before = w::dockSaveLayout(ui, s.area);
    const Rect docked = ui.rect(s.outline);
    // a docked dock that is first in its region goes back first
    s.dragTo(s.titleGrab(s.outline), ctr(w::dockDropIndicator(ui, s.area, w::DockDropZone::Right)));
    UI_CHECK(w::dockDropZone(ui) == w::DockDropZone::Right);
    UI_CHECK(ui.keyDown(Key::Escape));
    ui.keyUp(Key::Escape);
    ui.update(1.f / 60.f);
    UI_CHECK(!w::dockFloating(ui, s.outline) && ui.parent(s.outline) == s.left && ui.nextSibling(s.outline) == s.history);
    UI_CHECK_EQ(w::dockSaveLayout(ui, s.area), before);
    UI_CHECK_RECT(ui.rect(s.outline), docked.x, docked.y, docked.w, docked.h);
    UI_CHECK(!ui.hasClass(s.outline, "dock-floating") && s.dropLayers() == 0u);
    s.release(ctr(w::dockDropIndicator(ui, s.area, w::DockDropZone::Right))); // the release after Escape drops nothing
    UI_CHECK(ui.parent(s.outline) == s.left && !ui.hasClass(s.outline, "dock-closed"));
    UI_CHECK_EQ(w::dockSaveLayout(ui, s.area), before);
    // Escape without a drag is not consumed by the docks
    UI_CHECK(!w::dockCancelDrag(ui));
    // a floating dock goes back to its rect
    w::dockFloat(ui, s.history, {300, 200, 220, 180});
    ui.update();
    s.dragTo(s.titleGrab(s.history), ctr(w::dockDropIndicator(ui, s.area, w::DockDropZone::Center)));
    UI_CHECK(w::dockDropZone(ui) == w::DockDropZone::Center);
    ui.keyDown(Key::Escape);
    ui.update(1.f / 60.f);
    UI_CHECK(w::dockFloating(ui, s.history));
    UI_CHECK_RECT(w::dockFloatRect(ui, s.history), 300, 200, 220, 180);
    s.release(ctr(w::dockDropIndicator(ui, s.area, w::DockDropZone::Center)));
    UI_CHECK(w::dockFloating(ui, s.history));
    // the focus goes back to where it was before the drag
    const NodeHandle btn = ui.find("expand");
    ui.setFocus(btn);
    const Vec2 g = s.titleGrab(s.history);
    ui.pointerMove(g);
    ui.pointerDown(g);
    ui.setFocus(btn); // (a press on a title bar clears the pointer focus; an app may hold it elsewhere)
    s.move({g.x + 40.f, g.y + 40.f});
    UI_CHECK(ui.focused() == ui.firstChild(s.history));
    s.release({g.x + 40.f, g.y + 40.f});
    UI_CHECK(ui.focused() == btn);
}

// ---- touch ----------------------------------------------------------------------------------------------------------
void gateTouch() {
    Scene s;
    UiContext& ui = s.ui;
    const std::string before = w::dockSaveLayout(ui, s.area);
    double t = 0.0;
    auto step = [&](Vec2 p) {
        ui.touchMove(p, t += 0.016);
        ui.update(1.f / 60.f);
    };
    // a finger on a title bar tears the dock off (title bars capture drags: no pan)
    const Vec2 g = s.titleGrab(s.outline);
    ui.touchDown(g, t);
    ui.update(1.f / 60.f);
    step({g.x + 30.f, g.y + 30.f});
    step({g.x + 150.f, g.y + 120.f});
    UI_CHECK(!ui.touchPanning() && w::dockFloating(ui, s.outline) && w::dockDragged(ui) == s.outline);
    const Vec2 ib = ctr(w::dockDropIndicator(ui, s.area, w::DockDropZone::Bottom));
    step(ib);
    UI_CHECK(w::dockDropZone(ui) == w::DockDropZone::Bottom);
    ui.touchUp(ib, t += 0.016);
    ui.update(1.f / 60.f);
    UI_CHECK(ui.parent(s.outline) == s.bottom && !w::dockFloating(ui, s.outline) && !ui.flinging());
    // a cancelled touch (system gesture) restores the origin
    w::dockTo(ui, s.outline, w::DockSide::Left);
    ui.moveTo(s.outline, s.left, s.history);
    ui.update();
    UI_CHECK_EQ(w::dockSaveLayout(ui, s.area), before);
    const Vec2 g2 = s.titleGrab(s.history);
    ui.touchDown(g2, t += 0.5);
    ui.update(1.f / 60.f);
    step({g2.x + 60.f, g2.y + 20.f});
    step(ctr(w::dockDropIndicator(ui, s.area, w::DockDropZone::Right)));
    UI_CHECK(w::dockFloating(ui, s.history) && w::dockDropZone(ui) == w::DockDropZone::Right);
    ui.touchCancel();
    ui.update(1.f / 60.f);
    UI_CHECK(!w::dockFloating(ui, s.history) && !w::dockDragged(ui).valid());
    UI_CHECK_EQ(w::dockSaveLayout(ui, s.area), before);
    // a finger on a tab tears the tab's dock off
    w::dockTabify(ui, s.history, s.outline);
    ui.update();
    const Vec2 tg = ctr(w::dockTabRect(ui, s.history));
    ui.touchDown(tg, t += 0.5);
    ui.update(1.f / 60.f);
    step({tg.x + 40.f, tg.y + 40.f});
    step({tg.x + 300.f, tg.y + 200.f});
    UI_CHECK(!ui.touchPanning() && w::dockFloating(ui, s.history) && !w::dockTabBar(ui, s.outline).valid());
    ui.touchUp({tg.x + 300.f, tg.y + 200.f}, t += 0.016);
    ui.update(1.f / 60.f);
    UI_CHECK(w::dockFloating(ui, s.history) && !w::dockDragged(ui).valid());
    // a tap on a tab still raises it
    w::dockTabify(ui, s.history, s.outline);
    ui.update();
    UI_CHECK(w::dockCurrent(ui, s.outline) == s.outline);
    const Vec2 tap = ctr(w::dockTabRect(ui, s.history));
    ui.touchDown(tap, t += 0.5);
    ui.touchUp(tap, t += 0.05);
    ui.update(1.f / 60.f);
    UI_CHECK(w::dockCurrent(ui, s.outline) == s.history);
}

int demo(const char* out) {
    Scene s;
    UiContext& ui = s.ui;
    w::dockWidget(ui, s.area, "Log", w::DockSide::Bottom, "log");
    ui.update();
    s.dragTo(s.titleGrab(s.outline), {450.f, 200.f});
    s.move(ctr(w::dockDropIndicator(ui, s.area, w::DockDropZone::Tab, s.props)));
    ui.update(0.f);
    Image img;
    render(ui, img);
    return stbi_write_png(out, img.width, img.height, 4, img.pixels.data(), img.width * 4) ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    struct Gate { const char* name; void (*fn)(); };
    static const Gate kGates[] = {{"tear_off", &gateTearOff}, {"zones", &gateZones}, {"tabs", &gateTabs}, {"escape", &gateEscape},
                                  {"touch", &gateTouch}};
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
    return finish("fuse_ui_dock_drag");
}
