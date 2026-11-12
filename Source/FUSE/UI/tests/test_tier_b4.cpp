// fuse_ui_tier_b4: the last desktop pieces of the plan §5 catalogue — the flame-graph chart kind and tabbed / floating
// dock widgets (with their layout save / restore).
// `--gate=<name>` runs one widget's checks (ctest fuse_ui_widget_<name>); `--out=<png>` writes the demo screen.
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
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
bool near(f32 a, f32 b, f32 eps = 0.75f) { return std::abs(a - b) <= eps; }
void click(UiContext& ui, Vec2 p) {
    ui.pointerMove(p);
    ui.pointerDown(p);
    ui.pointerUp(p);
    ui.update(1.f / 60.f);
}
void key(UiContext& ui, Key k) {
    ui.keyDown(k);
    ui.keyUp(k);
    ui.update(1.f / 60.f);
}
bool inside(const UiContext& ui, NodeHandle n, NodeHandle ancestor) {
    for (; ui.alive(n); n = ui.parent(n))
        if (n == ancestor) return true;
    return false;
}
struct Fixture {
    UiContext ui{{.viewport = {900, 640}, .font = &font()}};
    Fixture() { w::install(ui); }
};

// ---- flame graph ---------------------------------------------------------------------------------------------------
constexpr const char* kLongName = "ai_pathfinding_with_a_very_long_span_name";
void setFrame(UiContext& ui, NodeHandle ch) {
    const w::FlameSpan spans[] = {{"frame", 0.f, 100.f, 0},
                                  {"update", 0.f, 60.f, 1},
                                  {"render", 60.f, 40.f, 1, Color::rgb(0x3D8BFD)},
                                  {"physics", 10.f, 30.f, 2},
                                  {kLongName, 45.f, 5.f, 2}};
    w::chartSetFlame(ui, ch, spans);
}

