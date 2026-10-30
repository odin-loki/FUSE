// fuse_ui_buttonbox: QDialogButtonBox parity — standard buttons and roles, Windows / macOS / KDE / GNOME orders,
// custom buttons, centre / vertical, default button, clicked / accepted / rejected signals.
#include "test_util.hpp"

#include "fuse/ui/extras/widgets_extra.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace fuse::ui;
namespace w = fuse::ui::widgets;

int main() {
    UiContext ui({.viewport = {640, 300}, .font = &fuse::ui::test::font()});
    w::install(ui);
    const NodeHandle col = w::panel(ui, ui.root());
    using B = u32;
    const B mask = w::StdButton::Ok | w::StdButton::Cancel | w::StdButton::Apply | w::StdButton::Help | w::StdButton::Reset;
    const NodeHandle box = w::dialogButtonBox(ui, col, mask, {.layout = w::ButtonBoxLayout::Windows}, "bb");
    ui.update();
    auto texts = [&] {
        std::string s;
        for (NodeHandle b : w::dialogButtonBoxButtons(ui, box)) { if (!s.empty()) s += ','; s += std::string(ui.text(b)); }
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
    w::setDialogButtonBoxStandardButtons(ui, box, w::StdButton::Save | w::StdButton::Discard | w::StdButton::Cancel);
    UI_CHECK_EQ(texts(), std::string("Close without Saving,Cancel,Save"));
    w::setDialogButtonBoxOptions(ui, box, {.layout = w::ButtonBoxLayout::Windows});
    UI_CHECK_EQ(texts(), std::string("Save,Discard,Cancel"));
    const NodeHandle more = w::dialogButtonBoxAdd(ui, box, "Details", w::ButtonRole::Action);
    UI_CHECK_EQ(texts(), std::string("Save,Discard,Details,Cancel"));
    UI_CHECK(w::dialogButtonBoxRole(ui, box, more) == w::ButtonRole::Action && w::dialogButtonBoxStandardOf(ui, box, more) == 0);
    const NodeHandle save = w::dialogButtonBoxButton(ui, box, w::StdButton::Save);
    UI_CHECK(ui.alive(save) && w::dialogButtonBoxStandardOf(ui, box, save) == w::StdButton::Save);
    UI_CHECK(w::dialogButtonBoxDefault(ui, box) == save && ui.hasClass(save, "default"));
    UI_CHECK(!ui.alive(w::dialogButtonBoxButton(ui, box, w::StdButton::Ok)));

    // signals: clicked {role}, then accepted / rejected
    int accepted = 0, rejected = 0, clicks = 0;
    f32 lastRole = -1.f;
    ui.connect(box, SignalKind::Clicked, [&](UiContext&, NodeHandle, const SignalArgs& a) { ++clicks; lastRole = a.value; });
    ui.connect(box, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) { (a.value > 0.5f ? accepted : rejected)++; });
    ui.update();
    auto click = [&](NodeHandle b) {
        const Rect r = ui.rect(b);
        const Vec2 p{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
        ui.pointerMove(p);
        ui.pointerDown(p, MouseButton::Left);
        ui.pointerUp(p, MouseButton::Left);
    };
    click(save);
    UI_CHECK(clicks == 1 && accepted == 1 && rejected == 0 && lastRole == f32(u8(w::ButtonRole::Accept)));
    click(w::dialogButtonBoxButton(ui, box, w::StdButton::Cancel));
    UI_CHECK(clicks == 2 && rejected == 1);
    click(more);
    UI_CHECK(clicks == 3 && accepted == 1 && rejected == 1 && lastRole == f32(u8(w::ButtonRole::Action)));
    w::dialogButtonBoxRemove(ui, box, more);
    UI_CHECK_EQ(texts(), std::string("Save,Discard,Cancel"));

    // layout: buttons pushed right by the stretch; centred; vertical stacks
    ui.update();
    const Rect br = ui.rect(box);
    const Rect last = ui.rect(w::dialogButtonBoxButton(ui, box, w::StdButton::Cancel));
    UI_CHECK(last.right() > br.right() - 20.f);
    w::setDialogButtonBoxOptions(ui, box, {.layout = w::ButtonBoxLayout::Windows, .centerButtons = true});
    ui.update();
    const Rect s0 = ui.rect(save), c0 = ui.rect(w::dialogButtonBoxButton(ui, box, w::StdButton::Cancel));
    UI_CHECK(std::abs((s0.x - br.x) - (br.right() - c0.right())) < 4.f);
    w::setDialogButtonBoxOptions(ui, box, {.layout = w::ButtonBoxLayout::Windows, .vertical = true});
    ui.update();
    const Rect s1 = ui.rect(save), d1 = ui.rect(w::dialogButtonBoxButton(ui, box, w::StdButton::Discard));
    UI_CHECK(d1.y > s1.y && std::abs(d1.x - s1.x) < 1.f);

    // QDialog keys: Enter = default button, Escape = reject button, a focused push button presses itself
    {
        f32 res = -1.f; std::string txt;
        auto watch = [&](NodeHandle d) { ui.connect(d, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) { res = a.value; txt = a.text; }); };
        NodeHandle mb = w::messageBox(ui, "Q", "Save?", "Save\nDiscard\nCancel");
        watch(mb); ui.update();
        ui.keyDown(Key::Escape, 0); ui.update();
        UI_CHECK(res == 2.f && txt == "Cancel");
        mb = w::messageBox(ui, "Q", "Save?", "Save\nDiscard\nCancel");
        watch(mb); ui.update(); res = -1.f;
        ui.keyDown(Key::Enter, 0); ui.update();
        UI_CHECK(res == 0.f && txt == "Save");
        // focused Discard: Enter presses it, not the default
        mb = w::messageBox(ui, "Q", "Save?", "Save\nDiscard\nCancel");
        watch(mb); ui.update(); res = -1.f;
        for (NodeHandle b : w::dialogButtonBoxButtons(ui, ui.nextSibling(ui.firstChild(w::dialogBody(ui, mb)))))
            if (ui.text(b) == "Discard") ui.setFocus(b, FocusReason::Keyboard);
        ui.keyDown(Key::Enter, 0); ui.update();
        UI_CHECK(res == 1.f && txt == "Discard");
        // custom-only buttons: Escape = last
        mb = w::messageBox(ui, "Q", "Pick", "Alpha\nBeta");
        watch(mb); ui.update(); res = -1.f;
        ui.keyDown(Key::Escape, 0); ui.update();
        UI_CHECK(res == 1.f);
        // input dialog: Enter in the line edit accepts with its text, Escape rejects
        NodeHandle in = w::inputDialog(ui, "Name", "Name:", "bob");
        watch(in); ui.update(); res = -1.f;
        ui.keyDown(Key::Enter, 0); ui.update();
        UI_CHECK(res == 1.f && txt == "bob");
        in = w::inputDialog(ui, "Name", "Name:", "bob");
        watch(in); ui.update(); res = -1.f;
        ui.keyDown(Key::Escape, 0); ui.update();
        UI_CHECK(res == 0.f);
        // wizard: Enter = Next until the last page, then Finish; Escape = Cancel
        NodeHandle wz = w::wizard(ui, "W", "One\nTwo");
        watch(wz); ui.update(); res = -1.f;
        ui.keyDown(Key::Enter, 0); ui.update();
        UI_CHECK(res == -1.f && w::wizardCurrent(ui, wz) == 1);
        ui.keyDown(Key::Enter, 0); ui.update();
        UI_CHECK(res == 1.f);
        wz = w::wizard(ui, "W", "One\nTwo");
        watch(wz); ui.update(); res = -1.f;
        ui.keyDown(Key::Escape, 0); ui.update();
        UI_CHECK(res == 0.f);
        // plain dialog with explicit default / reject
        const NodeHandle dl = w::dialog(ui, "D");
        const NodeHandle ok = w::button(ui, w::dialogBody(ui, dl), "Go"), no = w::button(ui, w::dialogBody(ui, dl), "Stop");
        int go = 0, stop = 0;
        ui.connect(ok, SignalKind::Clicked, [&](UiContext&, NodeHandle, const SignalArgs&) { ++go; });
        ui.connect(no, SignalKind::Clicked, [&](UiContext&, NodeHandle, const SignalArgs&) { ++stop; });
        w::setDialogDefaultButton(ui, dl, ok); w::setDialogRejectButton(ui, dl, no);
        w::openDialog(ui, dl); ui.update();
        ui.keyDown(Key::Enter, 0); ui.keyDown(Key::Escape, 0); ui.update();
        UI_CHECK(go == 1 && stop == 1);
        ui.setDisabled(no, true);
        ui.keyDown(Key::Escape, 0); ui.update(); // no enabled reject: closes
        UI_CHECK(stop == 1 && go == 1);
    }
    return fuse::ui::test::finish("fuse_ui_buttonbox");
}
