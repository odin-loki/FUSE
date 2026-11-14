// fuse_ui_datetimeedit: QDateTimeEdit / QDateEdit / QTimeEdit parity — display-format sections, section navigation,
// clamped / wrapping stepping, typed digits and names, AM / PM, min / max, calendar popup, a11y roles and value text.
#include "test_util.hpp"

#include "fuse/ui/widgets/widgets.hpp"

#include <locale>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fuse::ui;
namespace w = fuse::ui::widgets;

int main() {
    UiContext ui({.viewport = {500, 500}, .font = &fuse::ui::test::font()});
    w::install(ui);
    const NodeHandle col = w::panel(ui, ui.root());
    const NodeHandle before = w::lineEdit(ui, col, "", "before");
    const NodeHandle dt = w::dateTimeEdit(ui, col, w::DateTime{2026, 1, 31, 23, 58},
                                          {.displayFormat = "dd/MM/yyyy hh:mm:ss AP"}, "dt");
    const NodeHandle after = w::lineEdit(ui, col, "", "after");
    ui.update();
    UI_CHECK(w::dateTimeEditText(ui, dt) == "31/01/2026 11:58:00 PM");
    UI_CHECK(w::dateTimeEditSectionCount(ui, dt) == 7);
    int changes = 0, finished = 0;
    std::string iso;
    ui.connect(dt, SignalKind::ValueChanged, [&](UiContext&, NodeHandle, const SignalArgs& a) { ++changes; iso = std::string(a.text); });
    ui.connect(dt, SignalKind::EditingFinished, [&](UiContext&, NodeHandle, const SignalArgs&) { ++finished; });

    // ---- Tab enters at the first section, walks the sections, then leaves ----
    ui.setFocus(before, FocusReason::Keyboard);
    ui.keyDown(Key::Tab);
    UI_CHECK(ui.focused() == dt && w::dateTimeEditField(ui, dt) == 0);
    for (int i = 0; i < 6; ++i) ui.keyDown(Key::Tab);
    UI_CHECK(ui.focused() == dt && w::dateTimeEditField(ui, dt) == 6);
    ui.keyDown(Key::Tab);
    UI_CHECK(ui.focused() == after);
    ui.keyDown(Key::Tab, kModShift);
    UI_CHECK(ui.focused() == dt && w::dateTimeEditField(ui, dt) == 6);

    // ---- stepping clamps at section ends (no carry), the month length clamps the day ----
    ui.keyDown(Key::Home);
    ui.keyDown(Key::Up); // day 31 is the max: stays
    UI_CHECK(w::dateTimeEditText(ui, dt) == "31/01/2026 11:58:00 PM" && changes == 0);
    ui.keyDown(Key::Right);
    ui.keyDown(Key::Up); // Feb: 31 -> 28
    UI_CHECK(w::dateTimeEditText(ui, dt) == "28/02/2026 11:58:00 PM" && changes == 1);
    UI_CHECK(iso == "2026-02-28T23:58:00.000");
    ui.keyDown(Key::End);
    ui.keyDown(Key::Down); // PM -> AM
    UI_CHECK(w::dateTimeEditText(ui, dt) == "28/02/2026 11:58:00 AM");
    ui.keyDown(Key::Left); ui.keyDown(Key::Left); // minute
    ui.keyDown(Key::Up); ui.keyDown(Key::Up); ui.keyDown(Key::Up);
    UI_CHECK(w::dateTimeEditSectionText(ui, dt, 4) == "59"); // clamped, no carry into the hour
    ui.keyDown(Key::PageDown);
    UI_CHECK(w::dateTimeEditSectionText(ui, dt, 4) == "49");

    // ---- wrapping wraps inside the section ----
    w::setDateTimeEditOptions(ui, dt, {.displayFormat = "dd/MM/yyyy hh:mm:ss AP", .wrapping = true});
    w::setDateTimeEditSection(ui, dt, 1);
    ui.keyDown(Key::Down); ui.keyDown(Key::Down);
    UI_CHECK(w::dateTimeEditSectionText(ui, dt, 1) == "12");

    // ---- typing: digits advance, a separator jumps on, a / p ----
    ui.keyDown(Key::Home);
    ui.textInput("5/");
    UI_CHECK(w::dateTimeEditField(ui, dt) == 1 && w::dateTimeEditSectionText(ui, dt, 0) == "05");
    ui.textInput("3"); // no month 3x: advances at once
    UI_CHECK(w::dateTimeEditField(ui, dt) == 2 && w::dateTimeEditSectionText(ui, dt, 1) == "03");
    ui.textInput("199");
    UI_CHECK(w::dateTimeEditSectionText(ui, dt, 2) == "2026"); // a partial year is not applied
    ui.textInput("9");
    UI_CHECK(w::dateTimeEditSectionText(ui, dt, 2) == "1999" && w::dateTimeEditField(ui, dt) == 3);
    ui.textInput("0730");
    UI_CHECK(w::dateTimeEditField(ui, dt) == 5);
    ui.keyDown(Key::End);
    ui.textInput("p");
    UI_CHECK(w::dateTimeEditText(ui, dt) == "05/03/1999 07:30:00 PM");
    ui.textInput("a");
    UI_CHECK(w::dateTimeEditText(ui, dt) == "05/03/1999 07:30:00 AM");
    ui.keyDown(Key::Enter);
    UI_CHECK(finished == 1);
    const w::DateTime got = w::dateTimeEditDateTime(ui, dt);
    UI_CHECK(got.year == 1999 && got.month == 3 && got.day == 5 && got.hour == 7 && got.minute == 30);
    UI_CHECK(i32(ui.value(dt)) == w::daysFromCivil(1999, 3, 5));

    // ---- month and weekday names ----
    const NodeHandle de = w::dateEdit(ui, col, 2026, 10, 31, {.displayFormat = "dddd d MMMM yyyy"}, "de");
    ui.update();
    UI_CHECK(w::dateTimeEditText(ui, de) == "Saturday 31 October 2026");
    ui.setFocus(de);
    w::setDateTimeEditSection(ui, de, 2);
    ui.textInput("ju"); // June / July: not unique yet
    UI_CHECK(w::dateTimeEditSectionText(ui, de, 2) == "June" && w::dateTimeEditField(ui, de) == 2);
    ui.textInput("l");
    UI_CHECK(w::dateTimeEditSectionText(ui, de, 2) == "July" && w::dateTimeEditField(ui, de) == 3);
    UI_CHECK(w::dateTimeEditText(ui, de) == "Friday 31 July 2026"); // June showed 30; July returns to the 31st
    ui.keyDown(Key::Home);
    ui.keyDown(Key::Up); // weekday steps the date by a day
    UI_CHECK(w::dateTimeEditText(ui, de) == "Saturday 1 August 2026");
    // the intended day survives stepping through short months, and is replaced by a day set on purpose
    w::setDateTimeEditOptions(ui, de, {.displayFormat = "yyyy-MM-dd"});
    w::setDateTimeEditDateTime(ui, de, {2024, 1, 31});
    w::setDateTimeEditSection(ui, de, 1);
    ui.keyDown(Key::Up);
    UI_CHECK(w::dateTimeEditText(ui, de) == "2024-02-29");
    ui.keyDown(Key::Up);
    UI_CHECK(w::dateTimeEditText(ui, de) == "2024-03-31");
    ui.keyDown(Key::Home);
    ui.keyDown(Key::Up); ui.keyDown(Key::Up); ui.keyDown(Key::Up); ui.keyDown(Key::Up); // via Feb 2025..2027 (28)
    UI_CHECK(w::dateTimeEditText(ui, de) == "2028-03-31");
    w::setDateTimeEditSection(ui, de, 2);
    ui.keyDown(Key::Down); // day 30 on purpose
    w::setDateTimeEditSection(ui, de, 1);
    ui.keyDown(Key::Down); ui.keyDown(Key::Up);
    UI_CHECK(w::dateTimeEditText(ui, de) == "2028-03-30");

    // ---- two-digit year window ----
    const NodeHandle yy = w::dateEdit(ui, col, 2026, 5, 1, {.displayFormat = "dd.MM.yy", .twoDigitYearStart = 1950}, "yy");
    ui.update();
    ui.setFocus(yy);
    w::setDateTimeEditSection(ui, yy, 2);
    ui.textInput("49");
    UI_CHECK(w::dateTimeEditDateTime(ui, yy).year == 2049);
    w::setDateTimeEditSection(ui, yy, 2);
    ui.textInput("50");
    UI_CHECK(w::dateTimeEditDateTime(ui, yy).year == 1950 && w::dateTimeEditText(ui, yy) == "01.05.50");
    w::setDateTimeEditOptions(ui, yy, {.displayFormat = "dd.MM.yy"}); // default: the shown century
    w::setDateTimeEditSection(ui, yy, 2);
    ui.textInput("07");
    UI_CHECK(w::dateTimeEditDateTime(ui, yy).year == 1907);

    // ---- locale names: from the C++ runtime only; an unknown locale falls back to English ----
    w::setDateTimeEditOptions(ui, yy, {.displayFormat = "MMMM", .locale = "xx_NOPE.UTF-8"});
    UI_CHECK(!w::dateTimeEditLocalized(ui, yy) && w::dateTimeEditText(ui, yy) == "May");
    w::setDateTimeEditOptions(ui, yy, {.displayFormat = "MMMM ddd", .locale = "C"});
    UI_CHECK(w::dateTimeEditLocalized(ui, yy) && w::dateTimeEditText(ui, yy) == "May Wed"); // 1907-05-01
    bool de_ok = true;
    try { (void)std::locale("de_DE.UTF-8"); } catch (const std::exception&) { de_ok = false; }
    if (de_ok) {
        w::setDateTimeEditOptions(ui, yy, {.displayFormat = "MMMM", .locale = "de_DE.UTF-8"});
        UI_CHECK(w::dateTimeEditText(ui, yy) == "Mai");
    }
    w::setDateTimeEditDateTime(ui, de, {2026, 8, 1});

    // ---- minimum / maximum (the popup greys days outside and cannot pick them) ----
    w::setDateTimeEditOptions(ui, de, {.displayFormat = "yyyy-MM-dd", .minimum = {2026, 8, 1}, .maximum = {2026, 12, 31}});
    ui.setFocus(de);
    ui.keyDown(Key::Home);
    ui.keyDown(Key::Up);
    UI_CHECK(w::dateTimeEditText(ui, de) == "2026-12-31"); // year +1 clamped to the maximum
    w::setDateTimeEditDateTime(ui, de, {2020, 1, 1});
    UI_CHECK(w::dateTimeEditText(ui, de) == "2026-08-01");
    w::setDateTimeEditOptions(ui, de, {.displayFormat = "yyyy-MM-dd", .minimum = {2026, 7, 30}, .maximum = {2026, 8, 3}});
    ui.setFocus(de);
    ui.keyDown(Key::F4);
    UI_CHECK(w::dateTimeEditPopupOpen(ui, de));
    const NodeHandle rcal = ui.focused();
    UI_CHECK(ui.rangeMin(rcal) == f32(w::daysFromCivil(2026, 7, 30)) && ui.rangeMax(rcal) == f32(w::daysFromCivil(2026, 8, 3)));
    for (int i = 0; i < 5; ++i) ui.keyDown(Key::Right); // stops at the maximum
    UI_CHECK(w::dateTimeEditText(ui, de) == "2026-08-03");
    ui.keyDown(Key::Up); // a week back: 27 Jul is out of range -> clamps to the minimum
    UI_CHECK(w::dateTimeEditText(ui, de) == "2026-07-30");
    ui.keyDown(Key::Enter);
    ui.update();
    w::setDateTimeEditOptions(ui, de, {.displayFormat = "yyyy-MM-dd"});
    w::setDateTimeEditDateTime(ui, de, {2026, 8, 1});

    // ---- calendar popup: Alt+Down opens, picking keeps the time; the button sits at the right ----
    ui.setFocus(dt);
    ui.keyDown(Key::Down, kModAlt);
    UI_CHECK(w::dateTimeEditPopupOpen(ui, dt));
    const NodeHandle cal = ui.focused();
    UI_CHECK(ui.alive(cal) && cal != dt);
    ui.keyDown(Key::Right);
    UI_CHECK(w::dateTimeEditText(ui, dt) == "06/03/1999 07:30:00 AM");
    ui.keyDown(Key::Enter);
    ui.update();
    UI_CHECK(!w::dateTimeEditPopupOpen(ui, dt));
    w::setDateTimeEditOptions(ui, dt, {.displayFormat = "dd/MM/yyyy", .calendarPopup = false});
    ui.setFocus(dt);
    ui.keyDown(Key::F4);
    UI_CHECK(!w::dateTimeEditPopupOpen(ui, dt));
    // click picks the section under the pointer
    ui.update();
    const Rect r = ui.contentRect(dt);
    ui.pointerDown({r.x + 60.f, r.y + r.h * 0.5f}, MouseButton::Left);
    ui.pointerUp({r.x + 60.f, r.y + r.h * 0.5f}, MouseButton::Left);
    UI_CHECK(w::dateTimeEditField(ui, dt) == 2);
    ui.pointerDown({r.x + 2.f, r.y + r.h * 0.5f}, MouseButton::Left);
    ui.pointerUp({r.x + 2.f, r.y + r.h * 0.5f}, MouseButton::Left);
    UI_CHECK(w::dateTimeEditField(ui, dt) == 0);

    // ---- QTimeEdit: no popup, clamped to the day ----
    const NodeHandle te = w::timeEdit(ui, col, 23, 59, 30, {.displayFormat = "HH:mm:ss"}, "te");
    ui.update();
    UI_CHECK(w::dateTimeEditText(ui, te) == "23:59:30");
    ui.setFocus(te);
    ui.keyDown(Key::Space);
    UI_CHECK(!w::dateTimeEditPopupOpen(ui, te));
    w::setDateTimeEditOptions(ui, te, {.displayFormat = "HH:mm:ss", .carry = true});
    ui.keyDown(Key::End);
    ui.keyDown(Key::PageUp); ui.keyDown(Key::PageUp); ui.keyDown(Key::PageUp); // carry stops at the day's end
    UI_CHECK(w::dateTimeEditText(ui, te) == "23:59:59");

    // ---- accessibility: roles and the shown text ----
    ui.update();
    std::vector<A11ySnapshotNode> snap;
    ui.snapshotA11y(snap);
    int seen = 0;
    for (const A11ySnapshotNode& a : snap) {
        if (a.node == dt) seen += a.role == A11yRole::DateTimeInput && a.valueText == w::dateTimeEditText(ui, dt);
        if (a.node == de) seen += a.role == A11yRole::DateInput && a.valueText == "2026-08-01";
        if (a.node == te) seen += a.role == A11yRole::TimeInput && a.valueText == "23:59:59";
    }
    UI_CHECK(seen == 3);
    return fuse::ui::test::finish("fuse_ui_datetimeedit");
}
