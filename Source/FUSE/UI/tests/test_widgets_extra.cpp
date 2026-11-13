// fuse_ui_widgets_extra: the widgets of widgets/extra.hpp (ported from the gui-framework branch's Extras): dialog
// button box, dialog default / reject buttons, input dialog, tool box, LCD number, key sequence edit, MDI area,
// graphics scene / view (rubber-band selection), on-screen keyboard; their accessibility and their class registration.
// `--gate=<name>` runs one group (ctest fuse_ui_extra_<name>); `--out=<png>` writes the scene as an image.
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
void step(UiContext& ui) { ui.update(1.f / 60.f); }
void click(UiContext& ui, Vec2 p) {
    ui.pointerMove(p);
    ui.pointerDown(p);
    ui.pointerUp(p);
    step(ui);
}
void clickNode(UiContext& ui, NodeHandle n) { click(ui, ctr(ui.rect(n))); }
void key(UiContext& ui, Key k, u8 mods = 0) {
    ui.keyDown(k, mods);
    ui.keyUp(k, mods);
    step(ui);
}
struct Fixture {
    UiContext ui{{.viewport = {900, 640}, .font = &font()}};
    Fixture() {
        w::install(ui);
        ui.setCss(ui.root(), "padding", "10px");
    }
};
A11ySnapshotNode a11yOf(UiContext& ui, NodeHandle n) {
    std::vector<A11ySnapshotNode> snap;
    ui.update();
    ui.snapshotA11y(snap);
    for (const auto& s : snap)
        if (s.node == n && s.child == 0) return s;
    return {};
}
std::vector<A11ySnapshotNode> a11yItems(UiContext& ui, NodeHandle n) {
    std::vector<A11ySnapshotNode> snap, out;
    ui.update();
    ui.snapshotA11y(snap);
    for (auto& s : snap)
        if (s.node == n && s.child != 0) out.push_back(s);
    return out;
}
struct Result {
    f32 value = -1.f;
    std::string text;
    int count = 0;
};
Result& result() {
    static Result r;
    return r;
}
void watch(UiContext& ui, NodeHandle n) {
    result() = {};
    ui.connect(n, SignalKind::Submitted, [](UiContext&, NodeHandle, const SignalArgs& a) {
        result().value = a.value;
        result().text.assign(a.text);
        ++result().count;
    });
}

// ---- dialog button box: standard buttons, roles, platform orders, signals, layout ----------------------------------
void gateButtonBox() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle col = w::panel(ui, ui.root());
    ui.setCss(col, "width", "640px");
    const u32 mask = w::StdButton::Ok | w::StdButton::Cancel | w::StdButton::Apply | w::StdButton::Help | w::StdButton::Reset;
    const NodeHandle box = w::dialogButtonBox(ui, col, mask, {.layout = w::ButtonBoxLayout::Windows}, "bb");
    ui.update();
    auto texts = [&] {
        std::string s;
        for (NodeHandle b : w::dialogButtonBoxButtons(ui, box)) {
            if (!s.empty()) s += ',';
            s += std::string(ui.text(b));
        }
        return s;
    };
    UI_CHECK_EQ(texts(), std::string("Reset,OK,Cancel,Apply,Help"));
    w::setDialogButtonBoxOptions(ui, box, {.layout = w::ButtonBoxLayout::Mac});
    UI_CHECK_EQ(texts(), std::string("Help,Reset,Apply,Cancel,OK"));
    w::setDialogButtonBoxOptions(ui, box, {.layout = w::ButtonBoxLayout::Kde});
    UI_CHECK_EQ(texts(), std::string("Help,Reset,OK,Apply,Cancel"));
    w::setDialogButtonBoxOptions(ui, box, {.layout = w::ButtonBoxLayout::Gnome});
    UI_CHECK_EQ(texts(), std::string("Help,Reset,Apply,Cancel,OK"));

    // Discard text follows the platform; Yes / No / custom roles
    UI_CHECK(w::standardButtonText(w::StdButton::Discard, w::ButtonBoxLayout::Mac) == "Don't Save");
    UI_CHECK(w::standardButtonText(w::StdButton::Discard, w::ButtonBoxLayout::Windows) == "Discard");
    UI_CHECK(w::standardButtonText(0x1) .empty());
    w::setDialogButtonBoxStandardButtons(ui, box, w::StdButton::Save | w::StdButton::Discard | w::StdButton::Cancel);
    UI_CHECK_EQ(texts(), std::string("Close without Saving,Cancel,Save"));
    w::setDialogButtonBoxOptions(ui, box, {.layout = w::ButtonBoxLayout::Windows});
    UI_CHECK_EQ(texts(), std::string("Save,Discard,Cancel"));
    const NodeHandle more = w::dialogButtonBoxAdd(ui, box, "Details", w::ButtonRole::Action);
    UI_CHECK_EQ(texts(), std::string("Save,Discard,Details,Cancel"));
    UI_CHECK(w::dialogButtonBoxRole(ui, box, more) == w::ButtonRole::Action && w::dialogButtonBoxStandardOf(ui, box, more) == 0);
    const NodeHandle save = w::dialogButtonBoxButton(ui, box, w::StdButton::Save);
    const NodeHandle cancel = w::dialogButtonBoxButton(ui, box, w::StdButton::Cancel);
    UI_CHECK(ui.alive(save) && w::dialogButtonBoxStandardOf(ui, box, save) == w::StdButton::Save);
    UI_CHECK(w::dialogButtonBoxRole(ui, box, save) == w::ButtonRole::Accept && w::dialogButtonBoxRole(ui, box, cancel) == w::ButtonRole::Reject);
    // the default button is the first Accept (main's "primary" look), the reject button the first Reject
    UI_CHECK(w::dialogButtonBoxDefault(ui, box) == save && ui.hasClass(save, "primary") && ui.hasClass(save, "dialog-default"));
    UI_CHECK(w::dialogButtonBoxRejectButton(ui, box) == cancel && ui.hasClass(cancel, "dialog-reject"));
    UI_CHECK(!ui.alive(w::dialogButtonBoxButton(ui, box, w::StdButton::Ok)));

    // signals: Clicked {role}, then Submitted 1 (accepted) / 0 (rejected)
    static int accepted, rejected, clicks;
    static f32 lastRole;
    accepted = rejected = clicks = 0;
    lastRole = -1.f;
    ui.connect(box, SignalKind::Clicked, [](UiContext&, NodeHandle, const SignalArgs& a) { ++clicks; lastRole = a.value; });
    ui.connect(box, SignalKind::Submitted, [](UiContext&, NodeHandle, const SignalArgs& a) { (a.value > 0.5f ? accepted : rejected)++; });
    ui.update();
    clickNode(ui, save);
    UI_CHECK(clicks == 1 && accepted == 1 && rejected == 0 && lastRole == f32(u8(w::ButtonRole::Accept)));
    clickNode(ui, cancel);
    UI_CHECK(clicks == 2 && rejected == 1);
    clickNode(ui, more);
    UI_CHECK(clicks == 3 && accepted == 1 && rejected == 1 && lastRole == f32(u8(w::ButtonRole::Action)));
    w::dialogButtonBoxRemove(ui, box, more);
    UI_CHECK_EQ(texts(), std::string("Save,Discard,Cancel"));
    UI_CHECK(!ui.alive(more));

    // layout: buttons pushed right by the stretch; centred; vertical stacks
    ui.update();
    const Rect br = ui.rect(box);
    UI_CHECK(br.w > 500.f && ui.rect(cancel).right() > br.right() - 20.f && ui.rect(save).x > br.x + 200.f);
    w::setDialogButtonBoxOptions(ui, box, {.layout = w::ButtonBoxLayout::Windows, .centerButtons = true});
    ui.update();
    UI_CHECK(std::abs((ui.rect(save).x - br.x) - (br.right() - ui.rect(cancel).right())) < 4.f);
    w::setDialogButtonBoxOptions(ui, box, {.layout = w::ButtonBoxLayout::Windows, .vertical = true});
    ui.update();
    const Rect s1 = ui.rect(save), d1 = ui.rect(w::dialogButtonBoxButton(ui, box, w::StdButton::Discard));
    UI_CHECK(d1.y > s1.y && std::abs(d1.x - s1.x) < 1.f);
    // Yes / No box: Yes is the default, No the reject button
    const NodeHandle yn = w::dialogButtonBox(ui, col, w::StdButton::Yes | w::StdButton::No | w::StdButton::Help);
    UI_CHECK(w::dialogButtonBoxDefault(ui, yn) == w::dialogButtonBoxButton(ui, yn, w::StdButton::Yes));
    UI_CHECK(w::dialogButtonBoxRejectButton(ui, yn) == w::dialogButtonBoxButton(ui, yn, w::StdButton::No));
    // a11y: a named group of plain buttons
    const A11ySnapshotNode g = a11yOf(ui, box);
    UI_CHECK(g.role == A11yRole::Group && g.name == "Dialog buttons" && a11yOf(ui, save).role == A11yRole::Button);
    // not a button box: every call is a no-op
    UI_CHECK(w::dialogButtonBoxButtons(ui, col).empty() && !w::dialogButtonBoxAdd(ui, col, "x", w::ButtonRole::Action).valid());
    UI_CHECK(!w::dialogButtonBoxDefault(ui, col).valid());
}

