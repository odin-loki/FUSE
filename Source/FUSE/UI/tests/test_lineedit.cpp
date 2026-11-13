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

    // ---- regex validator: partial matches Intermediate, dead prefixes Invalid ----
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

    // ---- partial regex matcher ----
    using V = w::Validation;
    auto rx = [](std::string_view p, std::string_view t) { return w::regexPartialMatch(p, t); };
    UI_CHECK(rx("[A-Z]{3}-\\d{2,4}", "") == V::Intermediate);
    UI_CHECK(rx("[A-Z]{3}-\\d{2,4}", "AB") == V::Intermediate);
    UI_CHECK(rx("[A-Z]{3}-\\d{2,4}", "ABC-1") == V::Intermediate);
    UI_CHECK(rx("[A-Z]{3}-\\d{2,4}", "ABC-12") == V::Acceptable);
    UI_CHECK(rx("[A-Z]{3}-\\d{2,4}", "ABC-1234") == V::Acceptable);
    UI_CHECK(rx("[A-Z]{3}-\\d{2,4}", "ABC-12345") == V::Invalid);
    UI_CHECK(rx("[A-Z]{3}-\\d{2,4}", "AB1") == V::Invalid);
    UI_CHECK(rx("^(?:cat|car)s?$", "ca") == V::Intermediate);
    UI_CHECK(rx("^(?:cat|car)s?$", "cars") == V::Acceptable);
    UI_CHECK(rx("^(?:cat|car)s?$", "cab") == V::Invalid);
    UI_CHECK(rx("a*b+?c?", "aaab") == V::Acceptable);
    UI_CHECK(rx("[^0-9\\s]+", "ab c") == V::Invalid);
    UI_CHECK(rx("\\w+@\\w+\\.\\w{2,}", "me@x.i") == V::Intermediate);
    UI_CHECK(rx("x{2,}", "xxxxxxx") == V::Acceptable);
    UI_CHECK(rx("caf\u00e9.", "caf\u00e9\u00fc") == V::Acceptable); // code points, not bytes
    UI_CHECK(rx("", "") == V::Acceptable && rx("", "a") == V::Invalid);
    bool sup = true;
    UI_CHECK(rx("(a)\\1", "aa") == V::Acceptable); // backreference: std::regex fallback
    w::regexPartialMatch("(a)\\1", "", &sup);
    UI_CHECK(!sup);
    w::regexPartialMatch("[a-c]+(?:x|y){1,3}", "", &sup);
    UI_CHECK(sup);
    // a regex-validated field now rejects dead keystrokes
    const NodeHandle code = w::lineEdit(ui, col, "", "code");
    w::setLineEditOptions(ui, code, {.validator = w::regexValidator("[A-Z]{2}\\d{3}")});
    ui.setFocus(code);
    for (char ch : std::string("Ab1C23456")) type(ui, std::string(1, ch));
    UI_CHECK(w::lineEditText(ui, code) == "AC234"); // 'b' (lower), '1' after one letter, '5', '6' (too long): dead
    w::setLineEditText(ui, code, "");
    for (char ch : std::string("AB12x345")) type(ui, std::string(1, ch));
    UI_CHECK(w::lineEditText(ui, code) == "AB123");
    UI_CHECK(w::lineEditState(ui, code) == V::Acceptable);

    // ---- fixup + editingFinished ----
    const NodeHandle age = w::lineEdit(ui, col, "", "age");
    int finished = 0, ageSubmit = 0;
    std::string finText;
    ui.connect(age, SignalKind::EditingFinished, [&](UiContext&, NodeHandle, const SignalArgs& a) { ++finished; finText.assign(a.text); });
    ui.connect(age, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs&) { ++ageSubmit; });
    w::setLineEditOptions(ui, age, {.validator = w::intValidator(18, 120), .fixup = w::intFixup(18, 120)});
    ui.setFocus(age);
    type(ui, "7");
    ui.keyDown(Key::Enter); // "7" Intermediate -> fixup clamps to 18
    UI_CHECK(w::lineEditText(ui, age) == "18" && ageSubmit == 1 && finished == 1 && finText == "18");
    ui.setFocus(plain); // no edit since: focus loss does not repeat it
    UI_CHECK(finished == 1);
    ui.setFocus(age);
    type(ui, "0"); // 180 -> rejected (Invalid); type a valid edit instead
    ui.keyDown(Key::Backspace);
    type(ui, "9"); // "19"
    ui.setFocus(plain);
    UI_CHECK(finished == 2 && finText == "19" && ageSubmit == 1);
    w::setLineEditOptions(ui, age, {.validator = w::intValidator(18, 120)}); // no fixup: Intermediate never finishes
    ui.setFocus(age);
    w::setLineEditText(ui, age, "");
    type(ui, "5");
    ui.setFocus(plain);
    UI_CHECK(finished == 2);
    std::string dfx = "3.14159";
    w::doubleFixup(0.0, 3.0, 2)(dfx);
    UI_CHECK(dfx == "3.00");

    // ---- input mask ----
    const NodeHandle ip = w::lineEdit(ui, col, "", "ip");
    w::setLineEditOptions(ui, ip, {.inputMask = "000.000.000.000;_"});
    UI_CHECK(ui.editBuffer(ip).text() == "___.___.___.___");
    UI_CHECK(w::lineEditState(ui, ip) == V::Acceptable); // all optional
    ui.setFocus(ip);
    ui.keyDown(Key::Home);
    type(ui, "192.168.1.1");
    UI_CHECK(ui.editBuffer(ip).text() == "192.168.1__.1__");
    UI_CHECK(w::lineEditText(ui, ip) == "192.168.1.1");
    const NodeHandle lic = w::lineEdit(ui, col, "", "lic");
    std::string licSignal;
    ui.connect(lic, SignalKind::TextChanged, [&](UiContext&, NodeHandle, const SignalArgs& a) { licSignal.assign(a.text); });
    w::setLineEditOptions(ui, lic, {.inputMask = ">AAA-999<aa"});
    ui.setFocus(lic);
    type(ui, "ab1");   // '1' is not a letter: dropped
    UI_CHECK(ui.editBuffer(lic).text() == "AB -     ");
    type(ui, "c-42");  // '-' typed at the literal steps over it
    UI_CHECK(ui.editBuffer(lic).text() == "ABC-42   ");
    UI_CHECK(w::lineEditState(ui, lic) == V::Intermediate);
    type(ui, "7XY");
    UI_CHECK(ui.editBuffer(lic).text() == "ABC-427xy" && w::lineEditText(ui, lic) == "ABC-427xy" && licSignal == "ABC-427xy");
    UI_CHECK(w::lineEditState(ui, lic) == V::Acceptable);
    type(ui, "z"); // full: ignored
    UI_CHECK(w::lineEditText(ui, lic) == "ABC-427xy");
    ui.keyDown(Key::Backspace);
    ui.keyDown(Key::Backspace);
    UI_CHECK(ui.editBuffer(lic).text() == "ABC-427  ");
    ui.keyDown(Key::Backspace); // over nothing literal: clears '7'
    ui.keyDown(Key::Backspace);
    ui.keyDown(Key::Backspace);
    ui.keyDown(Key::Backspace); // steps over '-' and clears 'C'
    UI_CHECK(ui.editBuffer(lic).text() == "AB -     " && w::lineEditText(ui, lic) == "AB-");
    ui.keyDown(Key::A, kModCtrl);
    ui.keyDown(Key::Delete);
    UI_CHECK(ui.editBuffer(lic).text() == "   -     ");
    w::setLineEditText(ui, lic, "xyz123ab");
    UI_CHECK(w::lineEditText(ui, lic) == "XYZ-123ab");
    ui.keyDown(Key::A, kModCtrl);
    ui.keyDown(Key::C, kModCtrl);
    ui.clipboardText(clip);
    UI_CHECK(clip == "XYZ-123ab");
    const NodeHandle esc = w::lineEdit(ui, col, "", "esc");
    w::setLineEditOptions(ui, esc, {.inputMask = "\\A99"});
    ui.setFocus(esc);
    type(ui, "5");
    UI_CHECK(ui.editBuffer(esc).text() == "A5 " && w::lineEditState(ui, esc) == V::Intermediate);

    // ---- protected a11y flag ----
    const NodeHandle pw2 = w::lineEdit(ui, col, "", "pw2");
    w::setLineEditOptions(ui, pw2, {.echo = w::EchoMode::Password});
    ui.update();
    ui.snapshotA11y(snap);
    bool prot = false, plainProt = true;
    for (const auto& s : snap) {
        if (s.node == pw2) prot = s.protectedText;
        if (s.node == plain) plainProt = s.protectedText;
    }
    UI_CHECK(prot && !plainProt);
    w::setLineEditOptions(ui, pw2, {});
    UI_CHECK(!ui.a11yProtected(pw2));
    return fuse::ui::test::finish("fuse_ui_lineedit");
}
