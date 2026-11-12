// Gate fuse_ui_gestures: click counting and touch gesture recognisers (plan §4.7 "Touch and pen").
// Mouse: Event::clicks, the DoubleClick event / DoubleClicked signal (time and distance limits, a third click starts
// over), double click activating a list row. Touch: tap, double tap, long press (timer in the context's time domain,
// cancelled by leaving the slop, consumed = no Click), multi-touch ids: the primary finger behaves as the id-less API
// (tap, pan), two fingers pinch (scale, centre) and rotate, with Begin / Update / End phases.
#include "test_util.hpp"

#include <cmath>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
struct Seen {
    EventType type;
    GesturePhase phase;
    f32 scale, rotation;
    Vec2 pos;
    u8 clicks;
};
std::vector<Seen> g_log;
int g_clicked = 0, g_double = 0, g_submitted = 0;
f32 g_submittedRow = -1.f;

Vec2 centre(const Rect& r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
bool near(f32 a, f32 b, f32 eps = 0.01f) { return std::abs(a - b) <= eps; }
void logSlot(UiContext&, NodeHandle, const SignalArgs& a) {
    if (!a.event) return;
    const Event& e = *a.event;
    switch (e.type) {
    case EventType::PointerDown: case EventType::Click: case EventType::DoubleClick: case EventType::GestureTap:
    case EventType::GestureDoubleTap: case EventType::GestureLongPress: case EventType::GesturePinch: case EventType::GestureRotate:
        g_log.push_back({e.type, e.gesturePhase, e.scale, e.rotation, e.pos, e.clicks});
        break;
    default: break;
    }
}
int count(EventType t) {
    int c = 0;
    for (const Seen& s : g_log) c += s.type == t;
    return c;
}
const Seen* last(EventType t) {
    for (std::size_t i = g_log.size(); i-- > 0;)
        if (g_log[i].type == t) return &g_log[i];
    return nullptr;
}
/// A class that consumes long presses (a context-menu style widget).
bool holdEvent(EventContext&, const Event& e) { return e.type == EventType::GestureLongPress; }
} // namespace