// ---- QDialog keys: Escape = reject button, unconsumed Enter = default button ---------------------------------------
void gateDialogKeys() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle dl = w::dialog(ui, "D", "dlg");
    const NodeHandle body = w::dialogBody(ui, dl);
    const NodeHandle field = w::lineEdit(ui, body, "", "field");
    const NodeHandle check = w::checkBox(ui, body, "option", false, "check");
    const NodeHandle ok = w::button(ui, body, "Go", "go"), no = w::button(ui, body, "Stop", "stop");
    static int go, stop, submitted;
    go = stop = submitted = 0;
    ui.connect(ok, SignalKind::Clicked, [](UiContext&, NodeHandle, const SignalArgs&) { ++go; });
    ui.connect(no, SignalKind::Clicked, [](UiContext&, NodeHandle, const SignalArgs&) { ++stop; });
    ui.connect(field, SignalKind::Submitted, [](UiContext&, NodeHandle, const SignalArgs&) { ++submitted; });
    UI_CHECK(!w::dialogDefaultButton(ui, dl).valid() && !w::dialogRejectButton(ui, dl).valid());
    w::setDialogDefaultButton(ui, dl, ok);
    w::setDialogRejectButton(ui, dl, no);
    UI_CHECK(w::dialogDefaultButton(ui, dl) == ok && w::dialogRejectButton(ui, dl) == no);
    w::openDialog(ui, dl);
    step(ui);
    // focus on a widget that does not use Enter: the default button is pressed
    ui.setFocus(check);
    key(ui, Key::Enter);
    UI_CHECK(go == 1 && stop == 0);
    // a line edit keeps Enter for Submitted (the default button is not pressed twice over)
    ui.setFocus(field);
    key(ui, Key::Enter);
    UI_CHECK(submitted == 1 && go == 1);
    // a focused push button presses itself, not the default
    ui.setFocus(no);
    key(ui, Key::Enter);
    UI_CHECK(stop == 1 && go == 1);
    // Escape presses the reject button (its slot decides: here the dialog stays open)
    ui.setFocus(field);
    key(ui, Key::Escape);
    UI_CHECK(stop == 2 && w::popupOpen(ui, ui.parent(dl)));
    // moving the default: only one button carries the class
    w::setDialogDefaultButton(ui, dl, no);
    UI_CHECK(w::dialogDefaultButton(ui, dl) == no && !ui.hasClass(ok, "dialog-default"));
    ui.setFocus(check);
    key(ui, Key::Enter);
    UI_CHECK(stop == 3 && go == 1);
    // a button outside the dialog is refused; an invalid handle clears
    const NodeHandle outside = w::button(ui, ui.root(), "outside");
    w::setDialogDefaultButton(ui, dl, outside);
    UI_CHECK(!w::dialogDefaultButton(ui, dl).valid() && !ui.hasClass(outside, "dialog-default"));
    ui.setFocus(check);
    key(ui, Key::Enter);
    UI_CHECK(stop == 3 && go == 1); // no default button: Enter does nothing
    // no enabled reject button: Escape closes the dialog as before
    ui.setDisabled(no, true);
    step(ui);
    key(ui, Key::Escape);
    UI_CHECK(stop == 3 && !w::popupOpen(ui, ui.parent(dl)));
    // a dialog without either keeps main's behaviour: Escape closes
    const NodeHandle plain = w::dialog(ui, "P");
    w::button(ui, w::dialogBody(ui, plain), "x");
    w::openDialog(ui, plain);
    step(ui);
    key(ui, Key::Escape);
    UI_CHECK(!w::popupOpen(ui, ui.parent(plain)));
    // message boxes answer Escape with their Cancel button
    NodeHandle mb = w::messageBox(ui, "Q", "Save?", "Save\nDiscard\nCancel");
    watch(ui, mb);
    step(ui);
    key(ui, Key::Escape);
    UI_CHECK(result().count == 1 && result().value == 2.f && result().text == "Cancel");
    mb = w::messageBox(ui, "Q", "Pick", "Alpha\nBeta"); // no reject-like button: Escape just closes
    watch(ui, mb);
    step(ui);
    key(ui, Key::Escape);
    UI_CHECK(result().count == 0 && !w::popupOpen(ui, ui.parent(mb)));
    // wizard validator: Next / Finish are blocked while it says no; Back is not
    const NodeHandle wiz = w::wizard(ui, "Setup", "wiz");
    w::wizardAddPage(ui, wiz, "One");
    w::wizardAddPage(ui, wiz, "Two");
    static bool allow;
    static u32 askedPage;
    allow = false;
    askedPage = 99;
    w::setWizardValidator(ui, wiz, [](UiContext&, NodeHandle, u32 page, void*) { askedPage = page; return allow; });
    w::openWizard(ui, wiz);
    watch(ui, wiz);
    step(ui);
    clickNode(ui, w::wizardButton(ui, wiz, w::WizardButton::Next));
    UI_CHECK(w::wizardPage(ui, wiz) == 0 && askedPage == 0);
    allow = true;
    clickNode(ui, w::wizardButton(ui, wiz, w::WizardButton::Next));
    UI_CHECK(w::wizardPage(ui, wiz) == 1);
    allow = false;
    clickNode(ui, w::wizardButton(ui, wiz, w::WizardButton::Next)); // Finish on the last page
    UI_CHECK(result().count == 0 && askedPage == 1 && w::popupOpen(ui, ui.parent(wiz)));
    clickNode(ui, w::wizardButton(ui, wiz, w::WizardButton::Back));
    UI_CHECK(w::wizardPage(ui, wiz) == 0);
    w::setWizardValidator(ui, wiz, nullptr); // removed: nothing blocks
    clickNode(ui, w::wizardButton(ui, wiz, w::WizardButton::Next));
    clickNode(ui, w::wizardButton(ui, wiz, w::WizardButton::Next));
    UI_CHECK(result().count == 1 && result().text == "finish");
}