void gateChartFlame() {
    Fixture f;
    UiContext& ui = f.ui;
    ui.setCss(ui.root(), "padding", "10px");
    const NodeHandle ch = w::chart(ui, ui.root(), w::ChartKind::Flame, "flame");
    ui.setCss(ch, "width", "640px");
    ui.setCss(ch, "height", "240px");
    setFrame(ui, ch);
    int changes = 0, submits = 0;
    f32 submitted = -2.f;
    std::string name;
    ui.connect(ch, SignalKind::ValueChanged, [&changes](UiContext&, NodeHandle, const SignalArgs&) { ++changes; });
    ui.connect(ch, SignalKind::Submitted, [&submits, &submitted, &name](UiContext&, NodeHandle, const SignalArgs& a) {
        ++submits;
        submitted = a.value;
        name = a.text;
    });
    ui.update();
    UI_CHECK_EQ(w::chartFlameSpanCount(ui, ch), 5u);
    UI_CHECK_EQ(ui.value(ch), -1.f);
    const Rect c = ui.contentRect(ch);
    Rect r[5];
    for (u32 i = 0; i < 5; ++i) r[i] = w::chartFlameSpanRect(ui, ch, i);
    // the root span covers the whole time range = the full plot width; children are scaled to it
    UI_CHECK(near(r[0].x, c.x) && near(r[0].w, c.w));
    UI_CHECK(near(r[1].x, c.x) && near(r[1].w, c.w * 0.6f));
    UI_CHECK(near(r[2].x, c.x + c.w * 0.6f) && near(r[2].right(), c.right()));
    UI_CHECK(near(r[3].x, c.x + c.w * 0.1f) && near(r[3].w, c.w * 0.3f));
    // stacked by depth, depth 0 at the top, rows of one height, all inside the widget
    UI_CHECK(r[0].y > c.y && r[0].h >= 8.f);
    UI_CHECK(near(r[1].y, r[0].bottom()) && near(r[2].y, r[1].y) && near(r[3].y, r[1].bottom()) && near(r[4].y, r[3].y));
    for (const Rect& x : r) UI_CHECK(near(x.h, r[0].h) && x.bottom() <= c.bottom() + 0.5f);
    UI_CHECK(w::chartFlameSpanRect(ui, ch, 5).empty());
    // hit query
    for (u32 i = 0; i < 5; ++i) UI_CHECK_EQ(w::chartFlameSpanAt(ui, ch, ctr(r[i])), i32(i));
    UI_CHECK_EQ(w::chartFlameSpanAt(ui, ch, {c.x + c.w * 0.8f, r[3].y + 2.f}), -1); // depth 2 is empty under "render"
    UI_CHECK_EQ(w::chartFlameSpanAt(ui, ch, {c.x + 5.f, c.bottom() - 2.f}), -1);
    // hover -> value() = span index, -1 outside the spans
    ui.pointerMove(ctr(r[2]));
    ui.update(1.f / 60.f);
    UI_CHECK_EQ(ui.value(ch), 2.f);
    UI_CHECK_EQ(changes, 1);
    ui.pointerMove({ctr(r[2]).x + 3.f, ctr(r[2]).y}); // same span: no new signal
    UI_CHECK_EQ(changes, 1);
    ui.pointerMove(ctr(r[3]));
    UI_CHECK_EQ(ui.value(ch), 3.f);
    ui.pointerMove({c.x + 5.f, c.bottom() - 2.f});
    UI_CHECK_EQ(ui.value(ch), -1.f);
    ui.pointerMove(ctr(r[1]));
    UI_CHECK_EQ(ui.value(ch), 1.f);
    ui.pointerMove({850.f, 600.f}); // left the chart
    UI_CHECK_EQ(ui.value(ch), -1.f);
    // click -> Submitted {index, name}; a click on empty plot submits nothing
    click(ui, ctr(r[3]));
    UI_CHECK_EQ(submits, 1);
    UI_CHECK_EQ(submitted, 3.f);
    UI_CHECK(name == "physics");
    click(ui, {c.x + 5.f, c.bottom() - 2.f});
    UI_CHECK_EQ(submits, 1);
    click(ui, ctr(r[4]));
    UI_CHECK_EQ(submits, 2);
    UI_CHECK(name == kLongName);
    // labels stay in their bars: the long name is wider than its bar, so it is cut short (elided) and painted under
    // a clip of the bar; no glyph of it lies outside
    ui.pointerMove({850.f, 600.f});
    ui.update(1.f / 60.f);
    UI_CHECK(ui.measureString(kLongName, ui.computed(ch).fontSize * 0.8f) > r[4].w);
    std::vector<Rect> clips;
    bool sawBarClip = false, glyphOutside = false, explicitColour = false;
    std::size_t glyphs = 0;
    for (const DrawCmd& d : ui.drawList().cmds()) {
        if (d.op == DrawOp::PushClip) clips.push_back(d.rect);
        else if (d.op == DrawOp::PopClip) clips.pop_back();
        else if (d.op == DrawOp::Rect && near(d.rect.x, r[2].x) && near(d.rect.y, r[2].y)) // widget rects carry no node index
            explicitColour = d.fill == Color::rgb(0x3D8BFD);
        else if (d.node == ch.index && d.op == DrawOp::Glyph && !clips.empty() && near(clips.back().x, r[4].x) && near(clips.back().y, r[4].y)) {
            sawBarClip = near(clips.back().w, r[4].w - 1.f, 1.f);
            ++glyphs;
            if (d.rect.x < r[4].x || d.rect.right() > r[4].right() + 0.5f) glyphOutside = true;
        }
    }
    UI_CHECK(sawBarClip && !glyphOutside);
    UI_CHECK(glyphs > 0 && glyphs < std::strlen(kLongName));
    UI_CHECK(explicitColour);
    // new data: rescaled to the new range (which need not start at 0), hover reset
    ui.pointerMove(ctr(r[1]));
    UI_CHECK_EQ(ui.value(ch), 1.f);
    const w::FlameSpan spans2[] = {{"a", 50.f, 150.f, 0}, {"b", 200.f, 50.f, 0}, {"c", 200.f, 25.f, 1}};
    w::chartSetFlame(ui, ch, spans2);
    ui.update(1.f / 60.f);
    UI_CHECK_EQ(w::chartFlameSpanCount(ui, ch), 3u);
    UI_CHECK_EQ(ui.value(ch), -1.f);
    const Rect a = w::chartFlameSpanRect(ui, ch, 0), b = w::chartFlameSpanRect(ui, ch, 1), cc = w::chartFlameSpanRect(ui, ch, 2);
    UI_CHECK(near(a.x, c.x) && near(a.w, c.w * 0.75f) && near(b.x, a.right()) && near(b.right(), c.right()));
    UI_CHECK(near(cc.x, b.x) && near(cc.w, c.w * 0.125f) && near(cc.y, b.bottom()));
    // the series kinds are untouched by the flame API
    const NodeHandle line = w::chart(ui, ui.root(), w::ChartKind::Line, "line");
    UI_CHECK_EQ(w::chartFlameSpanCount(ui, line), 0u);
    UI_CHECK(w::chartFlameSpanRect(ui, line, 0).empty());
    UI_CHECK_EQ(w::chartFlameSpanAt(ui, line, {20.f, 20.f}), -1);
}