int main() {
    UiContext ui({.viewport = {500, 700}, .font = &font()});
    w::install(ui);
    WidgetClass hold;
    hold.name = "holdpad";
    hold.onEvent = &holdEvent;
    ui.registry().add(hold);
    UI_CHECK(ui.addStyleSheet(".pad { height: 160px; background: #202428; flex-shrink: 0; }").has_value());
    ui.setCss(ui.root(), "padding", "10px");
    ui.setCss(ui.root(), "gap", "10px");
    const NodeHandle btn = w::button(ui, ui.root(), "Button", "btn");
    const NodeHandle pad = w::panel(ui, ui.root(), "pad");
    ui.addClass(pad, "pad");
    const NodeHandle holdPad = *ui.create("holdpad", ui.root(), "hold");
    ui.addClass(holdPad, "pad");
    ui.setCss(holdPad, "height", "50px");
    const NodeHandle list = w::listView(ui, ui.root(), "alpha\nbeta\ngamma\ndelta", -1, "list");
    const NodeHandle page = w::scrollArea(ui, ui.root(), "page");
    ui.setCss(page, "height", "120px");
    for (int i = 0; i < 8; ++i) ui.setCss(w::label(ui, page, "row"), "height", "40px");
    ui.connect(btn, SignalKind::Clicked, [](UiContext&, NodeHandle, const SignalArgs&) { ++g_clicked; });
    ui.connect(btn, SignalKind::DoubleClicked, [](UiContext&, NodeHandle, const SignalArgs&) { ++g_double; });
    ui.connect(btn, SignalKind::Event, &logSlot);
    ui.connect(pad, SignalKind::Event, &logSlot);
    ui.connect(pad, SignalKind::Clicked, [](UiContext&, NodeHandle, const SignalArgs&) { ++g_clicked; });
    ui.connect(holdPad, SignalKind::Clicked, [](UiContext&, NodeHandle, const SignalArgs&) { ++g_clicked; });
    ui.connect(list, SignalKind::Submitted, [](UiContext&, NodeHandle, const SignalArgs& a) { ++g_submitted; g_submittedRow = a.value; });
    ui.update(0.f);
    UiTestDriver drv(ui);
    const Vec2 pb = centre(ui.rect(btn)), pp = centre(ui.rect(pad));

    // ---- mouse double click ----
    drv.click("btn");
    UI_CHECK_EQ(g_clicked, 1);
    UI_CHECK_EQ(g_double, 0);
    UI_CHECK(last(EventType::PointerDown)->clicks == 1 && last(EventType::Click)->clicks == 1);
    drv.click("btn"); // 1/60 s later: a double click (Click first, then DoubleClick)
    UI_CHECK_EQ(g_clicked, 2);
    UI_CHECK_EQ(g_double, 1);
    UI_CHECK(last(EventType::PointerDown)->clicks == 2 && last(EventType::Click)->clicks == 2);
    UI_CHECK(g_log.back().type == EventType::DoubleClick && g_log[g_log.size() - 2].type == EventType::Click);
    drv.click("btn"); // a third quick click starts a new sequence
    UI_CHECK_EQ(g_double, 1);
    UI_CHECK(last(EventType::Click)->clicks == 1);
    drv.click("btn");
    UI_CHECK_EQ(g_double, 2);
    // too slow
    ui.update(1.f);
    drv.click("btn");
    ui.update(0.5f);
    drv.click("btn");
    UI_CHECK_EQ(g_double, 2);
    // too far apart
    ui.update(1.f);
    drv.clickAt({pb.x - 10.f, pb.y});
    drv.clickAt({pb.x + 10.f, pb.y});
    UI_CHECK_EQ(g_double, 2);
    UI_CHECK_EQ(g_clicked, 8);
    // a faster configured limit
    GestureConfig cfg = ui.gestureConfig();
    cfg.doubleClickTime = 0.01f;
    ui.setGestureConfig(cfg);
    ui.update(1.f);
    drv.click("btn");
    drv.click("btn");
    UI_CHECK_EQ(g_double, 2);
    ui.setGestureConfig(GestureConfig{});

    // ---- double click activates a list row (what the file dialog's list relies on) ----
    ui.update(1.f);
    const Vec2 row1 = centre(w::listRowRect(ui, list, 1)), row2 = centre(w::listRowRect(ui, list, 2));
    drv.clickAt(row1);
    UI_CHECK_EQ(ui.value(list), 1.f);
    UI_CHECK_EQ(g_submitted, 0);
    drv.clickAt(row1);
    UI_CHECK_EQ(g_submitted, 1);
    UI_CHECK_EQ(g_submittedRow, 1.f);
    ui.update(1.f);
    drv.clickAt(row1); // two different rows in quick succession are two selections, not an activation
    drv.clickAt(row2);
    UI_CHECK_EQ(g_submitted, 1);
    UI_CHECK_EQ(ui.value(list), 2.f);

    // ---- ... and a model view row (table / model list): cells that are not editable activate ----
    {
        static w::StringTableModel model("Name\tSize");
        for (const char* r : {"one\t1", "two\t2", "three\t3"}) model.addRow(r);
        const NodeHandle table = w::tableView(ui, ui.root(), &model.model(), "table");
        ui.setCss(table, "height", "120px");
        static int tableSubmitted = 0;
        static f32 tableRow = -1.f;
        ui.connect(table, SignalKind::Submitted, [](UiContext&, NodeHandle, const SignalArgs& a) { ++tableSubmitted; tableRow = a.value; });
        ui.update(1.f);
        const Rect r1 = w::tableRowRect(ui, table, 1), r2 = w::tableRowRect(ui, table, 2), tc = ui.contentRect(table);
        UI_CHECK(r1.y > tc.y && r2.y == r1.bottom()); // below the header row
        UI_CHECK_EQ(w::tableRowAt(ui, table, centre(r1)), 1);
        UI_CHECK_EQ(w::tableRowAt(ui, table, {tc.x + 20.f, tc.y + 4.f}), -1); // the header
        drv.clickAt(centre(r1));
        UI_CHECK_EQ(ui.value(table), 1.f);
        UI_CHECK_EQ(tableSubmitted, 0);
        drv.clickAt(centre(r1));
        UI_CHECK_EQ(tableSubmitted, 1);
        UI_CHECK_EQ(tableRow, 1.f);
        UI_CHECK(!w::viewEditor(ui, table).valid());
        ui.update(1.f);
        drv.clickAt(centre(r1)); // two rows in quick succession: two selections
        drv.clickAt(centre(r2));
        UI_CHECK_EQ(tableSubmitted, 1);
        ui.update(1.f);
        const Vec2 head{tc.x + 20.f, tc.y + 4.f}; // a double click on the header sorts twice, it activates nothing
        drv.clickAt(head);
        drv.clickAt(head);
        UI_CHECK_EQ(tableSubmitted, 1);
        ui.update(1.f);
    }

    // ---- tap / double tap ----
    g_log.clear();
    g_clicked = 0;
    ui.update(1.f);
    double t = 10.0;
    ui.touchDown(pp, t);
    ui.touchUp(pp, t += 0.05);
    UI_CHECK_EQ(count(EventType::GestureTap), 1);
    UI_CHECK_EQ(count(EventType::GestureDoubleTap), 0);
    UI_CHECK_EQ(g_clicked, 1); // a tap still clicks
    UI_CHECK(last(EventType::GestureTap)->pos == pp);
    ui.update(0.1f);
    ui.touchDown({pp.x + 6.f, pp.y}, t += 0.1);
    ui.touchUp({pp.x + 6.f, pp.y}, t += 0.05);
    UI_CHECK_EQ(count(EventType::GestureTap), 2);
    UI_CHECK_EQ(count(EventType::GestureDoubleTap), 1);
    UI_CHECK(g_log.back().type == EventType::GestureDoubleTap);
    ui.update(0.1f);
    ui.touchDown(pp, t += 0.1); // the third tap is a first tap again
    ui.touchUp(pp, t += 0.05);
    UI_CHECK_EQ(count(EventType::GestureDoubleTap), 1);
    ui.update(1.f);
    ui.touchDown(pp, t += 1.0); // too late for a double tap
    ui.touchUp(pp, t += 0.05);
    UI_CHECK_EQ(count(EventType::GestureDoubleTap), 1);
    UI_CHECK_EQ(count(EventType::GestureTap), 4);

    // ---- long press ----
    g_log.clear();
    g_clicked = 0;
    ui.update(1.f);
    UI_CHECK(!ui.needsUpdate());
    ui.touchDown(pp, t += 2.0);
    ui.update(0.f);
    UI_CHECK(ui.needsUpdate()); // the timer runs
    ui.update(0.3f);
    UI_CHECK_EQ(count(EventType::GestureLongPress), 0);
    UI_CHECK(near(ui.nextWakeSeconds(), 0.2f, 0.02f));
    ui.update(0.21f);
    UI_CHECK_EQ(count(EventType::GestureLongPress), 1);
    UI_CHECK(last(EventType::GestureLongPress)->pos == pp);
    ui.update(1.f);
    UI_CHECK_EQ(count(EventType::GestureLongPress), 1); // once
    ui.touchUp(pp, t += 1.6);
    UI_CHECK_EQ(count(EventType::GestureTap), 0); // a long press is not a tap
    UI_CHECK_EQ(g_clicked, 1);                    // nobody consumed it: the release still clicks
    ui.update(0.f);
    UI_CHECK(!ui.needsUpdate());
    UI_CHECK(std::isinf(ui.nextWakeSeconds()));
    // leaving the slop cancels it
    ui.touchDown(pp, t += 1.0);
    ui.update(0.2f);
    ui.touchMove({pp.x + 30.f, pp.y}, t += 0.2);
    ui.update(0.6f);
    UI_CHECK_EQ(count(EventType::GestureLongPress), 1);
    UI_CHECK(!ui.needsUpdate());
    ui.touchUp({pp.x + 30.f, pp.y}, t += 0.6);
    UI_CHECK_EQ(count(EventType::GestureTap), 0);
    // lifting before the time cancels it
    ui.touchDown(pp, t += 1.0);
    ui.update(0.2f);
    ui.touchUp(pp, t += 0.2);
    ui.update(0.6f);
    UI_CHECK_EQ(count(EventType::GestureLongPress), 1);
    // a widget that consumes the long press: the press ends, the release does not click
    g_clicked = 0;
    const Vec2 ph = centre(ui.rect(holdPad));
    ui.touchDown(ph, t += 1.0);
    UI_CHECK(ui.pressed() == holdPad);
    ui.update(0.6f);
    UI_CHECK(!ui.pressed().valid());
    ui.touchUp(ph, t += 0.6);
    UI_CHECK_EQ(g_clicked, 0);
    ui.touchDown(ph, t += 1.0); // a short tap on it still clicks
    ui.touchUp(ph, t += 0.05);
    UI_CHECK_EQ(g_clicked, 1);

    // ---- multi-touch: the primary finger alone is the id-less path ----
    g_log.clear();
    g_clicked = 0;
    ui.update(1.f);
    ui.touchDown(7u, pp, t += 1.0);
    UI_CHECK_EQ(ui.touchCount(), 1u);
    ui.touchUp(7u, pp, t += 0.05);
    UI_CHECK_EQ(ui.touchCount(), 0u);
    UI_CHECK_EQ(g_clicked, 1);
    UI_CHECK_EQ(count(EventType::GestureTap), 1);
    const Vec2 pg = centre(ui.rect(page));
    ui.touchDown(3u, pg, t += 1.0);
    for (int i = 1; i <= 5; ++i) ui.touchMove(3u, {pg.x, pg.y - 10.f * f32(i)}, t += 0.016);
    UI_CHECK(ui.touchPanning());
    ui.update(0.016f);
    UI_CHECK_EQ(ui.scroll(page).y, 50.f);
    ui.touchUp(3u, {pg.x, pg.y - 50.f}, t += 0.5);
    UI_CHECK(!ui.flinging());

    // ---- pinch ----
    g_log.clear();
    g_clicked = 0;
    ui.update(1.f);
    const Vec2 a{pp.x - 50.f, pp.y}, b{pp.x + 50.f, pp.y};
    ui.touchDown(1u, a, t += 1.0);
    UI_CHECK(ui.pressed() == pad);
    ui.touchDown(2u, b, t += 0.02);
    UI_CHECK(ui.gestureActive() && ui.touchCount() == 2u);
    UI_CHECK(!ui.pressed().valid()); // the second finger took the press away
    ui.touchMove(2u, {b.x + 2.f, b.y}, t += 0.016);
    UI_CHECK_EQ(count(EventType::GesturePinch), 0); // inside the pinch slop
    ui.touchMove(2u, {b.x + 50.f, b.y}, t += 0.016);
    UI_CHECK_EQ(count(EventType::GesturePinch), 1);
    UI_CHECK(last(EventType::GesturePinch)->phase == GesturePhase::Begin && near(last(EventType::GesturePinch)->scale, 1.5f));
    UI_CHECK(near(last(EventType::GesturePinch)->pos.x, pp.x + 25.f) && near(last(EventType::GesturePinch)->pos.y, pp.y));
    ui.touchMove(1u, {a.x - 50.f, a.y}, t += 0.016);
    UI_CHECK(last(EventType::GesturePinch)->phase == GesturePhase::Update && near(last(EventType::GesturePinch)->scale, 2.f));
    UI_CHECK(near(last(EventType::GesturePinch)->pos.x, pp.x));
    UI_CHECK_EQ(count(EventType::GestureRotate), 0); // the pair did not turn
    ui.touchUp(2u, {b.x + 50.f, b.y}, t += 0.016);
    UI_CHECK(last(EventType::GesturePinch)->phase == GesturePhase::End && near(last(EventType::GesturePinch)->scale, 2.f));
    UI_CHECK(!ui.gestureActive() && ui.touchCount() == 1u);
    const int pinches = count(EventType::GesturePinch);
    ui.touchMove(1u, {a.x, a.y + 40.f}, t += 0.016); // the remaining finger does nothing
    ui.touchUp(1u, {a.x, a.y + 40.f}, t += 0.016);
    UI_CHECK_EQ(count(EventType::GesturePinch), pinches);
    UI_CHECK_EQ(g_clicked, 0);
    UI_CHECK_EQ(count(EventType::GestureTap), 0);
    UI_CHECK_EQ(ui.touchCount(), 0u);

    // ---- rotate ----
    g_log.clear();
    ui.update(1.f);
    ui.touchDown(1u, a, t += 1.0);
    ui.touchDown(2u, b, t += 0.02);
    // finger 2 walks a quarter circle around finger 1's partner position: the pair turns, its length stays
    const f32 r = 100.f;
    for (int i = 1; i <= 9; ++i) {
        const f32 ang = f32(i) * 10.f * 3.14159265f / 180.f;
        ui.touchMove(2u, {a.x + r * std::cos(ang), a.y + r * std::sin(ang)}, t += 0.016);
    }
    UI_CHECK(count(EventType::GestureRotate) >= 8);
    UI_CHECK_EQ(count(EventType::GesturePinch), 0);
    UI_CHECK(near(last(EventType::GestureRotate)->rotation, 3.14159265f * 0.5f, 0.01f));
    UI_CHECK(last(EventType::GestureRotate)->phase == GesturePhase::Update);
    bool begun = false;
    for (const Seen& s : g_log)
        if (s.type == EventType::GestureRotate) { begun = s.phase == GesturePhase::Begin; break; }
    UI_CHECK(begun);
    ui.touchUp(1u, a, t += 0.016);
    UI_CHECK(last(EventType::GestureRotate)->phase == GesturePhase::End);
    ui.touchUp(2u, {a.x, a.y + r}, t += 0.016);
    UI_CHECK_EQ(ui.touchCount(), 0u);
    // touchCancel ends a running gesture
    ui.touchDown(1u, a, t += 1.0);
    ui.touchDown(2u, b, t += 0.02);
    ui.touchMove(2u, {b.x + 60.f, b.y}, t += 0.016);
    UI_CHECK(last(EventType::GesturePinch)->phase == GesturePhase::Begin);
    ui.touchCancel();
    UI_CHECK(last(EventType::GesturePinch)->phase == GesturePhase::End);
    UI_CHECK(!ui.gestureActive() && ui.touchCount() == 0u);

    ui.update(0.016f);
    ui.update(0.f);
    UI_CHECK(!ui.needsUpdate());
    return finish("fuse_ui_gestures");
}