// ---- input dialog --------------------------------------------------------------------------------------------------------
void gateInputDialog() {
    Fixture f;
    UiContext& ui = f.ui;
    NodeHandle in = w::inputDialog(ui, "Rename", "New name:", "bob", "rename");
    watch(ui, in);
    step(ui);
    const NodeHandle le = w::inputDialogEdit(ui, in);
    UI_CHECK(le.valid() && ui.focused() == le && w::lineEditText(ui, le) == "bob" && w::popupOpen(ui, ui.parent(in)));
    UI_CHECK(ui.editBuffer(le).hasSelection()); // the initial text is selected: typing replaces it
    ui.textInput("alice");
    key(ui, Key::Enter);
    UI_CHECK(result().count == 1 && result().value == 1.f && result().text == "alice" && !w::popupOpen(ui, ui.parent(in)));
    // OK button
    in = w::inputDialog(ui, "Rename", "New name:", "carol");
    watch(ui, in);
    step(ui);
    const NodeHandle box = ui.nextSibling(w::inputDialogEdit(ui, in));
    clickNode(ui, w::dialogButtonBoxButton(ui, box, w::StdButton::Ok));
    UI_CHECK(result().count == 1 && result().value == 1.f && result().text == "carol");
    // Cancel button, and Escape (the box's reject button)
    in = w::inputDialog(ui, "Rename", "New name:", "x");
    watch(ui, in);
    step(ui);
    clickNode(ui, w::dialogButtonBoxButton(ui, ui.nextSibling(w::inputDialogEdit(ui, in)), w::StdButton::Cancel));
    UI_CHECK(result().count == 1 && result().value == 0.f && !w::popupOpen(ui, ui.parent(in)));
    in = w::inputDialog(ui, "Rename", "New name:", "x");
    watch(ui, in);
    step(ui);
    key(ui, Key::Escape);
    UI_CHECK(result().count == 1 && result().value == 0.f && !w::popupOpen(ui, ui.parent(in)));
    // a validator on the field gates OK and Enter
    in = w::inputDialog(ui, "Port", "Port:", "");
    watch(ui, in);
    step(ui);
    const NodeHandle port = w::inputDialogEdit(ui, in);
    w::setLineEditOptions(ui, port, {.validator = w::intValidator(1, 65535)});
    ui.setFocus(port);
    key(ui, Key::Enter); // empty: Intermediate
    clickNode(ui, w::dialogButtonBoxButton(ui, ui.nextSibling(port), w::StdButton::Ok));
    UI_CHECK(result().count == 0 && w::popupOpen(ui, ui.parent(in)));
    ui.setFocus(port);
    for (const char* ch : {"8", "0", "a", "8"}) ui.textInput(ch); // the letter is rejected
    key(ui, Key::Enter);
    UI_CHECK(result().count == 1 && result().text == "808");
    // a11y: the field is named by the prompt
    in = w::inputDialog(ui, "Rename", "New name:", "x");
    step(ui);
    UI_CHECK(a11yOf(ui, w::inputDialogEdit(ui, in)).name == "New name:" && a11yOf(ui, in).role == A11yRole::Dialog);
    UI_CHECK(!w::inputDialogEdit(ui, ui.root()).valid());
}

// ---- tool box ----------------------------------------------------------------------------------------------------------------
void gateToolBox() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle tb = w::toolBox(ui, ui.root(), "General\nAdvanced\nAbout", "tb");
    for (u32 i = 0; i < 3; ++i) w::label(ui, w::toolBoxPage(ui, tb, i), i == 0 ? "general page" : i == 1 ? "advanced page" : "about page");
    static int changed;
    changed = 0;
    ui.connect(tb, SignalKind::ValueChanged, [](UiContext&, NodeHandle, const SignalArgs&) { ++changed; });
    step(ui);
    UI_CHECK(w::toolBoxCount(ui, tb) == 3 && u32(ui.value(tb)) == 0);
    UI_CHECK(ui.computed(w::toolBoxPage(ui, tb, 0)).visible && !ui.computed(w::toolBoxPage(ui, tb, 1)).visible && !ui.computed(w::toolBoxPage(ui, tb, 2)).visible);
    UI_CHECK(ui.text(w::toolBoxHeader(ui, tb, 1)) == "Advanced" && ui.hasClass(w::toolBoxHeader(ui, tb, 0), "open"));
    // headers stack: 0, page 0, 1, 2
    UI_CHECK(ui.rect(w::toolBoxHeader(ui, tb, 1)).y > ui.rect(w::toolBoxPage(ui, tb, 0)).bottom() - 1.f);
    clickNode(ui, w::toolBoxHeader(ui, tb, 2));
    UI_CHECK(u32(ui.value(tb)) == 2 && changed == 1);
    UI_CHECK(ui.computed(w::toolBoxPage(ui, tb, 2)).visible && !ui.computed(w::toolBoxPage(ui, tb, 0)).visible);
    UI_CHECK(ui.hasClass(w::toolBoxHeader(ui, tb, 2), "open") && !ui.hasClass(w::toolBoxHeader(ui, tb, 0), "open"));
    clickNode(ui, w::toolBoxHeader(ui, tb, 2)); // the open one again: no change
    UI_CHECK(changed == 1);
    // keyboard: Up / Down walk the headers, Space / Enter open
    ui.setFocus(w::toolBoxHeader(ui, tb, 2), FocusReason::Keyboard);
    key(ui, Key::Up);
    UI_CHECK(ui.focused() == w::toolBoxHeader(ui, tb, 1));
    key(ui, Key::Up);
    key(ui, Key::Up); // stays on the first
    UI_CHECK(ui.focused() == w::toolBoxHeader(ui, tb, 0));
    key(ui, Key::Down);
    key(ui, Key::Enter);
    UI_CHECK(u32(ui.value(tb)) == 1 && changed == 2 && ui.computed(w::toolBoxPage(ui, tb, 1)).visible);
    // programmatic
    w::setToolBoxIndex(ui, tb, 0);
    w::setToolBoxIndex(ui, tb, 9); // out of range: ignored
    step(ui);
    UI_CHECK(u32(ui.value(tb)) == 0 && changed == 3);
    const NodeHandle extra = w::toolBoxAddPage(ui, tb, "Extra");
    step(ui);
    UI_CHECK(extra.valid() && w::toolBoxCount(ui, tb) == 4 && w::toolBoxPage(ui, tb, 3) == extra && !ui.computed(extra).visible);
    clickNode(ui, w::toolBoxHeader(ui, tb, 3));
    UI_CHECK(u32(ui.value(tb)) == 3 && ui.computed(extra).visible);
    // a11y: headers are buttons (the open one checked), pages groups named by their title
    const A11ySnapshotNode h3 = a11yOf(ui, w::toolBoxHeader(ui, tb, 3)), h0 = a11yOf(ui, w::toolBoxHeader(ui, tb, 0));
    UI_CHECK(h3.role == A11yRole::Button && h3.name == "Extra" && h3.checked && !h0.checked);
    UI_CHECK(a11yOf(ui, extra).role == A11yRole::Group && a11yOf(ui, extra).name == "Extra");
    // an empty tool box takes pages later; not a tool box: nothing
    const NodeHandle empty = w::toolBox(ui, ui.root(), "");
    UI_CHECK(w::toolBoxCount(ui, empty) == 0 && !w::toolBoxPage(ui, empty, 0).valid());
    const NodeHandle first = w::toolBoxAddPage(ui, empty, "Only");
    step(ui);
    UI_CHECK(w::toolBoxCount(ui, empty) == 1 && ui.computed(first).visible);
    UI_CHECK(w::toolBoxCount(ui, ui.root()) == 0 && !w::toolBoxAddPage(ui, ui.root(), "x").valid());
}