// ---- tabbed docks --------------------------------------------------------------------------------------------------
void gateDockTabs() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle area = w::dockArea(ui, ui.root(), "area");
    const NodeHandle outline = w::dockWidget(ui, area, "Outline", w::DockSide::Left, "outline");
    const NodeHandle assets = w::dockWidget(ui, area, "Assets", w::DockSide::Left, "assets");
    const NodeHandle history = w::dockWidget(ui, area, "History", w::DockSide::Right, "history");
    const NodeHandle props = w::dockWidget(ui, area, "Properties", w::DockSide::Right, "props");
    w::label(ui, w::dockBody(ui, outline), "outline body", "outline.body");
    w::label(ui, w::dockBody(ui, assets), "assets body", "assets.body");
    const NodeHandle hb = w::button(ui, w::dockBody(ui, history), "Undo", "history.undo");
    w::label(ui, w::dockRegion(ui, area, w::DockSide::Center), "centre");
    const NodeHandle left = w::dockRegion(ui, area, w::DockSide::Left), right = w::dockRegion(ui, area, w::DockSide::Right);
    ui.update();
    // untabbed docks of a region stack
    UI_CHECK(ui.computed(outline).visible && ui.computed(assets).visible);
    UI_CHECK(ui.rect(assets).y >= ui.rect(outline).bottom());
    UI_CHECK(!w::dockTabBar(ui, outline).valid());
    UI_CHECK(w::dockCurrent(ui, outline) == outline);
    UI_CHECK(w::dockTabRect(ui, outline).empty());

    w::dockTabify(ui, assets, outline);
    w::dockTabify(ui, history, outline); // comes over from the right region
    w::dockTabify(ui, outline, outline); // no-op
    w::dockTabify(ui, assets, outline);  // already tabbed: no-op
    ui.update();
    const NodeHandle strip = w::dockTabBar(ui, outline);
    UI_CHECK(ui.alive(strip));
    UI_CHECK(w::dockTabBar(ui, assets) == strip && w::dockTabBar(ui, history) == strip);
    UI_CHECK(!w::dockTabBar(ui, props).valid());
    UI_CHECK(ui.parent(history) == left && ui.parent(strip) == left);
    // only the current dock's body shows, under the strip, filling the region; title bars give way to the tabs
    for (NodeHandle d : {outline, assets, history}) UI_CHECK(w::dockCurrent(ui, d) == outline);
    UI_CHECK(ui.computed(outline).visible && !ui.computed(assets).visible && !ui.computed(history).visible);
    UI_CHECK(!ui.computed(ui.firstChild(outline)).visible);
    UI_CHECK(inside(ui, ui.hitTest(ctr(ui.rect(ui.find("outline.body")))), outline));
    const Rect sr = ui.rect(strip), lr = ui.rect(left);
    UI_CHECK(near(sr.y, lr.y) && near(sr.w, lr.w) && sr.h > 10.f);
    UI_CHECK(ui.rect(outline).y >= sr.bottom() && near(ui.rect(outline).bottom(), lr.bottom()));
    UI_CHECK(near(ui.rect(w::dockBody(ui, outline)).y, ui.rect(outline).y)); // no title strip above the body
    // one tab per member, in tab order, inside the strip
    const Rect t0 = w::dockTabRect(ui, outline), t1 = w::dockTabRect(ui, assets), t2 = w::dockTabRect(ui, history);
    UI_CHECK(!t0.empty() && !t1.empty() && !t2.empty());
    UI_CHECK(near(t0.x, sr.x) && near(t1.x, t0.right()) && near(t2.x, t1.right()) && t2.right() <= sr.right());
    UI_CHECK_EQ(ui.value(strip), 0.f);

    int raised = 0, closed = 0, opened = 0, undo = 0;
    f32 tab = -2.f;
    ui.connect(strip, SignalKind::ValueChanged, [&raised, &tab](UiContext&, NodeHandle, const SignalArgs& a) { ++raised; tab = a.value; });
    for (NodeHandle d : {outline, assets, history})
        ui.connect(d, SignalKind::Toggled, [&closed, &opened](UiContext&, NodeHandle, const SignalArgs& a) { ++(a.flag ? opened : closed); });
    ui.connect(hb, SignalKind::Clicked, [&undo](UiContext&, NodeHandle, const SignalArgs&) { ++undo; });
    // clicking a tab raises its dock
    click(ui, ctr(t2));
    UI_CHECK(w::dockCurrent(ui, outline) == history);
    UI_CHECK_EQ(raised, 1);
    UI_CHECK_EQ(tab, 2.f);
    UI_CHECK(ui.computed(history).visible && !ui.computed(outline).visible && !ui.computed(assets).visible);
    UI_CHECK(near(ui.rect(history).bottom(), lr.bottom()) && near(ui.rect(history).x, lr.x));
    click(ui, ctr(ui.rect(hb))); // the raised dock's content takes input
    UI_CHECK_EQ(undo, 1);
    click(ui, ctr(t2)); // already current
    UI_CHECK_EQ(raised, 1);
    // keyboard on the focused strip (the click focused it)
    UI_CHECK(ui.focused() == strip);
    key(ui, Key::Left);
    UI_CHECK(w::dockCurrent(ui, outline) == assets);
    key(ui, Key::Right);
    key(ui, Key::Right); // wraps
    UI_CHECK(w::dockCurrent(ui, outline) == outline);
    UI_CHECK_EQ(raised, 4);
    // programmatic raise
    w::dockRaise(ui, assets);
    ui.update();
    UI_CHECK(w::dockCurrent(ui, history) == assets && ui.computed(assets).visible && !ui.computed(outline).visible);
    UI_CHECK_EQ(ui.value(strip), 1.f);
    // the strip's close box closes the current dock; the first open member takes over, the closed one loses its tab
    click(ui, {ui.rect(strip).right() - ui.rect(strip).h * 0.5f, ctr(ui.rect(strip)).y});
    UI_CHECK_EQ(closed, 1);
    UI_CHECK(ui.hasClass(assets, "dock-closed") && !ui.computed(assets).visible);
    UI_CHECK(w::dockCurrent(ui, assets) == outline && ui.computed(outline).visible);
    UI_CHECK(w::dockTabRect(ui, assets).empty());
    UI_CHECK(near(w::dockTabRect(ui, history).x, w::dockTabRect(ui, outline).right())); // history's tab moved up
    UI_CHECK(w::dockTabBar(ui, assets) == strip); // still a member
    // raising a closed member re-opens it
    w::dockRaise(ui, assets);
    ui.update();
    UI_CHECK_EQ(opened, 1);
    UI_CHECK(w::dockCurrent(ui, outline) == assets && ui.computed(assets).visible && !w::dockTabRect(ui, assets).empty());
    // every member closed: no strip, the region hides
    for (NodeHandle d : {outline, assets, history}) w::dockClose(ui, d);
    ui.update();
    UI_CHECK_EQ(closed, 4);
    UI_CHECK(!w::dockCurrent(ui, outline).valid());
    UI_CHECK(!ui.computed(left).visible && !ui.computed(strip).visible);
    w::dockRaise(ui, history);
    ui.update();
    UI_CHECK(ui.computed(left).visible && ui.computed(strip).visible && ui.computed(history).visible);
    UI_CHECK(w::dockTabRect(ui, outline).empty() && !w::dockTabRect(ui, history).empty());
    w::dockRaise(ui, outline);
    w::dockRaise(ui, assets);
    ui.update();
    // dockTo takes a dock out of its group: it gets its title bar back and stacks in the target region
    w::dockTo(ui, history, w::DockSide::Right);
    ui.update();
    UI_CHECK(!w::dockTabBar(ui, history).valid() && ui.parent(history) == right);
    UI_CHECK(ui.computed(history).visible && ui.computed(ui.firstChild(history)).visible);
    UI_CHECK(ui.rect(history).y >= ui.rect(props).bottom());
    UI_CHECK(w::dockTabBar(ui, outline) == strip && w::dockCurrent(ui, outline) == assets);
    UI_CHECK(w::dockTabRect(ui, history).empty());
    // a group left with one dock dissolves
    w::dockTo(ui, assets, w::DockSide::Left);
    ui.update();
    UI_CHECK(!ui.alive(strip));
    UI_CHECK(!w::dockTabBar(ui, outline).valid() && !w::dockTabBar(ui, assets).valid());
    UI_CHECK(ui.computed(outline).visible && ui.computed(assets).visible);
    UI_CHECK(ui.computed(ui.firstChild(outline)).visible && ui.computed(ui.firstChild(assets)).visible);
    UI_CHECK(ui.rect(assets).y >= ui.rect(outline).bottom());
    // tabbing with a closed dock re-opens the one that joins; docks of another area are refused
    w::dockClose(ui, props);
    w::dockTabify(ui, props, history);
    ui.update();
    UI_CHECK(!ui.hasClass(props, "dock-closed") && w::dockTabBar(ui, props) == w::dockTabBar(ui, history));
    UI_CHECK(w::dockCurrent(ui, props) == history && !ui.computed(props).visible);
    const NodeHandle area2 = w::dockArea(ui, ui.root(), "area2");
    const NodeHandle other = w::dockWidget(ui, area2, "Other", w::DockSide::Left, "other");
    w::dockTabify(ui, other, outline);
    w::dockTabify(ui, outline, other);
    ui.update();
    UI_CHECK(!w::dockTabBar(ui, other).valid() && !w::dockTabBar(ui, outline).valid());
    UI_CHECK(ui.parent(other) == w::dockRegion(ui, area2, w::DockSide::Left));
}

