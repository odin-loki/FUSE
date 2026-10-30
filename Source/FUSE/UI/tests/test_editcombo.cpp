// fuse_ui_editcombo: QComboBox::setEditable parity — inline completion over the items, Up / Down stepping, the full
// list (Alt+Down / F4 / drop button), Enter selects or inserts per InsertPolicy, findText, signals, a11y.
#include "test_util.hpp"

#include "fuse/ui/widgets/widgets.hpp"

#include <string>
#include <vector>

using namespace fuse::ui;
namespace w = fuse::ui::widgets;

namespace {
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
NodeHandle child(const UiContext& ui, NodeHandle n, u32 i) {
    NodeHandle k = ui.firstChild(n);
    while (i-- && ui.alive(k)) k = ui.nextSibling(k);
    return k;
}
std::string items(const UiContext& ui, NodeHandle c) { return std::string(ui.text(c)); }
} // namespace

int main() {
    UiContext ui({.viewport = {400, 400}, .font = &fuse::ui::test::font()});
    w::install(ui);
    const NodeHandle col = w::panel(ui, ui.root());
    const NodeHandle combo = w::editableComboBox(ui, col, "Red\nGreen\nBlue", 1, w::InsertPolicy::InsertAtBottom, "colour");
    const NodeHandle other = w::lineEdit(ui, col, "", "other");
    ui.update();
    const NodeHandle edit = w::comboLineEdit(ui, combo);
    UI_CHECK(ui.alive(edit) && w::comboCurrentIndex(ui, combo) == 1 && w::lineEditText(ui, edit) == "Green");
    UI_CHECK(w::comboFindText(ui, combo, "Blue") == 2 && w::comboFindText(ui, combo, "blue") == -1);
    UI_CHECK(w::comboFindText(ui, combo, "blue", false) == 2);
    UI_CHECK(ui.rect(edit).w > 0.f && ui.rect(edit).right() <= ui.rect(combo).right());

    i32 idxSig = -9, idxCount = 0;
    std::string textSig;
    ui.connect(combo, SignalKind::ValueChanged, [&](UiContext&, NodeHandle, const SignalArgs& a) { idxSig = i32(a.value); ++idxCount; });
    ui.connect(combo, SignalKind::TextChanged, [&](UiContext&, NodeHandle, const SignalArgs& a) { textSig.assign(a.text); });

    // ---- Up / Down step the current item ----
    ui.setFocus(edit);
    ui.keyDown(Key::Down);
    UI_CHECK(w::comboCurrentIndex(ui, combo) == 2 && w::lineEditText(ui, edit) == "Blue" && idxSig == 2 && textSig == "Blue");
    ui.keyDown(Key::Down); // clamps
    UI_CHECK(w::comboCurrentIndex(ui, combo) == 2 && idxCount == 1);
    ui.keyDown(Key::PageUp);
    UI_CHECK(w::comboCurrentIndex(ui, combo) == 0 && w::lineEditText(ui, edit) == "Red");
    ui.keyDown(Key::Up);
    UI_CHECK(w::comboCurrentIndex(ui, combo) == 0);
    ui.wheel(ctr(ui.rect(combo)), {0.f, -1.f});
    UI_CHECK(w::comboCurrentIndex(ui, combo) == 1);

    // ---- inline completion (case-insensitive), Enter selects the match ----
    w::setLineEditText(ui, edit, "");
    ui.textInput("bl");
    UI_CHECK(w::lineEditText(ui, edit) == "blue" && ui.editBuffer(edit).selection() == "ue");
    UI_CHECK(!w::completerPopupVisible(ui, edit));
    ui.keyDown(Key::Enter); // "blue" is not an exact (case-sensitive) item: inserted at the bottom
    UI_CHECK(items(ui, combo) == "Red\nGreen\nBlue\nblue" && w::comboCurrentIndex(ui, combo) == 3 && idxSig == 3);
    w::setLineEditText(ui, edit, "");
    ui.textInput("Gr");
    ui.keyDown(Key::Enter);
    UI_CHECK(w::lineEditText(ui, edit) == "Green" && w::comboCurrentIndex(ui, combo) == 1 && items(ui, combo) == "Red\nGreen\nBlue\nblue");

    // ---- the full list: Alt+Down, navigate, Enter picks; F4 toggles; Escape closes ----
    ui.keyDown(Key::Down, kModAlt);
    UI_CHECK(w::completerPopupVisible(ui, edit) && w::completionCount(ui, edit) == 4);
    UI_CHECK(w::completerCurrentRow(ui, edit) == 1); // the current item is highlighted
    UI_CHECK(ui.a11yState(combo).expanded == 1 && ui.a11yState(combo).controls == w::completerPopup(ui, edit));
    ui.keyDown(Key::Down);
    UI_CHECK(w::completerCurrentRow(ui, edit) == 2 && w::lineEditText(ui, edit) == "Green"); // field unchanged until chosen
    ui.keyDown(Key::Enter);
    UI_CHECK(!w::completerPopupVisible(ui, edit) && w::comboCurrentIndex(ui, combo) == 2 && w::lineEditText(ui, edit) == "Blue");
    UI_CHECK(ui.a11yState(combo).expanded == 0);
    ui.keyDown(Key::F4);
    UI_CHECK(w::completerPopupVisible(ui, edit));
    ui.keyDown(Key::F4);
    UI_CHECK(!w::completerPopupVisible(ui, edit));
    w::comboShowList(ui, combo);
    ui.keyDown(Key::Escape);
    UI_CHECK(!w::completerPopupVisible(ui, edit) && w::lineEditText(ui, edit) == "Blue");

    // ---- drop button: opens the list under the whole combo; click an option ----
    ui.update();
    const NodeHandle drop = child(ui, combo, 1);
    ui.pointerDown(ctr(ui.rect(drop)), MouseButton::Left);
    ui.pointerUp(ctr(ui.rect(drop)), MouseButton::Left);
    UI_CHECK(w::completerPopupVisible(ui, edit));
    ui.update();
    const NodeHandle pop = w::completerPopup(ui, edit);
    UI_CHECK(ui.rect(pop).x == ui.rect(combo).x && ui.rect(pop).w >= ui.rect(combo).w);
    const NodeHandle red = child(ui, pop, 0);
    UI_CHECK(ui.text(red) == "Red");
    ui.pointerDown(ctr(ui.rect(red)), MouseButton::Left);
    ui.pointerUp(ctr(ui.rect(red)), MouseButton::Left);
    UI_CHECK(w::comboCurrentIndex(ui, combo) == 0 && w::lineEditText(ui, edit) == "Red" && ui.focused() == edit);
    // typing closes the list
    w::comboShowList(ui, combo);
    ui.textInput("x");
    UI_CHECK(!w::completerPopupVisible(ui, edit));

    // ---- insert policies ----
    const NodeHandle top = w::editableComboBox(ui, col, "b\nd", 0, w::InsertPolicy::InsertAtTop);
    const NodeHandle alpha = w::editableComboBox(ui, col, "b\nd", 0, w::InsertPolicy::InsertAlphabetically);
    const NodeHandle none = w::editableComboBox(ui, col, "b\nd", 0, w::InsertPolicy::NoInsert);
    for (NodeHandle c : {top, alpha, none}) {
        ui.setFocus(w::comboLineEdit(ui, c));
        w::setLineEditText(ui, w::comboLineEdit(ui, c), "c");
        ui.keyDown(Key::Enter);
    }
    UI_CHECK(items(ui, top) == "c\nb\nd" && w::comboCurrentIndex(ui, top) == 0);
    UI_CHECK(items(ui, alpha) == "b\nc\nd" && w::comboCurrentIndex(ui, alpha) == 1);
    UI_CHECK(items(ui, none) == "b\nd" && w::comboCurrentIndex(ui, none) == 0 && w::lineEditText(ui, w::comboLineEdit(ui, none)) == "c");
    // the new item completes
    ui.setFocus(w::comboLineEdit(ui, top));
    w::setLineEditText(ui, w::comboLineEdit(ui, top), "");
    ui.textInput("c");
    UI_CHECK(w::lineEditText(ui, w::comboLineEdit(ui, top)) == "c" && w::completionCount(ui, w::comboLineEdit(ui, top)) == 1);

    // ---- programmatic index, empty combo ----
    w::setComboCurrentIndex(ui, combo, 1);
    UI_CHECK(w::lineEditText(ui, edit) == "Green" && idxSig == 1);
    w::setComboCurrentIndex(ui, combo, -1);
    UI_CHECK(w::comboCurrentIndex(ui, combo) == -1 && w::lineEditText(ui, edit).empty());
    const NodeHandle empty = w::editableComboBox(ui, col, "", 0);
    UI_CHECK(w::comboCurrentIndex(ui, empty) == -1);
    ui.setFocus(w::comboLineEdit(ui, empty));
    ui.keyDown(Key::Down);
    ui.textInput("new");
    ui.keyDown(Key::Enter);
    UI_CHECK(items(ui, empty) == "new" && w::comboCurrentIndex(ui, empty) == 0);

    // ---- a11y: ComboBox holding the TextField and the drop Button ----
    ui.update();
    std::vector<A11ySnapshotNode> snap;
    ui.snapshotA11y(snap);
    bool box = false, field = false, button = false;
    for (u32 i = 0; i < snap.size(); ++i) {
        if (snap[i].node == combo) box = snap[i].role == A11yRole::ComboBox && snap[i].state.expanded == 0;
        if (snap[i].node == edit) field = snap[i].role == A11yRole::TextField && snap[snap[i].parent].node == combo &&
                                          snap[i].state.autoComplete == A11yAutoComplete::Inline;
        if (snap[i].node == drop) button = snap[i].role == A11yRole::Button && snap[i].name == "Open list";
    }
    UI_CHECK(box && field && button);
    ui.setFocus(other);
    ui.update();
    return fuse::ui::test::finish("fuse_ui_editcombo");
}