// ---- LCD number ----------------------------------------------------------------------------------------------------------
/// Lit segments (and dots) of an LCD: the small rects inside it in the full text colour (unlit segments: 8% alpha).
bool lcdCmd(const UiContext& ui, NodeHandle n, const DrawCmd& d) {
    const Rect c = ui.contentRect(n);
    return d.op == DrawOp::Rect && d.rect.w < 30.f && d.rect.h < 30.f && c.contains({d.rect.x + d.rect.w * 0.5f, d.rect.y + d.rect.h * 0.5f});
}
u32 litCount(const UiContext& ui, NodeHandle n) {
    const Color on = ui.computed(n).color;
    u32 c = 0;
    for (const DrawCmd& d : ui.drawList().cmds())
        if (lcdCmd(ui, n, d) && std::abs(d.fill.g - on.g) < 0.01f && std::abs(d.fill.r - on.r) < 0.01f && d.fill.a > 0.9f) ++c;
    return c;
}
u32 dimCount(const UiContext& ui, NodeHandle n) {
    u32 c = 0;
    for (const DrawCmd& d : ui.drawList().cmds())
        if (lcdCmd(ui, n, d) && d.fill.a > 0.01f && d.fill.a < 0.2f) ++c;
    return c;
}
void gateLcd() {
    Fixture f;
    UiContext& ui = f.ui;
    UI_CHECK(w::sevenSegment('8') == 0x7F && w::sevenSegment('1') == 0x06 && w::sevenSegment(' ') == 0 && w::sevenSegment('-') == 0x40);
    UI_CHECK(w::sevenSegment('A') == w::sevenSegment('a') && w::sevenSegment('?') == 0);
    const NodeHandle lcd = w::lcdNumber(ui, ui.root(), 6, "lcd");
    ui.setValue(lcd, 42.5f);
    UI_CHECK(w::lcdText(ui, lcd) == "42.5");
    ui.setValue(lcd, -17.f);
    UI_CHECK(w::lcdText(ui, lcd) == "-17");
    ui.setValue(lcd, 123456.f);
    UI_CHECK(w::lcdText(ui, lcd) == "123456");
    ui.setValue(lcd, 12345678.f); // more digits than the display has
    UI_CHECK(w::lcdText(ui, lcd) == "E");
    w::setLcdBase(ui, lcd, 16);
    ui.setValue(lcd, 255.f);
    UI_CHECK(w::lcdText(ui, lcd) == "ff");
    w::setLcdBase(ui, lcd, 2);
    ui.setValue(lcd, 5.f);
    UI_CHECK(w::lcdText(ui, lcd) == "101");
    ui.setValue(lcd, -5.f);
    UI_CHECK(w::lcdText(ui, lcd) == "-101");
    ui.setValue(lcd, 255.f); // 8 binary digits on a 6 digit display
    UI_CHECK(w::lcdText(ui, lcd) == "E");
    w::setLcdBase(ui, lcd, 7); // not a base it has: decimal
    UI_CHECK(w::lcdText(ui, lcd) == "255");
    w::setLcdText(ui, lcd, "12:34");
    UI_CHECK(w::lcdText(ui, lcd) == "12:34");
    ui.setValue(lcd, 9.f); // the text stays until the value is asked for again
    UI_CHECK(w::lcdText(ui, lcd) == "12:34");
    w::setLcdValue(ui, lcd, 8.f);
    UI_CHECK(w::lcdText(ui, lcd) == "8");
    // painting: one seven-segment cell per shown character, lit per sevenSegment()
    step(ui);
    UI_CHECK_EQ(litCount(ui, lcd), 7u); // "8"
    UI_CHECK_EQ(dimCount(ui, lcd), 0u);
    w::setLcdText(ui, lcd, "1.5");
    step(ui);
    UI_CHECK_EQ(litCount(ui, lcd), 2u + 5u + 1u); // 1, 5 and the decimal point
    UI_CHECK_EQ(dimCount(ui, lcd), 5u + 2u);
    w::setLcdText(ui, lcd, "12:34");
    step(ui);
    UI_CHECK_EQ(litCount(ui, lcd), 2u + 5u + 2u + 5u + 4u); // the colon is two dots
    // a11y: text with the shown string as value
    const A11ySnapshotNode a = a11yOf(ui, lcd);
    UI_CHECK(a.role == A11yRole::Text && a.valueText == "12:34");
    UI_CHECK(w::lcdText(ui, ui.root()).empty());
}

// ---- key sequence edit ------------------------------------------------------------------------------------------------------
void gateKeySequence() {
    Fixture f;
    UiContext& ui = f.ui;
    setPrimaryModifier(kModCtrl);
    const NodeHandle ks = w::keySequenceEdit(ui, ui.root(), {}, "ks");
    const NodeHandle preset = w::keySequenceEdit(ui, ui.root(), Shortcut{Key::S, kModCtrl | kModShift}, "preset");
    const NodeHandle other = w::button(ui, ui.root(), "other", "other");
    static std::string rec, fin;
    static int changes;
    rec.clear();
    fin.clear();
    changes = 0;
    ui.connect(ks, SignalKind::Submitted, [](UiContext&, NodeHandle, const SignalArgs& a) { rec.assign(a.text); });
    ui.connect(ks, SignalKind::EditingFinished, [](UiContext&, NodeHandle, const SignalArgs& a) { fin.assign(a.text); });
    ui.connect(ks, SignalKind::TextChanged, [](UiContext&, NodeHandle, const SignalArgs&) { ++changes; });
    step(ui);
    UI_CHECK(ui.text(preset) == "Ctrl+Shift+S" && (w::keySequence(ui, preset) == Shortcut{Key::S, kModCtrl | kModShift}));
    UI_CHECK(ui.text(ks).empty() && !w::keySequence(ui, ks).valid());
    clickNode(ui, ks);
    UI_CHECK(ui.focused() == ks);
    ui.keyDown(Key::P, kModCtrl | kModAlt);
    UI_CHECK(rec == "Ctrl+Alt+P" && fin == "Ctrl+Alt+P" && ui.text(ks) == "Ctrl+Alt+P" && changes == 1);
    UI_CHECK((w::keySequence(ui, ks) == Shortcut{Key::P, kModCtrl | kModAlt}));
    ui.keyDown(Key::F5);
    UI_CHECK(rec == "F5" && changes == 2);
    // Escape and Backspace clear (TextChanged only), once
    rec.clear();
    ui.keyDown(Key::Escape);
    UI_CHECK(ui.text(ks).empty() && rec.empty() && changes == 3);
    ui.keyDown(Key::Backspace);
    UI_CHECK(changes == 3);
    // but Ctrl+Backspace is a chord
    ui.keyDown(Key::Backspace, kModCtrl);
    UI_CHECK(ui.text(ks) == "Ctrl+Backspace" && changes == 4);
    // Tab still moves the focus
    key(ui, Key::Tab);
    UI_CHECK(ui.focused() != ks && ui.text(ks) == "Ctrl+Backspace");
    // programmatic: no signals
    w::setKeySequence(ui, ks, Shortcut{Key::Q, kModCtrl});
    UI_CHECK(ui.text(ks) == "Ctrl+Q" && changes == 4);
    w::setKeySequence(ui, ks, {});
    UI_CHECK(ui.text(ks).empty());
    // a11y: text field with the chord as value; SetValue takes shortcut text
    const A11ySnapshotNode a = a11yOf(ui, preset);
    UI_CHECK(a.role == A11yRole::TextField && a.valueText == "Ctrl+Shift+S" && a.hasAction(A11yAction::SetValue));
    UI_CHECK(ui.performA11yAction(ks, A11yAction::SetValue, 0.f, 0, "Alt+F4") && ui.text(ks) == "Alt+F4" && rec == "Alt+F4");
    ui.performA11yAction(ks, A11yAction::SetValue, 0.f, 0, "Ctrl+NoSuchKey"); // not a shortcut: ignored, never stored
    UI_CHECK(ui.text(ks) == "Alt+F4" && rec == "Alt+F4");
    (void)other;
    setPrimaryModifier(0);
}