// ---- floating docks ------------------------------------------------------------------------------------------------
void drag(UiContext& ui, Vec2 from, Vec2 to) {
    ui.pointerMove(from);
    ui.pointerDown(from);
    ui.pointerMove({(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f});
    ui.pointerMove(to);
    ui.update(1.f / 60.f);
    ui.pointerUp(to);
    ui.update(1.f / 60.f);
}
void gateDockFloat() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle area = w::dockArea(ui, ui.root(), "area");
    const NodeHandle outline = w::dockWidget(ui, area, "Outline", w::DockSide::Left, "outline");
    const NodeHandle props = w::dockWidget(ui, area, "Properties", w::DockSide::Right, "props");
    const NodeHandle btn = w::button(ui, w::dockBody(ui, outline), "Expand", "expand");
    w::label(ui, w::dockBody(ui, props), "props body");
    const NodeHandle centre = w::dockRegion(ui, area, w::DockSide::Center);
    w::label(ui, centre, "centre");
    const NodeHandle left = w::dockRegion(ui, area, w::DockSide::Left);
    int clicks = 0, closed = 0, opened = 0;
    ui.connect(btn, SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
    ui.connect(outline, SignalKind::Toggled, [&closed, &opened](UiContext&, NodeHandle, const SignalArgs& a) { ++(a.flag ? opened : closed); });
    ui.update();
    UI_CHECK(!w::dockFloating(ui, outline) && w::dockFloatRect(ui, outline).empty());
    UI_CHECK(ui.computed(left).visible && near(ui.rect(centre).x, ui.rect(left).right(), 8.f));
    const f32 dockedW = ui.rect(outline).w, dockedH = ui.rect(outline).h;

    // floating: leaves the region (which hides, the centre takes the room) and sits in the overlay layer at the rect
    w::dockFloat(ui, outline, {300, 200, 220, 180});
    ui.update();
    UI_CHECK(w::dockFloating(ui, outline) && !w::dockFloating(ui, props));
    UI_CHECK(ui.parent(outline) == w::overlayLayer(ui));
    UI_CHECK_RECT(ui.rect(outline), 300, 200, 220, 180);
    UI_CHECK_RECT(w::dockFloatRect(ui, outline), 300, 200, 220, 180);
    UI_CHECK(!ui.computed(left).visible && near(ui.rect(centre).x, ui.rect(area).x));
    UI_CHECK(ui.computed(outline).visible && ui.computed(ui.firstChild(outline)).visible);
    UI_CHECK(inside(ui, ui.hitTest({400, 330}), outline)); // over the window content
    UI_CHECK(!inside(ui, ui.hitTest({600, 330}), outline));
    click(ui, ctr(ui.rect(btn)));
    UI_CHECK_EQ(clicks, 1);
    UI_CHECK(inside(ui, btn, outline) && ui.rect(btn).x >= 300.f && ui.rect(btn).right() <= 520.f);

    // dragging the title bar moves it
    const NodeHandle title = ui.firstChild(outline);
    const Rect tr = ui.rect(title);
    const Vec2 grab{tr.x + 30.f, tr.y + tr.h * 0.5f};
    drag(ui, grab, {grab.x + 50.f, grab.y + 30.f});
    UI_CHECK_RECT(ui.rect(outline), 350, 230, 220, 180);
    UI_CHECK_RECT(w::dockFloatRect(ui, outline), 350, 230, 220, 180);
    UI_CHECK(!ui.hasClass(outline, "dock-closed")); // a drag is not a click on the close box
    // dragging the body does not move it
    drag(ui, {360.f, 400.f}, {460.f, 300.f});
    UI_CHECK_RECT(ui.rect(outline), 350, 230, 220, 180);
    // the title bar cannot be dragged out of reach
    drag(ui, {grab.x + 50.f, grab.y + 30.f}, {-2000.f, -2000.f});
    UI_CHECK(ui.rect(outline).y == 0.f && ui.rect(outline).right() == 40.f);
    // (grab the part of the title still on screen, left of the close box)
    drag(ui, {ui.rect(title).right() - ui.rect(title).h - 6.f, ctr(ui.rect(title)).y}, {5000.f, 5000.f});
    UI_CHECK(ui.rect(outline).x == 900.f - 40.f && ui.rect(outline).y == 640.f - 24.f);
    const Vec2 g2{ui.rect(title).x + 10.f, ctr(ui.rect(title)).y};
    drag(ui, g2, {300.f + g2.x - ui.rect(outline).x, 200.f + g2.y - ui.rect(outline).y});
    UI_CHECK_RECT(ui.rect(outline), 300, 200, 220, 180);

    // two floating docks: the later one is in front; pressing a title bar brings its dock to the front
    w::dockFloat(ui, props, {400, 260, 240, 200});
    ui.update();
    UI_CHECK(w::dockFloating(ui, props));
    UI_CHECK(!ui.computed(w::dockRegion(ui, area, w::DockSide::Right)).visible);
    UI_CHECK(inside(ui, ui.hitTest({450, 300}), props));
    click(ui, {ui.rect(title).x + 20.f, ctr(ui.rect(title)).y});
    UI_CHECK(inside(ui, ui.hitTest({450, 300}), outline));
    w::dockRaise(ui, props);
    ui.update();
    UI_CHECK(inside(ui, ui.hitTest({450, 300}), props));
    w::dockTabify(ui, outline, props); // a floating dock hosts no tabs
    UI_CHECK(!w::dockTabBar(ui, props).valid() && w::dockFloating(ui, outline));
    // re-floating moves / resizes; sizes have a floor
    w::dockFloat(ui, props, {20, 30, 10, 10});
    ui.update();
    UI_CHECK_RECT(ui.rect(props), 20, 30, 80, 60);

    // the close box closes a floating dock; dockTo docks it again (and re-opens it)
    const Rect tr2 = ui.rect(title);
    click(ui, {tr2.right() - tr2.h * 0.5f, tr2.y + tr2.h * 0.5f});
    UI_CHECK_EQ(closed, 1);
    UI_CHECK(!ui.computed(outline).visible && w::dockFloating(ui, outline));
    w::dockTo(ui, outline, w::DockSide::Left);
    ui.update();
    UI_CHECK_EQ(opened, 1);
    UI_CHECK(!w::dockFloating(ui, outline) && ui.parent(outline) == left && !ui.hasClass(outline, "dock-floating"));
    UI_CHECK(ui.computed(left).visible && ui.computed(outline).visible);
    UI_CHECK(near(ui.rect(outline).w, dockedW) && near(ui.rect(outline).h, dockedH) && near(ui.rect(outline).x, ui.rect(left).x));
    click(ui, ctr(ui.rect(btn)));
    UI_CHECK_EQ(clicks, 2);
    // a docked title bar does not move within the drag threshold (past it the dock tears off: fuse_ui_dock_drag)
    const Rect before = ui.rect(outline);
    drag(ui, {ui.rect(title).x + 20.f, ctr(ui.rect(title)).y}, {ui.rect(title).x + 25.f, ctr(ui.rect(title)).y + 4.f});
    UI_CHECK(ui.rect(outline) == before && !w::dockFloating(ui, outline));
    // floating a tabbed dock takes it out of its group
    w::dockTo(ui, props, w::DockSide::Left);
    w::dockTabify(ui, props, outline);
    ui.update();
    UI_CHECK(w::dockTabBar(ui, props).valid());
    w::dockFloat(ui, props, {500, 100, 200, 150});
    ui.update();
    UI_CHECK(!w::dockTabBar(ui, props).valid() && !w::dockTabBar(ui, outline).valid());
    UI_CHECK(ui.computed(ui.firstChild(outline)).visible && ui.computed(ui.firstChild(props)).visible);
    UI_CHECK_RECT(ui.rect(props), 500, 100, 200, 150);
}

// ---- layout save / restore with tabs and floating docks ------------------------------------------------------------
constexpr const char* kIds[5] = {"outline", "assets", "history", "log", "props"};
struct Area {
    NodeHandle area, docks[5];
};
Area buildArea(UiContext& ui, const std::string& prefix) {
    Area a;
    a.area = w::dockArea(ui, ui.root(), prefix + "area");
    const w::DockSide sides[5] = {w::DockSide::Left, w::DockSide::Left, w::DockSide::Right, w::DockSide::Bottom, w::DockSide::Right};
    for (u32 i = 0; i < 5; ++i) {
        a.docks[i] = w::dockWidget(ui, a.area, kIds[i], sides[i], prefix + kIds[i]);
        w::label(ui, w::dockBody(ui, a.docks[i]), kIds[i]);
    }
    w::label(ui, w::dockRegion(ui, a.area, w::DockSide::Center), "viewport");
    return a;
}
std::string swapPrefix(std::string s, const std::string& from, const std::string& to) {
    for (std::size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
    return s;
}
void gateDockLayout() {
    Fixture f;
    UiContext& ui = f.ui;
    const Area a = buildArea(ui, "A."), b = buildArea(ui, "B.");
    ui.update();
    // a layout without tabs or floating docks is written exactly as before these features existed
    const std::string v1 = "fusedock 1\nsize left 200\nsize right 200\nsize bottom 160\n"
                           "dock A.outline left 0 open\ndock A.assets left 1 open\ndock A.history right 0 open\n"
                           "dock A.props right 1 open\ndock A.log bottom 0 open\n";
    UI_CHECK_EQ(w::dockSaveLayout(ui, a.area), v1);

    w::dockTabify(ui, a.docks[1], a.docks[0]); // assets + outline
    w::dockTabify(ui, a.docks[2], a.docks[0]); // history joins from the right
    w::dockRaise(ui, a.docks[1]);
    w::dockClose(ui, a.docks[2]);                        // a closed tab member
    w::dockFloat(ui, a.docks[4], {500, 120, 240, 200});  // props floats
    w::dockFloat(ui, a.docks[3], {120, 90, 260, 140});   // log floats in front of it ...
    w::dockClose(ui, a.docks[3]);                        // ... and is closed
    ui.update();
    const std::string saved = w::dockSaveLayout(ui, a.area);
    UI_CHECK(saved.rfind("fusedock 1\n", 0) == 0);
    for (const char* line : {"dock A.outline left 0 open\n", "dock A.assets left 1 open\n", "dock A.history left 2 closed\n",
                             "tab A.outline 0 0 back\n", "tab A.assets 0 1 current\n", "tab A.history 0 2 back\n",
                             "float A.props 500 120 240 200 open\n", "float A.log 120 90 260 140 closed\n"})
        if (saved.find(line) == std::string::npos) {
            std::fprintf(stderr, "missing layout line: %s", line);
            UI_CHECK(false);
        }
    UI_CHECK(saved.find("dock A.props") == std::string::npos && saved.find("dock A.log") == std::string::npos);
    UI_CHECK(saved.find("float A.props") < saved.find("float A.log")); // back to front

    // restore into a second, default-laid-out area
    const std::string blob = swapPrefix(saved, "A.", "B.");
    int toggles = 0;
    ui.connect(b.docks[0], SignalKind::Toggled, [&toggles](UiContext&, NodeHandle, const SignalArgs&) { ++toggles; });
    UI_CHECK(w::dockRestoreLayout(ui, b.area, blob));
    ui.update();
    UI_CHECK_EQ(toggles, 0); // outline stayed open: no close / open flicker
    UI_CHECK_EQ(w::dockSaveLayout(ui, b.area), blob);
    const NodeHandle strip = w::dockTabBar(ui, b.docks[0]);
    UI_CHECK(ui.alive(strip) && w::dockTabBar(ui, b.docks[1]) == strip && w::dockTabBar(ui, b.docks[2]) == strip);
    UI_CHECK(w::dockCurrent(ui, b.docks[0]) == b.docks[1]);
    UI_CHECK(ui.computed(b.docks[1]).visible && !ui.computed(b.docks[0]).visible && !ui.computed(b.docks[2]).visible);
    UI_CHECK(ui.hasClass(b.docks[2], "dock-closed") && w::dockTabRect(ui, b.docks[2]).empty());
    UI_CHECK(ui.parent(b.docks[2]) == w::dockRegion(ui, b.area, w::DockSide::Left));
    UI_CHECK(w::dockFloating(ui, b.docks[4]) && w::dockFloating(ui, b.docks[3]));
    UI_CHECK_RECT(ui.rect(b.docks[4]), 500, 120, 240, 200);
    UI_CHECK_RECT(w::dockFloatRect(ui, b.docks[3]), 120, 90, 260, 140);
    UI_CHECK(ui.computed(b.docks[4]).visible && !ui.computed(b.docks[3]).visible && ui.hasClass(b.docks[3], "dock-closed"));
    UI_CHECK(!ui.computed(w::dockRegion(ui, b.area, w::DockSide::Right)).visible);
    UI_CHECK(!ui.computed(w::dockRegion(ui, b.area, w::DockSide::Bottom)).visible);
    // same geometry as the source area for what is docked (the two areas are stacked: compare relative to the area)
    for (u32 i = 0; i < 3; ++i) {
        UI_CHECK_EQ(ui.computed(a.docks[i]).visible, ui.computed(b.docks[i]).visible);
        const Rect x = ui.rect(a.docks[i]), y = ui.rect(b.docks[i]);
        if (ui.computed(a.docks[i]).visible)
            UI_CHECK(near(x.x - ui.rect(a.area).x, y.x - ui.rect(b.area).x) && near(x.y - ui.rect(a.area).y, y.y - ui.rect(b.area).y) &&
                     near(x.w, y.w) && near(x.h, y.h));
    }
    // restoring twice is stable
    UI_CHECK(w::dockRestoreLayout(ui, b.area, blob));
    ui.update();
    UI_CHECK_EQ(w::dockSaveLayout(ui, b.area), blob);
    UI_CHECK(w::dockTabBar(ui, b.docks[0]).valid() && w::dockFloating(ui, b.docks[4]));

    // malformed tab / float lines change nothing
    for (const char* bad : {"fusedock 1\ntab B.assets x 0 current\n", "fusedock 1\ntab B.assets 0 0 front\n", "fusedock 1\ntab B.assets 0 0\n",
                            "fusedock 1\ntab B.assets 0 -1 back\n", "fusedock 1\nfloat B.props 1 2 3\n",
                            "fusedock 1\nfloat B.props 1 2 -3 4 open\n", "fusedock 1\nfloat B.props 1 2 300 abc open\n",
                            "fusedock 1\nfloat B.props 1 2 300 200 ajar\n", "fusedock 1\ndock B.props left 0 open\nfloat B.props 1 2 0 200 open\n"}) {
        UI_CHECK(!w::dockRestoreLayout(ui, b.area, bad));
        ui.update();
        UI_CHECK_EQ(w::dockSaveLayout(ui, b.area), blob);
    }
    // names with no dock are ignored
    UI_CHECK(w::dockRestoreLayout(ui, b.area, "fusedock 1\ntab B.nothere 0 0 current\ntab B.gone 0 1 back\nfloat B.nothere 1 2 300 200 open\n"));
    ui.update();
    UI_CHECK_EQ(w::dockSaveLayout(ui, b.area), blob);

    // a blob written before tabs / floating existed still restores, and brings every named dock back to plain docking
    const std::string old = swapPrefix(v1, "A.", "B.");
    UI_CHECK(w::dockRestoreLayout(ui, b.area, old));
    ui.update();
    UI_CHECK_EQ(w::dockSaveLayout(ui, b.area), old);
    UI_CHECK(!ui.alive(strip));
    for (NodeHandle d : b.docks) {
        UI_CHECK(!w::dockFloating(ui, d) && !w::dockTabBar(ui, d).valid());
        UI_CHECK(ui.computed(d).visible && ui.computed(ui.firstChild(d)).visible && !ui.hasClass(d, "dock-floating"));
    }
    UI_CHECK(ui.parent(b.docks[2]) == w::dockRegion(ui, b.area, w::DockSide::Right));
    UI_CHECK(ui.parent(b.docks[3]) == w::dockRegion(ui, b.area, w::DockSide::Bottom));
    UI_CHECK(ui.rect(b.docks[1]).y >= ui.rect(b.docks[0]).bottom());
    UI_CHECK(near(ui.rect(b.docks[3]).w, ui.rect(b.area).w));
    // and back again: the tabbed / floating layout over the plain one
    UI_CHECK(w::dockRestoreLayout(ui, b.area, blob));
    ui.update();
    UI_CHECK_EQ(w::dockSaveLayout(ui, b.area), blob);
    // a hand-written blob may regroup: tab order and current come from the tab lines
    UI_CHECK(w::dockRestoreLayout(ui, b.area, "fusedock 1\ndock B.props right 0 open\ndock B.log right 1 open\ndock B.outline right 2 open\n"
                                              "tab B.log 7 0 back\ntab B.outline 7 2 current\ntab B.props 7 1 back\n"));
    ui.update();
    const NodeHandle s2 = w::dockTabBar(ui, b.docks[3]);
    UI_CHECK(ui.alive(s2) && w::dockTabBar(ui, b.docks[0]) == s2 && w::dockTabBar(ui, b.docks[4]) == s2);
    UI_CHECK(w::dockCurrent(ui, b.docks[3]) == b.docks[0] && !w::dockFloating(ui, b.docks[4]));
    UI_CHECK(w::dockTabRect(ui, b.docks[3]).x < w::dockTabRect(ui, b.docks[4]).x &&
             w::dockTabRect(ui, b.docks[4]).x < w::dockTabRect(ui, b.docks[0]).x);
    UI_CHECK(w::dockTabBar(ui, b.docks[1]).valid() && w::dockTabBar(ui, b.docks[1]) != s2); // assets + history kept their group
}

int demo(const char* out) {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle area = w::dockArea(ui, ui.root(), "area");
    const NodeHandle outline = w::dockWidget(ui, area, "Outline", w::DockSide::Left, "outline");
    const NodeHandle assets = w::dockWidget(ui, area, "Assets", w::DockSide::Left, "assets");
    const NodeHandle history = w::dockWidget(ui, area, "History", w::DockSide::Left, "history");
    const NodeHandle prof = w::dockWidget(ui, area, "Profiler", w::DockSide::Bottom, "profiler");
    const NodeHandle props = w::dockWidget(ui, area, "Properties", w::DockSide::Right, "props");
    w::label(ui, w::dockBody(ui, outline), "outline");
    w::label(ui, w::dockBody(ui, assets), "assets: textures, meshes, sounds");
    w::label(ui, w::dockBody(ui, history), "history");
    w::label(ui, w::dockBody(ui, props), "A floating dock: drag the title bar.");
    w::label(ui, w::dockRegion(ui, area, w::DockSide::Center), "centre");
    const NodeHandle ch = w::chart(ui, w::dockBody(ui, prof), w::ChartKind::Flame, "flame");
    ui.setCss(ch, "flex-grow", "1");
    ui.setCss(w::dockBody(ui, prof), "padding", "4px");
    setFrame(ui, ch);
    w::dockTabify(ui, assets, outline);
    w::dockTabify(ui, history, outline);
    w::dockRaise(ui, assets);
    w::dockFloat(ui, props, {520, 90, 260, 170});
    ui.update();
    ui.pointerMove(ctr(w::chartFlameSpanRect(ui, ch, 3)));
    ui.update(0.f);
    Image img;
    render(ui, img);
    return stbi_write_png(out, img.width, img.height, 4, img.pixels.data(), img.width * 4) ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    struct Gate { const char* name; void (*fn)(); };
    static const Gate kGates[] = {{"chart_flame", &gateChartFlame}, {"dock_tabs", &gateDockTabs}, {"dock_float", &gateDockFloat},
                                  {"dock_layout_tabs_float", &gateDockLayout}};
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
    return finish("fuse_ui_tier_b4");
}
