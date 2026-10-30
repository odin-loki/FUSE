// fuse_ui_spinbox: QSpinBox / QDoubleSpinBox parity — typed entry with commit / revert, prefix / suffix, decimals,
// wrapping, specialValueText, adaptive decimal steps, keyboardTracking, custom text <-> value, editingFinished.
#include "test_util.hpp"

#include "fuse/ui/widgets/widgets.hpp"

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

using namespace fuse::ui;
namespace w = fuse::ui::widgets;

int main() {
    UiContext ui({.viewport = {400, 400}, .font = &fuse::ui::test::font()});
    w::install(ui);
    const NodeHandle col = w::panel(ui, ui.root());
    const NodeHandle sp = w::spinBox(ui, col, 0.f, 100.f, 5.f, 1.f, "qty");
    const NodeHandle other = w::lineEdit(ui, col, "", "other");
    ui.update();
    UI_CHECK(w::spinBoxText(ui, sp) == "5");

    int changes = 0, finished = 0;
    f32 last = -1.f;
    ui.connect(sp, SignalKind::ValueChanged, [&](UiContext&, NodeHandle, const SignalArgs& a) { ++changes; last = a.value; });
    ui.connect(sp, SignalKind::EditingFinished, [&](UiContext&, NodeHandle, const SignalArgs&) { ++finished; });

    // ---- prefix / suffix ----
    w::setSpinBoxOptions(ui, sp, {.prefix = "$ ", .suffix = " ea"});
    ui.update();
    UI_CHECK(w::spinBoxText(ui, sp) == "$ 5 ea" && w::spinBoxCleanText(ui, sp) == "5");

    // ---- typed entry: replaces, tracks per keystroke, Enter commits + editingFinished ----
    ui.setFocus(sp);
    UI_CHECK(ui.wantsTextInput());
    ui.textInput("4");
    UI_CHECK(w::spinBoxText(ui, sp) == "$ 4 ea" && ui.value(sp) == 4.f && changes == 1);
    ui.textInput("2");
    UI_CHECK(ui.value(sp) == 42.f && w::spinBoxCleanText(ui, sp) == "42");
    ui.textInput("x"); // not numeric: dropped
    UI_CHECK(w::spinBoxCleanText(ui, sp) == "42");
    ui.textInput("0"); // 420 is out of range: shown, value kept
    UI_CHECK(w::spinBoxCleanText(ui, sp) == "420" && ui.value(sp) == 42.f);
    ui.keyDown(Key::Enter); // commit clamps
    UI_CHECK(ui.value(sp) == 100.f && w::spinBoxText(ui, sp) == "$ 100 ea" && finished == 1);
    // Backspace / Escape
    ui.textInput("12");
    ui.keyDown(Key::Backspace);
    UI_CHECK(w::spinBoxCleanText(ui, sp) == "1" && ui.value(sp) == 1.f);
    ui.keyDown(Key::Escape);
    UI_CHECK(w::spinBoxText(ui, sp) == "$ 1 ea");
    // garbage reverts on focus out (editingFinished too)
    ui.textInput("-");
    ui.setFocus(other);
    UI_CHECK(ui.value(sp) == 1.f && w::spinBoxText(ui, sp) == "$ 1 ea" && finished == 2);

    // ---- keyboardTracking off: one valueChanged on commit ----
    w::setSpinBoxOptions(ui, sp, {.keyboardTracking = false});
    ui.setFocus(sp);
    changes = 0;
    ui.textInput("3");
    ui.textInput("7");
    UI_CHECK(changes == 0 && ui.value(sp) == 1.f);
    ui.keyDown(Key::Enter);
    UI_CHECK(changes == 1 && last == 37.f);
    // arrows commit the typed text first, then step
    ui.textInput("50");
    ui.keyDown(Key::Up);
    UI_CHECK(ui.value(sp) == 51.f);
    ui.keyDown(Key::PageDown);
    UI_CHECK(ui.value(sp) == 41.f);

    // ---- wrapping (Qt: past an end jumps to the other end) ----
    w::setSpinBoxOptions(ui, sp, {.wrapping = true});
    ui.setValue(sp, 99.f);
    ui.keyDown(Key::Up);
    UI_CHECK(ui.value(sp) == 100.f);
    ui.keyDown(Key::Up);
    UI_CHECK(ui.value(sp) == 0.f);
    ui.keyDown(Key::Down);
    UI_CHECK(ui.value(sp) == 100.f);
    ui.setValue(sp, 95.f);
    ui.keyDown(Key::PageUp); // 105 -> max first
    UI_CHECK(ui.value(sp) == 100.f);
    w::spinBoxStepBy(ui, sp, 1);
    UI_CHECK(ui.value(sp) == 0.f);

    // ---- specialValueText ----
    w::setSpinBoxOptions(ui, sp, {.suffix = " min", .specialValueText = "Auto"});
    ui.setValue(sp, 0.f);
    UI_CHECK(w::spinBoxText(ui, sp) == "Auto");
    ui.keyDown(Key::Up);
    UI_CHECK(w::spinBoxText(ui, sp) == "1 min");
    ui.textInput("Auto");
    ui.keyDown(Key::Enter);
    UI_CHECK(ui.value(sp) == 0.f && w::spinBoxText(ui, sp) == "Auto");

    // ---- double spin box: decimals, ',' accepted, adaptive steps ----
    const NodeHandle dsp = w::spinBox(ui, col, -1000.f, 1000.f, 0.5f, 0.01f, "price");
    ui.update();
    UI_CHECK(w::spinBoxText(ui, dsp) == "0.50");
    w::setSpinBoxOptions(ui, dsp, {.decimals = 3});
    UI_CHECK(w::spinBoxText(ui, dsp) == "0.500");
    ui.setFocus(dsp);
    ui.textInput("2,25");
    ui.keyDown(Key::Enter);
    UI_CHECK(std::abs(ui.value(dsp) - 2.25f) < 1e-4f);
    ui.setValue(dsp, -0.f);
    UI_CHECK(w::spinBoxText(ui, dsp) == "0.000"); // no "-0"
    const NodeHandle isp = w::spinBox(ui, col, -100000.f, 100000.f, 50.f, 1.f, "big");
    ui.update();
    w::setSpinBoxOptions(ui, isp, {.adaptiveStep = true});
    ui.setFocus(isp);
    ui.keyDown(Key::Up);
    UI_CHECK(ui.value(isp) == 51.f); // < 100: 1
    ui.setValue(isp, 100.f);
    ui.keyDown(Key::Up);
    UI_CHECK(ui.value(isp) == 110.f); // 10^(2-1)
    ui.setValue(isp, 1000.f);
    ui.keyDown(Key::Down);
    UI_CHECK(ui.value(isp) == 990.f); // 999 -> log 2 -> 10 (Qt sign compensation)
    ui.setValue(isp, 2500.f);
    ui.keyDown(Key::Up);
    UI_CHECK(ui.value(isp) == 2600.f);

    // ---- textFromValue / valueFromText (hex) ----
    const NodeHandle hsp = w::spinBox(ui, col, 0.f, 255.f, 255.f, 1.f, "hex");
    ui.update();
    w::setSpinBoxOptions(ui, hsp, {.prefix = "0x",
                                   .textFromValue = [](double v) { char b[16]; std::snprintf(b, sizeof b, "%02X", unsigned(v)); return std::string(b); },
                                   .valueFromText = [](std::string_view t) -> std::optional<double> {
                                       if (t.empty() || t.size() > 2) return std::nullopt;
                                       unsigned v = 0;
                                       for (char c : t) {
                                           const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                                           if (d < 0) return std::nullopt;
                                           v = v * 16 + unsigned(d);
                                       }
                                       return double(v);
                                   }});
    UI_CHECK(w::spinBoxText(ui, hsp) == "0xFF");
    ui.setFocus(hsp);
    ui.textInput("1a");
    ui.keyDown(Key::Enter);
    UI_CHECK(ui.value(hsp) == 26.f && w::spinBoxText(ui, hsp) == "0x1A");

    // ---- pointer: arrow halves still step; wheel ----
    ui.setValue(sp, 10.f);
    const Rect r = ui.rect(sp);
    ui.pointerDown({r.right() - 6.f, r.y + 3.f}, MouseButton::Left);
    ui.pointerUp({r.right() - 6.f, r.y + 3.f}, MouseButton::Left);
    UI_CHECK(ui.value(sp) == 11.f);
    return fuse::ui::test::finish("fuse_ui_spinbox");
}
