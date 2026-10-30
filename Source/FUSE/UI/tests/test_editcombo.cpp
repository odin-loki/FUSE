// fuse_ui_editcombo: QComboBox::setEditable parity — inline completion over the items, Up / Down stepping, the full
// list (Alt+Down / F4 / drop button), Enter selects or inserts per InsertPolicy, findText, signals, a11y.
#include "test_util.hpp"

#include "fuse/ui/widgets/widgets.hpp"

#include <cstdio>
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
    ui.keyDown(Key::Enter); // Qt MatchFixedString: "blue" selects "Blue" and the field shows the item's own case
    UI_CHECK(items(ui, combo) == "Red\nGreen\nBlue" && w::comboCurrentIndex(ui, combo) == 2 && idxSig == 2);
    UI_CHECK(w::lineEditText(ui, edit) == "Blue");
    w::setComboOptions(ui, combo, {.caseSensitive = true});
    w::setLineEditText(ui, edit, "");
    ui.textInput("bl");
    ui.keyDown(Key::Enter); // case-sensitive: not an item, inserted at the bottom
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
    const NodeHandle drop = child(ui, combo, 2); // [icon] edit, button
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
    // ---- round 3: remaining insert policies, maxCount, duplicates, validator, model ----
    {
        auto mk = [&](w::InsertPolicy p, i32 sel) {
            const NodeHandle c = w::editableComboBox(ui, col, "A\nB\nC", sel, p);
            ui.update();
            return c;
        };
        auto enter = [&](NodeHandle c, std::string_view t) {
            const NodeHandle e = w::comboLineEdit(ui, c);
            ui.setFocus(e);
            w::setLineEditText(ui, e, "");
            ui.textInput(t);
            ui.keyDown(Key::Enter);
        };
        NodeHandle c = mk(w::InsertPolicy::InsertAfterCurrent, 0);
        enter(c, "x");
        UI_CHECK(items(ui, c) == "A\nx\nB\nC" && w::comboCurrentIndex(ui, c) == 1);
        c = mk(w::InsertPolicy::InsertBeforeCurrent, 2);
        enter(c, "x");
        UI_CHECK(items(ui, c) == "A\nB\nx\nC" && w::comboCurrentIndex(ui, c) == 2);
        c = mk(w::InsertPolicy::InsertAtCurrent, 1);
        enter(c, "x");
        UI_CHECK(items(ui, c) == "A\nx\nC" && w::comboCurrentIndex(ui, c) == 1);
        // maxCount: full -> nothing inserted, index unchanged; AtCurrent still replaces
        c = mk(w::InsertPolicy::InsertAtBottom, 0);
        w::setComboOptions(ui, c, {.policy = w::InsertPolicy::InsertAtBottom, .maxCount = 3});
        enter(c, "x");
        UI_CHECK(items(ui, c) == "A\nB\nC" && w::comboCurrentIndex(ui, c) == 0);
        w::setComboOptions(ui, c, {.policy = w::InsertPolicy::InsertAtCurrent, .maxCount = 3});
        enter(c, "x");
        UI_CHECK(items(ui, c) == "x\nB\nC");
        // duplicates
        c = mk(w::InsertPolicy::InsertAtBottom, 0);
        enter(c, "B");
        UI_CHECK(items(ui, c) == "A\nB\nC" && w::comboCurrentIndex(ui, c) == 1);
        w::setComboOptions(ui, c, {.duplicatesEnabled = true});
        enter(c, "B");
        UI_CHECK(items(ui, c) == "A\nB\nC\nB" && w::comboCurrentIndex(ui, c) == 3 && w::comboCount(ui, c) == 4);
        // validator: digits only; Intermediate / Invalid text never inserts
        c = mk(w::InsertPolicy::InsertAtBottom, 0);
        w::setComboValidator(ui, c, [](std::string_view t) {
            if (t.empty()) return w::Validation::Intermediate;
            for (char ch : t) if (ch < '0' || ch > '9') return w::Validation::Invalid;
            return w::Validation::Acceptable;
        });
        ui.setFocus(w::comboLineEdit(ui, c));
        w::setLineEditText(ui, w::comboLineEdit(ui, c), "");
        ui.textInput("4");
        ui.textInput("z");
        ui.textInput("2");
        UI_CHECK(w::lineEditText(ui, w::comboLineEdit(ui, c)) == "42");
        ui.keyDown(Key::Enter);
        UI_CHECK(items(ui, c) == "A\nB\nC\n42");
        // model-backed: items mirror the column, Enter selects but never inserts
        w::StringTableModel tm("id\tcity");
        tm.addRow("1\tOslo");
        tm.addRow("2\tParis");
        c = mk(w::InsertPolicy::InsertAtBottom, 0);
        w::setComboModel(ui, c, &tm.model(), 1);
        UI_CHECK(w::comboCount(ui, c) == 2 && w::comboItemText(ui, c, 1) == "Paris");
        enter(c, "Rome");
        UI_CHECK(w::comboCount(ui, c) == 2);
        tm.addRow("3\tRome");
        w::comboModelChanged(ui, c);
        UI_CHECK(w::comboCount(ui, c) == 3);
        enter(c, "ro"); // inline (typed case kept), Enter matches "Rome" ignoring case completion now sees the new row
        UI_CHECK(w::comboCurrentIndex(ui, c) == 2 && w::lineEditText(ui, w::comboLineEdit(ui, c)) == "Rome");
        // assistive-tech Expand / Collapse (AccessKit, TalkBack ACTION_EXPAND / COLLAPSE)
        ui.setFocus(other);
        UI_CHECK(ui.a11yState(c).expanded == 0 && ui.a11yExpand(c, true));
        UI_CHECK(w::completerPopupVisible(ui, w::comboLineEdit(ui, c)) && ui.a11yState(c).expanded == 1);
        UI_CHECK(!ui.a11yExpand(c, true) && ui.a11yExpand(c, false));
        UI_CHECK(!w::completerPopupVisible(ui, w::comboLineEdit(ui, c)) && ui.a11yState(c).expanded == 0);
        UI_CHECK(!ui.a11yExpand(other, true)); // not expandable
    }
    // ---- round 4: item icons / user data, insert / remove ----
    {
        const NodeHandle c = w::editableComboBox(ui, col, "", -1);
        ui.update();
        UI_CHECK(w::comboCurrentIndex(ui, c) == -1);
        w::comboAddItem(ui, c, "Apple", 3, 100);
        UI_CHECK(w::comboCurrentIndex(ui, c) == 0 && w::lineEditText(ui, w::comboLineEdit(ui, c)) == "Apple");
        w::comboAddItem(ui, c, "Cherry", w::kNoIcon, 300);
        w::comboInsertItem(ui, c, 1, "Banana", 5, 200);
        UI_CHECK(items(ui, c) == "Apple\nBanana\nCherry" && w::comboItemIcon(ui, c, 1) == 5 && w::comboItemData(ui, c, 2) == 300);
        UI_CHECK(w::comboFindData(ui, c, 200) == 1 && w::comboFindData(ui, c, 7) == -1);
        // the field's icon shows the current item's icon
        ui.update();
        const NodeHandle ic = ui.firstChild(c);
        UI_CHECK(ui.userData(ic) == 3 && ui.rect(ic).w == 16.f);
        w::setComboCurrentIndex(ui, c, 2);
        ui.update();
        UI_CHECK(ui.rect(ic).w == 0.f); // Cherry: no icon
        // inserting before the current item keeps it current; meta follows the items
        w::comboInsertItem(ui, c, 0, "Apricot", 9, 50);
        UI_CHECK(w::comboCurrentIndex(ui, c) == 3 && w::comboItemData(ui, c, 3) == 300 && w::comboItemIcon(ui, c, 1) == 3);
        // policy insertion shifts meta too
        w::setComboOptions(ui, c, {.policy = w::InsertPolicy::InsertAtTop});
        const NodeHandle e = w::comboLineEdit(ui, c);
        ui.setFocus(e);
        w::setLineEditText(ui, e, "");
        ui.textInput("Date");
        ui.keyDown(Key::Enter);
        UI_CHECK(items(ui, c) == "Date\nApricot\nApple\nBanana\nCherry" && w::comboItemData(ui, c, 0) == 0);
        UI_CHECK(w::comboItemIcon(ui, c, 0) == w::kNoIcon && w::comboItemData(ui, c, 4) == 300 && w::comboFindData(ui, c, 100) == 2);
        // the list rows carry the icons
        w::comboShowList(ui, c);
        ui.update();
        const NodeHandle pop = w::completerPopup(ui, e);
        UI_CHECK(ui.userData(child(ui, pop, 1)) == 10 && ui.userData(child(ui, pop, 0)) == 0); // image index + 1
        ui.keyDown(Key::Escape);
        // remove: the current item removed -> the next one becomes current
        w::setComboCurrentIndex(ui, c, 3);
        w::comboRemoveItem(ui, c, 3);
        UI_CHECK(items(ui, c) == "Date\nApricot\nApple\nCherry" && w::comboCurrentIndex(ui, c) == 3);
        UI_CHECK(w::lineEditText(ui, e) == "Cherry" && w::comboItemData(ui, c, 3) == 300);
        w::comboRemoveItem(ui, c, 0);
        UI_CHECK(w::comboCurrentIndex(ui, c) == 2 && w::comboItemIcon(ui, c, 0) == 9);
    }
    return fuse::ui::test::finish("fuse_ui_editcombo");
}