// ---- MDI area ------------------------------------------------------------------------------------------------------------------
void gateMdi() {
    Fixture f;
    UiContext& ui = f.ui;
    ui.setCss(ui.root(), "padding", "0px");
    const NodeHandle area = w::mdiArea(ui, ui.root(), "area");
    ui.setCss(area, "height", "600px");
    const NodeHandle a = w::mdiSubWindow(ui, area, "Doc A", {20, 20, 300, 200}, "a");
    const NodeHandle b = w::mdiSubWindow(ui, area, "Doc B", {100, 80, 300, 200}, "b");
    w::label(ui, w::mdiBody(ui, a), "A body");
    static int aActive, bClosed;
    aActive = bClosed = 0;
    ui.connect(a, SignalKind::Toggled, [](UiContext&, NodeHandle, const SignalArgs& t) { aActive += t.flag ? 1 : -1; });
    step(ui);
    UI_CHECK(w::mdiActive(ui, area) == b && ui.hasClass(b, "active") && !ui.hasClass(a, "active"));
    UI_CHECK(w::mdiSubWindows(ui, area) == (std::vector<NodeHandle>{a, b}));
    const Rect ao = ui.contentRect(area);
    const Rect ra = ui.rect(a);
    UI_CHECK(near(ra.w, 300.f) && near(ra.h, 200.f) && near(ra.x, ao.x + 20.f) && near(ra.y, ao.y + 20.f));
    // a press on a part of A that B does not cover raises and activates it
    click(ui, {ra.x + 10.f, ra.y + ra.h - 20.f});
    UI_CHECK(w::mdiActive(ui, area) == a && ui.hasClass(a, "active") && !ui.hasClass(b, "active") && aActive == 1);
    // drag A's title bar by (+50, +30)
    const Rect tbar = ui.rect(ui.firstChild(a));
    const Vec2 p0{tbar.x + 30.f, tbar.y + tbar.h * 0.5f};
    ui.pointerMove(p0);
    ui.pointerDown(p0);
    step(ui);
    ui.pointerMove({p0.x + 25.f, p0.y + 15.f});
    step(ui);
    ui.pointerMove({p0.x + 50.f, p0.y + 30.f});
    step(ui);
    ui.pointerUp({p0.x + 50.f, p0.y + 30.f});
    step(ui);
    const Rect g = w::mdiGeometry(ui, a);
    UI_CHECK(near(g.x, 70.f) && near(g.y, 50.f) && near(ui.rect(a).x, ao.x + 70.f) && near(g.w, 300.f));
    // the resize grip (bottom-right) grows it; the minimum size holds
    const NodeHandle grip = ui.nextSibling(w::mdiBody(ui, a));
    const Vec2 q = ctr(ui.rect(grip));
    UI_CHECK(ui.rect(grip).right() > ui.rect(a).right() - 4.f && ui.rect(grip).bottom() > ui.rect(a).bottom() - 4.f);
    ui.pointerMove(q);
    ui.pointerDown(q);
    ui.pointerMove({q.x + 40.f, q.y + 20.f});
    ui.pointerUp({q.x + 40.f, q.y + 20.f});
    step(ui);
    UI_CHECK(near(w::mdiGeometry(ui, a).w, 340.f) && near(w::mdiGeometry(ui, a).h, 220.f));
    const Vec2 q2 = ctr(ui.rect(grip));
    ui.pointerMove(q2);
    ui.pointerDown(q2);
    ui.pointerMove({q2.x - 900.f, q2.y - 900.f});
    ui.pointerUp({q2.x - 900.f, q2.y - 900.f});
    step(ui);
    UI_CHECK(near(w::mdiGeometry(ui, a).w, 80.f) && near(w::mdiGeometry(ui, a).h, 48.f));
    // dragged far out, the title stays reachable inside the area
    w::setMdiGeometry(ui, a, {-5000.f, -5000.f, 300.f, 200.f});
    UI_CHECK(near(w::mdiGeometry(ui, a).x, 40.f - 300.f) && near(w::mdiGeometry(ui, a).y, 0.f));
    w::setMdiGeometry(ui, a, {5000.f, 5000.f, 300.f, 200.f});
    UI_CHECK(near(w::mdiGeometry(ui, a).x, ao.w - 40.f) && near(w::mdiGeometry(ui, a).y, ao.h - 24.f));
    // tile: a grid over the area; cascade: stepped
    w::mdiTile(ui, area);
    step(ui);
    const Rect ta = w::mdiGeometry(ui, a), tb = w::mdiGeometry(ui, b);
    UI_CHECK(near(ta.w, tb.w) && ta.y == tb.y && near(ta.w + tb.w, ao.w, 2.f) && near(ta.h, ao.h, 1.f));
    w::mdiCascade(ui, area);
    step(ui);
    const std::vector<NodeHandle> order = w::mdiSubWindows(ui, area);
    UI_CHECK(near(w::mdiGeometry(ui, order[1]).x - w::mdiGeometry(ui, order[0]).x, 24.f));
    // activate programmatically; close with the title's x: Submitted, destroyed, the other takes over
    w::mdiActivate(ui, b);
    UI_CHECK(w::mdiActive(ui, area) == b && aActive == 0);
    ui.connect(b, SignalKind::Submitted, [](UiContext&, NodeHandle, const SignalArgs&) { ++bClosed; });
    w::mdiTile(ui, area);
    step(ui);
    const NodeHandle x = ui.nextSibling(ui.firstChild(ui.firstChild(b)));
    UI_CHECK(a11yOf(ui, x).name == "Close" && a11yOf(ui, b).role == A11yRole::Window && a11yOf(ui, b).name == "Doc B");
    clickNode(ui, x);
    UI_CHECK(bClosed == 1 && !ui.alive(b) && w::mdiSubWindows(ui, area).size() == 1 && w::mdiActive(ui, area) == a && aActive == 1);
    // touch: the title bar captures the drag (the area does not pan)
    const Rect t2 = ui.rect(ui.firstChild(a));
    const Rect before = w::mdiGeometry(ui, a);
    ui.touchDown({t2.x + 30.f, t2.y + t2.h * 0.5f});
    ui.touchMove({t2.x + 60.f, t2.y + t2.h * 0.5f + 40.f});
    ui.touchMove({t2.x + 90.f, t2.y + t2.h * 0.5f + 80.f});
    ui.touchUp({t2.x + 90.f, t2.y + t2.h * 0.5f + 80.f});
    step(ui);
    UI_CHECK(near(w::mdiGeometry(ui, a).x, before.x + 60.f, 9.f) && near(w::mdiGeometry(ui, a).y, before.y + 80.f, 9.f));
    // not an MDI node
    UI_CHECK(!w::mdiSubWindow(ui, ui.root(), "x", {0, 0, 100, 100}).valid() && w::mdiSubWindows(ui, ui.root()).empty());
    UI_CHECK(w::mdiGeometry(ui, area).empty() && !w::mdiBody(ui, area).valid());
}

