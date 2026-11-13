// fuse_ui_calendar: QCalendarWidget parity — first day of week, ISO week numbers, Home / End / Ctrl / PageUp keys,
// date range (greyed, unpickable, keys stop), page signal, clicked, a11y value text.
#include "test_util.hpp"

#include "fuse/ui/widgets/widgets.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace fuse::ui;
namespace w = fuse::ui::widgets;

int main() {
    UiContext ui({.viewport = {500, 500}, .font = &fuse::ui::test::font()});
    w::install(ui);
    const NodeHandle col = w::panel(ui, ui.root());
    const NodeHandle cal = w::calendar(ui, col, 2026, 10, 29, "cal"); // a Thursday
    ui.update();
    int y = 0, m = 0, d = 0, pages = 0, clicks = 0, lastPage = -1;
    auto date = [&] { w::calendarDate(ui, cal, y, m, d); return y * 10000 + m * 100 + d; };
    ui.connect(cal, SignalKind::TextChanged, [&](UiContext&, NodeHandle, const SignalArgs& a) { ++pages; lastPage = int(a.value); });
    ui.connect(cal, SignalKind::Clicked, [&](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });

    // ISO weeks
    UI_CHECK(w::calendarIsoWeek(2026, 10, 29) == 44);
    UI_CHECK(w::calendarIsoWeek(2021, 1, 3) == 53);  // belongs to 2020's week 53
    UI_CHECK(w::calendarIsoWeek(2024, 12, 30) == 1); // belongs to 2025's week 1

    // keys: Home / End = the week (Monday first), Ctrl = the month, Ctrl+PageUp a year
    ui.setFocus(cal);
    ui.keyDown(Key::Home);
    UI_CHECK(date() == 20261026);
    ui.keyDown(Key::End);
    UI_CHECK(date() == 20261101 && pages == 1 && lastPage == 2026 * 12 + 10);
    ui.keyDown(Key::Home, kModCtrl);
    UI_CHECK(date() == 20261101);
    ui.keyDown(Key::End, kModCtrl);
    UI_CHECK(date() == 20261130);
    ui.keyDown(Key::PageUp, kModCtrl);
    UI_CHECK(date() == 20251130);
    ui.keyDown(Key::PageDown, kModCtrl);
    // Sunday first: the week of Mon 30 Nov 2026 starts on Sun 29 Nov
    w::setCalendarOptions(ui, cal, {.firstDayOfWeek = 6});
    ui.keyDown(Key::Home);
    UI_CHECK(date() == 20261129);
    ui.keyDown(Key::End);
    UI_CHECK(date() == 20261205);

    // click mapping with a week-number column and Sunday first: Dec 2026 starts on Tue -> first cell Sun 29 Nov
    w::setCalendarOptions(ui, cal, {.firstDayOfWeek = 6, .weekNumbers = true});
    ui.update();
    const Rect c = ui.contentRect(cal);
    const f32 hh = std::ceil(fuse::ui::test::font().metrics(ui.computed(cal).fontSize).lineHeight()) + 8.f;
    const f32 cw = c.w / 8.f, ch = (c.h - hh) / 7.f;
    auto cell = [&](int row, int colm) { return Vec2{c.x + cw * (f32(colm) + 1.5f), c.y + hh + ch + ch * (f32(row) + 0.5f)}; };
    ui.pointerDown(cell(0, 0), MouseButton::Left);
    ui.pointerUp(cell(0, 0), MouseButton::Left);
    UI_CHECK(date() == 20261129 && clicks == 1);
    // clicking a leading day switches the page (as Qt): November starts on a Sunday, so row 2 = from Sun 15 Nov
    ui.pointerDown(cell(2, 3), MouseButton::Left);
    ui.pointerUp(cell(2, 3), MouseButton::Left);
    UI_CHECK(date() == 20261118);
    ui.pointerDown({c.x + cw * 0.5f, cell(2, 0).y}, MouseButton::Left); // the week-number column does nothing
    ui.pointerUp({c.x + cw * 0.5f, cell(2, 0).y}, MouseButton::Left);
    UI_CHECK(date() == 20261118);

    // range: keys clamp, out-of-range cells cannot be clicked, chevrons stop at the range's pages
    w::setCalendarRange(ui, cal, 2026, 12, 10, 2026, 12, 20);
    ui.update();
    UI_CHECK(date() == 20261210); // the selection clamps into the new range
    ui.keyDown(Key::Down);
    ui.keyDown(Key::Down);
    UI_CHECK(date() == 20261220);
    ui.keyDown(Key::PageUp);
    UI_CHECK(date() == 20261210);
    ui.pointerDown(cell(3, 4), MouseButton::Left); // Thu 24 Dec: out of range
    ui.pointerUp(cell(3, 4), MouseButton::Left);
    UI_CHECK(date() == 20261210);
    pages = 0;
    ui.pointerDown({c.x + hh * 0.5f, c.y + hh * 0.5f}, MouseButton::Left); // previous page: November is out of range
    ui.pointerUp({c.x + hh * 0.5f, c.y + hh * 0.5f}, MouseButton::Left);
    int py = 0, pm = 0;
    w::calendarShownPage(ui, cal, py, pm);
    UI_CHECK(py == 2026 && pm == 12 && pages == 0);
    w::setCalendarRange(ui, cal, 1900, 1, 1, 2100, 12, 31);
    w::setCalendarShownPage(ui, cal, 2027, 3);
    w::calendarShownPage(ui, cal, py, pm);
    UI_CHECK(py == 2027 && pm == 3 && pages == 1 && date() == 20261210); // page only, selection kept

    // a11y: the selected date written out
    ui.update();
    std::vector<A11ySnapshotNode> snap;
    ui.snapshotA11y(snap);
    bool seen = false;
    for (const A11ySnapshotNode& a : snap)
        if (a.node == cal) seen = a.valueText == "Thursday 10 December 2026";
    UI_CHECK(seen);
    return fuse::ui::test::finish("fuse_ui_calendar");
}
