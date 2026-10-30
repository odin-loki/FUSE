// fuse_ui_lineedit: QLineEdit parity — echo modes (password / no echo / echo on edit), max length, placeholder,
// read-only, validators (int, double, regex), Submitted gated on Acceptable, secrets kept out of text / a11y / clipboard.
#include "test_util.hpp"

#include "fuse/ui/widgets/widgets.hpp"

#include <string>
#include <vector>

using namespace fuse::ui;
namespace w = fuse::ui::widgets;

namespace {
u32 glyphs(const UiContext& ui, NodeHandle n, f32* alpha = nullptr) {
    u32 c = 0;
    for (const DrawCmd& d : ui.drawList().cmds())
        if (d.op == DrawOp::Glyph && d.node == n.index) { ++c; if (alpha) *alpha = d.fill.a; }
    return c;
}
void type(UiContext& ui, std::string_view s) { ui.textInput(s); }
} // namespace

int main() {
    UiContext ui({.viewport = {400, 300}, .font = &fuse::ui::test::font()});
    w::install(ui);
    const NodeHandle col = w::panel(ui, ui.root());
    const NodeHandle pw = w::lineEdit(ui, col, "", "pw");
    const NodeHandle num = w::lineEdit(ui, col, "", "num");
    const NodeHandle dbl = w::lineEdit(ui, col, "", "dbl");
    const NodeHandle mail = w::lineEdit(ui, col, "", "mail");
    const NodeHandle ro = w::lineEdit(ui, col, "fixed", "ro");
    const NodeHandle plain = w::lineEdit(ui, col, "", "plain");
    ui.update();

    // ---- password: secret only in lineEditText / signals; masked everywhere else ----
    std::string lastSignal;
    ui.connect(pw, SignalKind::TextChanged, [&](UiContext&, NodeHandle, const SignalArgs& a) { lastSignal.assign(a.text); });
    w::setLineEditOptions(ui, pw, {.echo = w::EchoMode::Password, .maxLength = 8});
    ui.setFocus(pw);
    type(ui, "hunter2");
    UI_CHECK(w::lineEditText(ui, pw) == "hunter2");
    UI_CHECK(lastSignal == "hunter2");
    UI_CHECK(ui.text(pw) == "\u2022\u2022\u2022\u2022\u2022\u2022\u2022");
    std::vector<A11ySnapshotNode> snap;
    ui.update();
    ui.snapshotA11y(snap);
    bool leak = false, seen = false;
    for (const auto& s : snap) {
        if (s.name.find("hunter") != std::string::npos) leak = true;
        if (s.node == pw) seen = s.role == A11yRole::TextField;
    }
    UI_CHECK(!leak && seen);
    UI_CHECK(glyphs(ui, pw) == 7);
    // copy / cut refused while masked; select-all + paste replaces
    ui.setClipboardText("clip");
    ui.keyDown(Key::A, kModCtrl);
    ui.keyDown(Key::C, kModCtrl);
    std::string clip;
    ui.clipboardText(clip);
    UI_CHECK(clip == "clip");
    ui.keyDown(Key::X, kModCtrl);
    UI_CHECK(w::lineEditText(ui, pw) == "hunter2");
    // max length 8: paste of "clip" over nothing selected adds one code point only
    ui.keyDown(Key::End);
    ui.keyDown(Key::V, kModCtrl);
    UI_CHECK(w::lineEditText(ui, pw) == "hunter2c");
    type(ui, "z");
    UI_CHECK(w::lineEditText(ui, pw) == "hunter2c");
    // multi-byte code points count once
    w::setLineEditText(ui, pw, "");
    type(ui, "\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9");
    UI_CHECK(w::lineEditText(ui, pw).size() == 16);
    // Ctrl+Left in a masked field goes home (no word boundaries revealed)
    w::setLineEditText(ui, pw, "ab cd");
    ui.keyDown(Key::Left, kModCtrl);
    type(ui, "X");
    UI_CHECK(w::lineEditText(ui, pw) == "Xab cd");

    // ---- echo on edit: plain while focused, masked otherwise; no echo draws nothing ----
    w::setLineEditOptions(ui, pw, {.echo = w::EchoMode::PasswordEchoOnEdit});
    ui.update();
    UI_CHECK(glyphs(ui, pw) == 5); // "Xab cd": the space draws no glyph
    UI_CHECK(ui.text(pw) == "\u2022\u2022\u2022\u2022\u2022\u2022");
    w::setLineEditOptions(ui, pw, {.echo = w::EchoMode::NoEcho});
    ui.update();
    UI_CHECK(glyphs(ui, pw) == 0 && ui.text(pw).empty());

    // ---- int validator: invalid keystrokes rejected, Submitted only when acceptable ----
    int submitted = 0;
    ui.connect(num, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs&) { ++submitted; });
    w::setLineEditOptions(ui, num, {.validator = w::intValidator(10, 500)});
    ui.setFocus(num);
    type(ui, "a");
    UI_CHECK(w::lineEditText(ui, num).empty());
    type(ui, "4");
    UI_CHECK(w::lineEditState(ui, num) == w::Validation::Intermediate);
    ui.keyDown(Key::Enter);
    UI_CHECK(submitted == 0);
    type(ui, "2");
    UI_CHECK(w::lineEditState(ui, num) == w::Validation::Acceptable);
    type(ui, "0"); // 420
    type(ui, "0"); // 4200 > 500: rejected
    UI_CHECK(w::lineEditText(ui, num) == "420");
    type(ui, "-"); // "420-": not a number
    UI_CHECK(w::lineEditText(ui, num) == "420");
    ui.keyDown(Key::Enter);
    UI_CHECK(submitted == 1);
    // undo of a valid state still works; undo into an invalid one is not possible by construction
    ui.keyDown(Key::Z, kModCtrl);
    UI_CHECK(w::lineEditText(ui, num) == "42");
    UI_CHECK(w::intValidator(-50, 50)("-") == w::Validation::Intermediate);
    UI_CHECK(w::intValidator(0, 50)("-") == w::Validation::Invalid);
    UI_CHECK(w::intValidator(-50, 50)("-51") == w::Validation::Invalid);
    UI_CHECK(w::intValidator(100, 200)("1") == w::Validation::Intermediate);

    // ---- double validator ----
    const w::Validator dv = w::doubleValidator(-1.0, 100.0, 2);
    UI_CHECK(dv("") == w::Validation::Intermediate);
    UI_CHECK(dv("-") == w::Validation::Intermediate);
    UI_CHECK(dv("3.") == w::Validation::Acceptable);
    UI_CHECK(dv("3.14") == w::Validation::Acceptable);
    UI_CHECK(dv("3.141") == w::Validation::Invalid);
    UI_CHECK(dv("1e") == w::Validation::Intermediate);
    UI_CHECK(dv("1e1") == w::Validation::Acceptable);
    UI_CHECK(dv("1x") == w::Validation::Invalid);
    UI_CHECK(dv("101") == w::Validation::Invalid);
    UI_CHECK(dv("-2") == w::Validation::Invalid); // no digits can bring it back above -1
    UI_CHECK(dv("-0.") == w::Validation::Acceptable);
    w::setLineEditOptions(ui, dbl, {.validator = dv});
    ui.setFocus(dbl);
    type(ui, "12.345");
    UI_CHECK(w::lineEditText(ui, dbl).empty()); // one Text event = one edit: rejected whole
    for (char ch : std::string("12.345")) type(ui, std::string(1, ch));
    UI_CHECK(w::lineEditText(ui, dbl) == "12.34");

    // ---- regex validator gates Submitted only ----
    int mailOk = 0;
    ui.connect(mail, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs&) { ++mailOk; });
    w::setLineEditOptions(ui, mail, {.placeholder = "you@example.com", .validator = w::regexValidator(R"([^@\s]+@[^@\s]+\.\w+)")});
    ui.update();
    f32 phA = 0.f;
    UI_CHECK(glyphs(ui, mail, &phA) == 15); // placeholder drawn, dimmed
    UI_CHECK(phA > 0.f && phA < 0.75f);
    UI_CHECK(ui.text(mail).empty());
    ui.setFocus(mail);
    type(ui, "a@b");
    ui.keyDown(Key::Enter);
    UI_CHECK(mailOk == 0);
    type(ui, ".io");
    ui.keyDown(Key::Enter);
    UI_CHECK(mailOk == 1);
    ui.update();
    UI_CHECK(glyphs(ui, mail) == 6);

    // ---- read-only: no edits, copy works ----
    w::setLineEditOptions(ui, ro, {.readOnly = true});
    ui.setFocus(ro);
    type(ui, "x");
    ui.keyDown(Key::Backspace);
    ui.keyDown(Key::A, kModCtrl);
    ui.keyDown(Key::X, kModCtrl);
    UI_CHECK(w::lineEditText(ui, ro) == "fixed");
    ui.clipboardText(clip);
    UI_CHECK(clip == "fixed");
    ui.keyDown(Key::V, kModCtrl);
    UI_CHECK(w::lineEditText(ui, ro) == "fixed");

    // ---- defaults unchanged: a plain field behaves as before ----
    ui.setFocus(plain);
    type(ui, "hello world");
    UI_CHECK(ui.text(plain) == "hello world");
    ui.keyDown(Key::Left, kModCtrl);
    type(ui, "big ");
    UI_CHECK(ui.text(plain) == "hello big world");
    ui.keyDown(Key::Enter);

    // ---- a reused node slot gets default options ----
    ui.destroy(pw);
    const NodeHandle fresh = w::lineEdit(ui, col, "", "fresh");
    ui.setFocus(fresh);
    type(ui, "visible");
    UI_CHECK(ui.text(fresh) == "visible");
    return fuse::ui::test::finish("fuse_ui_lineedit");
}