// ---- graphics scene / view -----------------------------------------------------------------------------------------------
void gateGraphics() {
    using w::GraphicsScene;
    using w::GraphicsShape;
    GraphicsScene sc;
    const u32 r = sc.add({.shape = GraphicsShape::Rect, .rect = {0, 0, 100, 100}});
    const u32 e = sc.add({.shape = GraphicsShape::Ellipse, .rect = {50, 50, 100, 100}, .fill = Color::rgb(0xFF5555), .z = 1});
    const u32 l = sc.add({.shape = GraphicsShape::Line, .rect = {300, 0, 100, 100}, .strokeWidth = 2});
    const u32 p = sc.add({.shape = GraphicsShape::Polygon, .points = {{500, 0}, {600, 0}, {550, 100}}});
    sc.add({.shape = GraphicsShape::Text, .rect = {0, 200, 200, 30}, .fill = Color::rgb(0xFFFFFF), .text = "scene text"});
    UI_CHECK(sc.itemAt({90, 90}) == e); // the ellipse is above (z = 1)
    UI_CHECK(sc.itemAt({10, 10}) == r);
    UI_CHECK(sc.itemAt({52, 52}) == r); // outside the ellipse's curve
    UI_CHECK(sc.itemAt({350, 50}) == l && sc.itemAt({350, 60}) == ~0u);
    UI_CHECK(sc.itemAt({550, 50}) == p && sc.itemAt({505, 90}) == ~0u);
    UI_CHECK(sc.itemsIn({0, 0, 200, 200}).size() == 2);
    UI_CHECK(sc.paintOrder().back() == e); // highest z paints last
    // the spatial index under many items
    for (int i = 0; i < 20000; ++i) sc.add({.rect = {f32(1000 + (i % 200) * 20), f32((i / 200) * 20), 10, 10}, .z = -1});
    UI_CHECK(sc.itemAt({1000 + 20 * 5 + 5, 20 * 7 + 5}) != ~0u && sc.size() == 20005);
    UI_CHECK(sc.itemAt({1000 + 20 * 5 + 15, 20 * 7 + 5}) == ~0u); // the gap between two of them
    const Rect b = sc.bounds();
    UI_CHECK(b.x == 0 && b.right() >= 4990);
    static int changedCalls;
    changedCalls = 0;
    sc.onChanged = [](u32) { ++changedCalls; };
    sc.move(r, {5, 5});
    UI_CHECK(sc.item(r)->rect.x == 5 && sc.itemAt({3, 3}) == ~0u && sc.itemAt({104, 8}) == r && changedCalls == 1);
    sc.remove(r);
    UI_CHECK(!sc.item(r) && sc.itemAt({20, 20}) == ~0u && sc.size() == 20004 && changedCalls == 2);
    sc.remove(r); // twice: nothing
    UI_CHECK(changedCalls == 2);

    Fixture f;
    UiContext& ui = f.ui;
    ui.setCss(ui.root(), "padding", "0px");
    GraphicsScene small;
    const u32 a = small.add({.rect = {10, 10, 50, 50}, .text = "blue box"});
    const u32 c = small.add({.shape = GraphicsShape::Ellipse, .rect = {100, 10, 50, 50}, .fill = Color::rgb(0x55FF55), .text = "green dot"});
    const u32 fixed = small.add({.rect = {10, 120, 40, 40}, .text = "pinned", .movable = false});
    const NodeHandle v = w::graphicsView(ui, ui.root(), &small, "view");
    step(ui);
    static std::vector<int> sel;
    static int activated;
    sel.clear();
    activated = -1;
    ui.connect(v, SignalKind::ValueChanged, [](UiContext&, NodeHandle, const SignalArgs& g) { sel.push_back(int(g.value)); });
    ui.connect(v, SignalKind::Submitted, [](UiContext&, NodeHandle, const SignalArgs& g) { activated = int(g.value); });
    UI_CHECK(w::graphicsScene(ui, v) == &small && w::viewZoom(ui, v) == 1.f);
    const Vec2 pa = w::sceneToView(ui, v, {35, 35});
    click(ui, pa);
    UI_CHECK(sel == std::vector<int>{int(a)} && small.item(a)->selected && ui.focused() == v);
    click(ui, pa); // already selected: no new signal
    UI_CHECK(sel.size() == 1);
    // Ctrl+click toggles another one in and out
    const Vec2 pc = w::sceneToView(ui, v, {125, 35});
    ui.setModifiers(kModCtrl);
    click(ui, pc);
    UI_CHECK(small.selection().size() == 2 && sel.back() == int(c));
    click(ui, pc);
    ui.setModifiers(0);
    UI_CHECK(small.selection() == std::vector<u32>{a});
    // a drag moves the selected item (scene units = px at zoom 1)
    ui.pointerMove(pa);
    ui.pointerDown(pa);
    step(ui);
    ui.pointerMove({pa.x + 20.f, pa.y + 10.f});
    step(ui);
    ui.pointerUp({pa.x + 20.f, pa.y + 10.f});
    step(ui);
    UI_CHECK(near(small.item(a)->rect.x, 30.f, 0.01f) && near(small.item(a)->rect.y, 20.f, 0.01f));
    // zooming around the pointer keeps the scene point under it
    const Vec2 sp0 = w::viewToScene(ui, v, {200, 150});
    ui.wheel({200, 150}, {0, 2});
    const Vec2 sp1 = w::viewToScene(ui, v, {200, 150});
    UI_CHECK(w::viewZoom(ui, v) > 1.3f && near(sp0.x, sp1.x, 0.01f) && near(sp0.y, sp1.y, 0.01f));
    // Alt+drag pans
    w::setViewTransform(ui, v, 2.f, {0, 0});
    ui.setModifiers(kModAlt);
    ui.pointerMove({300, 200});
    ui.pointerDown({300, 200});
    ui.pointerMove({260, 180});
    ui.pointerUp({260, 180});
    ui.setModifiers(0);
    step(ui);
    UI_CHECK(near(w::viewPan(ui, v).x, 20.f, 0.01f) && near(w::viewPan(ui, v).y, 10.f, 0.01f) && small.selection() == std::vector<u32>{a});
    // rubber band over empty space: the band is reported while dragged, the items inside get selected on release
    w::setViewTransform(ui, v, 1.f, {0, 0});
    step(ui);
    const Vec2 o = w::sceneToView(ui, v, {0, 0}), q = w::sceneToView(ui, v, {200, 100});
    Rect band;
    UI_CHECK(!w::viewRubberBand(ui, v, band));
    ui.pointerMove({o.x + 1.f, o.y + 1.f});
    ui.pointerDown({o.x + 1.f, o.y + 1.f});
    step(ui);
    UI_CHECK(small.selection().empty() && sel.back() == -1); // the press on empty space cleared the selection
    ui.pointerMove(q);
    step(ui);
    UI_CHECK(w::viewRubberBand(ui, v, band) && near(band.x, o.x + 1.f) && near(band.w, 199.f) && near(band.h, 99.f));
    u32 bandCmds = 0; // the band is painted (a translucent fill + outline)
    for (const DrawCmd& d : ui.drawList().cmds())
        if (near(d.rect.x, band.x, 2.f) && near(d.rect.y, band.y, 2.f) && near(d.rect.w, 199.f, 3.f) && near(d.rect.h, 99.f, 3.f)) ++bandCmds;
    UI_CHECK(bandCmds >= 1);
    ui.pointerUp(q);
    step(ui);
    UI_CHECK(!w::viewRubberBand(ui, v, band) && small.selection() == (std::vector<u32>{a, c}) && !small.item(fixed)->selected);
    // Ctrl + rubber band adds to the selection
    const Vec2 f0 = w::sceneToView(ui, v, {0, 110}), f1 = w::sceneToView(ui, v, {80, 180});
    ui.setModifiers(kModCtrl);
    ui.pointerMove(f0);
    ui.pointerDown(f0);
    ui.pointerMove(f1);
    ui.pointerUp(f1);
    ui.setModifiers(0);
    step(ui);
    UI_CHECK(small.selection().size() == 3);
    // arrow keys move the movable ones only; a double click activates
    const f32 ax = small.item(a)->rect.x, fx = small.item(fixed)->rect.x;
    key(ui, Key::Right);
    UI_CHECK(near(small.item(a)->rect.x, ax + 10.f, 0.01f) && small.item(fixed)->rect.x == fx);
    const Vec2 pf = w::sceneToView(ui, v, {30, 140});
    ui.pointerMove(pf);
    ui.pointerDown(pf);
    ui.pointerUp(pf);
    ui.pointerDown(pf);
    ui.pointerUp(pf);
    step(ui);
    UI_CHECK(activated == int(fixed));
    // a11y: one child per item, named, with selection state; Select picks one
    const std::vector<A11ySnapshotNode> items = a11yItems(ui, v);
    UI_CHECK(items.size() == 3);
    if (items.size() == 3) {
        UI_CHECK(items[0].name == "blue box" && items[0].role == A11yRole::Image && items[0].selectable && items[0].setSize == 3);
        const Vec2 tl = w::sceneToView(ui, v, {small.item(a)->rect.x, small.item(a)->rect.y});
        UI_CHECK(near(items[0].bounds.x, tl.x) && near(items[0].bounds.w, 50.f) && items[0].hasAction(A11yAction::Select));
    }
    UI_CHECK(ui.performA11yAction(v, A11yAction::Select, 0.f, c + 1) && small.selection() == std::vector<u32>{c});
    UI_CHECK(!ui.performA11yAction(v, A11yAction::Select, 0.f, 99));
    // Ctrl+A selects all, Delete removes the selection
    key(ui, Key::A, kModCtrl);
    UI_CHECK(small.selection().size() == 3);
    key(ui, Key::Delete);
    UI_CHECK(small.size() == 0 && !small.item(c) && sel.back() == -1);
    small.add({.rect = {0, 0, 1000, 500}});
    w::fitInView(ui, v);
    const Rect vc = ui.contentRect(v);
    UI_CHECK(w::viewZoom(ui, v) < 1.f && near(w::sceneToView(ui, v, {500, 250}).x, vc.x + vc.w * 0.5f, 1.f));
    // touch: a drag on an item moves it (the view captures the drag)
    w::setViewTransform(ui, v, 1.f, {0, 0});
    const u32 t = small.add({.rect = {600, 200, 60, 60}});
    step(ui);
    const Vec2 tp = w::sceneToView(ui, v, {630, 230});
    ui.touchDown(tp);
    ui.touchMove({tp.x + 30.f, tp.y + 30.f});
    ui.touchMove({tp.x + 60.f, tp.y + 60.f});
    ui.touchUp({tp.x + 60.f, tp.y + 60.f});
    step(ui);
    UI_CHECK(small.item(t)->rect.x > 630.f && small.item(t)->rect.y > 230.f);
    // a view without a scene does nothing; the scene can be swapped
    w::setGraphicsScene(ui, v, nullptr);
    click(ui, tp);
    UI_CHECK(w::graphicsScene(ui, v) == nullptr && a11yItems(ui, v).empty());
    UI_CHECK(w::viewZoom(ui, ui.root()) == 1.f && w::graphicsScene(ui, ui.root()) == nullptr);
}

// ---- on-screen keyboard -------------------------------------------------------------------------------------------------------
void gateVirtualKeyboard() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle le = w::lineEdit(ui, ui.root(), "", "field");
    const NodeHandle other = w::lineEdit(ui, ui.root(), "", "other");
    const NodeHandle btn = w::button(ui, ui.root(), "plain", "plain");
    const NodeHandle kb = w::virtualKeyboard(ui, ui.root(), "vk");
    static int submitted, shiftOn, pages;
    submitted = shiftOn = pages = 0;
    ui.connect(le, SignalKind::Submitted, [](UiContext&, NodeHandle, const SignalArgs&) { ++submitted; });
    ui.connect(kb, SignalKind::Toggled, [](UiContext&, NodeHandle, const SignalArgs& a) { shiftOn = a.flag ? 1 : 0; });
    ui.connect(kb, SignalKind::ValueChanged, [](UiContext&, NodeHandle, const SignalArgs&) { ++pages; });
    step(ui);
    clickNode(ui, le);
    step(ui);
    w::virtualKeyboardAutoShow(ui, kb);
    step(ui);
    UI_CHECK(ui.rect(kb).h > 60.f && w::virtualKeyboardTarget(ui, kb) == le);
    auto press = [&](std::string_view l) {
        const NodeHandle k = w::virtualKeyboardKey(ui, kb, l);
        UI_CHECK(k.valid());
        if (k.valid()) clickNode(ui, k);
        w::virtualKeyboardAutoShow(ui, kb); // (an app calls this every frame)
        step(ui);
        UI_CHECK(ui.focused() == le);      // the keys never keep the focus
        UI_CHECK(ui.computed(kb).visible); // and typing does not hide the keyboard
    };
    press("Shift");
    UI_CHECK(shiftOn == 1 && ui.checked(w::virtualKeyboardKey(ui, kb, "Shift")));
    press("H"); // shift is one-shot
    UI_CHECK(shiftOn == 0);
    press("i");
    press("Space");
    press("?123");
    UI_CHECK(u32(ui.value(kb)) == 1 && pages == 1 && ui.text(w::virtualKeyboardKey(ui, kb, "?123")) == "ABC");
    press("4");
    press("2");
    press("?123");
    UI_CHECK(u32(ui.value(kb)) == 0);
    press("x");
    press("Backspace");
    UI_CHECK(ui.editBuffer(le).text() == "Hi 42");
    press("Shift");
    press("Shift"); // caps lock
    UI_CHECK(ui.hasClass(w::virtualKeyboardKey(ui, kb, "Shift"), "caps"));
    press("O");
    press("K");
    UI_CHECK(ui.editBuffer(le).text() == "Hi 42OK" && shiftOn == 1);
    press("Shift"); // caps off
    press("y");
    UI_CHECK(ui.editBuffer(le).text() == "Hi 42OKy" && shiftOn == 0);
    press("Enter");
    UI_CHECK(submitted == 1);
    // the keys are buttons that cannot take the focus; the special ones have accessible names
    const NodeHandle bs = w::virtualKeyboardKey(ui, kb, "Backspace");
    UI_CHECK(a11yOf(ui, bs).role == A11yRole::Button && a11yOf(ui, bs).name == "Backspace" && !a11yOf(ui, bs).focusable);
    UI_CHECK(a11yOf(ui, kb).role == A11yRole::Group && a11yOf(ui, kb).name == "Keyboard");
    UI_CHECK(!w::virtualKeyboardKey(ui, kb, "nope").valid());
    // typing follows the focus to another field
    ui.setFocus(other);
    step(ui);
    w::virtualKeyboardAutoShow(ui, kb);
    step(ui);
    clickNode(ui, w::virtualKeyboardKey(ui, kb, "z"));
    UI_CHECK(ui.editBuffer(other).text() == "z" && ui.focused() == other && ui.editBuffer(le).text() == "Hi 42OKy");
    // an explicit target
    w::setVirtualKeyboardTarget(ui, kb, le);
    ui.setFocus({});
    clickNode(ui, w::virtualKeyboardKey(ui, kb, "q"));
    UI_CHECK(ui.editBuffer(le).text() == "Hi 42OKyq" && ui.focused() == le);
    // focus on something that takes no text: the keyboard hides, and it shows again for a text field
    ui.setFocus(btn);
    step(ui);
    w::virtualKeyboardAutoShow(ui, kb);
    step(ui);
    UI_CHECK(!ui.computed(kb).visible && ui.rect(kb).h < 1.f);
    ui.setFocus({});
    w::virtualKeyboardAutoShow(ui, kb);
    step(ui);
    UI_CHECK(!ui.computed(kb).visible);
    clickNode(ui, le);
    step(ui);
    w::virtualKeyboardAutoShow(ui, kb);
    step(ui);
    UI_CHECK(ui.computed(kb).visible);
    // the target is gone: keys do nothing
    ui.destroy(le);
    ui.destroy(other);
    step(ui);
    w::setVirtualKeyboardTarget(ui, kb, {});
    ui.setCss(kb, "display", "flex");
    step(ui);
    clickNode(ui, w::virtualKeyboardKey(ui, kb, "a"));
    UI_CHECK(!w::virtualKeyboardTarget(ui, kb).valid());
}

// ---- registration: install(), installClasses, steady frames ----------------------------------------------------------
void gateClasses() {
    static constexpr std::string_view kNames[] = {"lcdnumber", "keysequenceedit", "mdiarea",  "mdisubwindow", "mdititle",
                                                  "graphicsview", "vkeyboard",    "completer", "completeritem", "editcombo", "combodrop"};
    {
        Fixture f;
        for (std::string_view n : kNames) UI_CHECK(f.ui.registry().find(n) != 0xFFFFFFFFu);
    }
    // a context that installs only what it names gets these classes (and their theme rules) by name
    UiContext ui({.viewport = {400, 300}, .font = &font()});
    static constexpr std::string_view kBase[] = {"label", "button", "toolbutton", "panel", "lineedit", "popup", "image"};
    UI_CHECK_EQ(w::installClasses(ui, kBase), 0u);
    UI_CHECK(ui.registry().find("lcdnumber") == 0xFFFFFFFFu);
    UI_CHECK_EQ(w::installClasses(ui, kNames), 0u);
    for (std::string_view n : kNames) UI_CHECK(ui.registry().find(n) != 0xFFFFFFFFu);
    const NodeHandle lcd = w::lcdNumber(ui, ui.root(), 4);
    ui.setValue(lcd, 12.f);
    const NodeHandle ks = w::keySequenceEdit(ui, ui.root());
    const NodeHandle area = w::mdiArea(ui, ui.root());
    ui.setCss(area, "height", "120px");
    w::mdiSubWindow(ui, area, "w", {4, 4, 120, 80});
    ui.update();
    UI_CHECK(ui.rect(lcd).h > 20.f && ui.rect(ks).w > 100.f && ui.computed(lcd).color.g > 0.9f); // the theme rule reached it
    UI_CHECK(a11yOf(ui, lcd).valueText == "12"); // the hooks came along with the class
}

void buildScene(UiContext& ui, w::GraphicsScene& scene) {
    ui.setCss(ui.root(), "flex-direction", "row");
    ui.setCss(ui.root(), "gap", "12px");
    const NodeHandle left = w::panel(ui, ui.root());
    ui.setCss(left, "width", "340px");
    ui.setCss(left, "gap", "8px");
    const NodeHandle lcd = w::lcdNumber(ui, left, 6);
    w::setLcdText(ui, lcd, "12:34.5");
    w::keySequenceEdit(ui, left, Shortcut{Key::S, kModCtrl | kModShift});
    const NodeHandle tb = w::toolBox(ui, left, "General\nAdvanced\nAbout");
    w::label(ui, w::toolBoxPage(ui, tb, 1), "the open page");
    w::setToolBoxIndex(ui, tb, 1);
    w::dialogButtonBox(ui, left, w::StdButton::Ok | w::StdButton::Cancel | w::StdButton::Help, {.layout = w::ButtonBoxLayout::Windows});
    const NodeHandle kb = w::virtualKeyboard(ui, left);
    (void)kb;
    const NodeHandle right = w::panel(ui, ui.root());
    ui.setCss(right, "flex-grow", "1");
    ui.setCss(right, "gap", "8px");
    const NodeHandle area = w::mdiArea(ui, right);
    ui.setCss(area, "height", "260px");
    const NodeHandle a = w::mdiSubWindow(ui, area, "Document 1", {16, 14, 240, 150});
    w::label(ui, w::mdiBody(ui, a), "an MDI sub-window");
    const NodeHandle b = w::mdiSubWindow(ui, area, "Document 2", {150, 70, 260, 160});
    w::label(ui, w::mdiBody(ui, b), "the active one");
    scene.add({.rect = {20, 20, 90, 60}, .text = "box"});
    scene.add({.shape = w::GraphicsShape::Ellipse, .rect = {150, 30, 70, 70}, .fill = Color::rgb(0x81C784)});
    scene.add({.shape = w::GraphicsShape::Line, .rect = {40, 130, 200, 40}, .strokeWidth = 2.f});
    scene.add({.shape = w::GraphicsShape::Polygon, .points = {{300, 30}, {380, 60}, {330, 140}}});
    scene.add({.shape = w::GraphicsShape::Text, .rect = {260, 150, 200, 24}, .fill = Color::rgb(0xFFFFFF), .text = "graphics view"});
    scene.select(0, true);
    const NodeHandle view = w::graphicsView(ui, right, &scene);
    ui.setCss(view, "flex-grow", "1");
}
int demo(const char* out) {
    Fixture f;
    w::GraphicsScene scene;
    buildScene(f.ui, scene);
    f.ui.update();
    Image img;
    render(f.ui, img);
    return stbi_write_png(out, img.width, img.height, 4, img.pixels.data(), img.width * 4) ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    struct Gate { const char* name; void (*fn)(); };
    static const Gate kGates[] = {{"buttonbox", &gateButtonBox},   {"dialog_keys", &gateDialogKeys}, {"input_dialog", &gateInputDialog},
                                  {"toolbox", &gateToolBox},       {"lcd", &gateLcd},                {"keysequence", &gateKeySequence},
                                  {"mdi", &gateMdi},               {"graphics", &gateGraphics},      {"vkeyboard", &gateVirtualKeyboard},
                                  {"classes", &gateClasses}};
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
    return finish("fuse_ui_widgets_extra");
}
