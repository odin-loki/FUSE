// fuse_ui_completer: QCompleter parity for line edits — popup / inline / unfiltered-popup modes, case and filter
// modes, model-backed completion, keyboard navigation (focus stays in the field), mouse pick, wheel, a11y state.
#include "test_util.hpp"

#include "fuse/ui/widgets/widgets.hpp"

#include <string>
#include <vector>

using namespace fuse::ui;
namespace w = fuse::ui::widgets;

namespace {
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
u32 children(const UiContext& ui, NodeHandle n) {
    u32 c = 0;
    for (NodeHandle k = ui.firstChild(n); ui.alive(k); k = ui.nextSibling(k)) ++c;
    return c;
}
NodeHandle child(const UiContext& ui, NodeHandle n, u32 i) {
    NodeHandle k = ui.firstChild(n);
    while (i-- && ui.alive(k)) k = ui.nextSibling(k);
    return k;
}
void clear(UiContext& ui, NodeHandle e) { w::setLineEditText(ui, e, ""); }
} // namespace

int main() {
    UiContext ui({.viewport = {400, 400}, .font = &fuse::ui::test::font()});
    w::install(ui);
    const NodeHandle col = w::panel(ui, ui.root());
    const NodeHandle fruit = w::lineEdit(ui, col, "", "fruit");
    const NodeHandle other = w::lineEdit(ui, col, "", "other");
    ui.update();
    const std::string kItems = "Apple\nApricot\nBanana\napple pie\nAvocado";

    std::string hiText, actText, subText;
    i32 hiRow = -2, actRow = -2, hiCount = 0, actCount = 0;
    ui.connect(fruit, SignalKind::Highlighted, [&](UiContext&, NodeHandle, const SignalArgs& a) { hiText.assign(a.text); hiRow = i32(a.value); ++hiCount; });
    ui.connect(fruit, SignalKind::Activated, [&](UiContext&, NodeHandle, const SignalArgs& a) { actText.assign(a.text); actRow = i32(a.value); ++actCount; });
    ui.connect(fruit, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) { subText.assign(a.text); });

    // ---- popup mode, case-insensitive starts-with ----
    w::setCompleter(ui, fruit, {.items = kItems});
    UI_CHECK(ui.a11yState(fruit).autoComplete == A11yAutoComplete::List && ui.a11yState(fruit).expanded == 0);
    ui.setFocus(fruit);
    ui.textInput("ap");
    ui.update();
    UI_CHECK(w::completerPopupVisible(ui, fruit));
    UI_CHECK(w::completionCount(ui, fruit) == 3);
    UI_CHECK(w::completionAt(ui, fruit, 0) == "Apple" && w::completionAt(ui, fruit, 2) == "apple pie");
    UI_CHECK(w::completerCurrentRow(ui, fruit) == -1);
    UI_CHECK(ui.focused() == fruit); // focus stays in the field
    const NodeHandle pop = w::completerPopup(ui, fruit);
    UI_CHECK(ui.alive(pop) && children(ui, pop) == 3);
    UI_CHECK(ui.rect(pop).y >= ui.rect(fruit).bottom() && ui.rect(pop).w >= ui.rect(fruit).w);
    UI_CHECK(ui.a11yState(fruit).expanded == 1 && ui.a11yState(fruit).controls == pop);
    // a11y snapshot: List "Completions" with three ListItem options
    {
        std::vector<A11ySnapshotNode> snap;
        ui.snapshotA11y(snap);
        u32 items = 0;
        bool list = false;
        for (const auto& s : snap) {
            if (s.node == pop) list = s.role == A11yRole::List && s.name == "Completions";
            if (s.role == A11yRole::ListItem && ui.parent(s.node) == pop) ++items;
            if (s.node == fruit) UI_CHECK(s.state.expanded == 1 && s.state.controls == pop && !s.state.activeDescendant.valid());
        }
        UI_CHECK(list && items == 3);
    }
    // typing keeps filtering
    ui.textInput("r");
    UI_CHECK(w::completionCount(ui, fruit) == 1 && w::completionAt(ui, fruit, 0) == "Apricot");
    ui.keyDown(Key::Backspace);
    UI_CHECK(w::completionCount(ui, fruit) == 3 && w::lineEditText(ui, fruit) == "ap");

    // ---- keyboard navigation: Down / Up put the completion in the field, past either end returns to the prefix ----
    ui.keyDown(Key::Down);
    UI_CHECK(w::completerCurrentRow(ui, fruit) == 0 && w::lineEditText(ui, fruit) == "Apple");
    UI_CHECK(hiText == "Apple" && hiRow == 0);
    UI_CHECK(w::completionCount(ui, fruit) == 3); // highlighting does not refilter
    ui.keyDown(Key::Down);
    ui.keyDown(Key::Down);
    UI_CHECK(w::lineEditText(ui, fruit) == "apple pie" && hiRow == 3); // source model row
    {
        const A11yState a = ui.a11yState(fruit);
        UI_CHECK(a.activeDescendant.valid() && ui.parent(a.activeDescendant) == pop);
        UI_CHECK(ui.text(a.activeDescendant) == "apple pie" && ui.a11yState(a.activeDescendant).selected);
        UI_CHECK(!ui.a11yState(child(ui, pop, 0)).selected && ui.checked(a.activeDescendant));
    }
    ui.keyDown(Key::Down); // wrap: back to the typed prefix
    UI_CHECK(w::completerCurrentRow(ui, fruit) == -1 && w::lineEditText(ui, fruit) == "ap");
    UI_CHECK(!ui.a11yState(fruit).activeDescendant.valid());
    ui.keyDown(Key::Up); // from none: the last row
    UI_CHECK(w::completerCurrentRow(ui, fruit) == 2);
    ui.keyDown(Key::Home, kModCtrl);
    UI_CHECK(w::completerCurrentRow(ui, fruit) == 0);
    ui.keyDown(Key::End, kModCtrl);
    UI_CHECK(w::completerCurrentRow(ui, fruit) == 2);
    ui.keyDown(Key::PageUp);
    UI_CHECK(w::completerCurrentRow(ui, fruit) == 0);
    ui.keyDown(Key::PageDown);
    UI_CHECK(w::completerCurrentRow(ui, fruit) == 2);
    ui.keyDown(Key::Up);
    UI_CHECK(w::lineEditText(ui, fruit) == "Apricot");

    // ---- Enter chooses (Activated), closes, and still submits (returnPressed) ----
    ui.keyDown(Key::Enter);
    UI_CHECK(actText == "Apricot" && actRow == 1 && actCount == 1);
    UI_CHECK(!w::completerPopupVisible(ui, fruit) && subText == "Apricot");
    UI_CHECK(ui.a11yState(fruit).expanded == 0 && !ui.a11yState(fruit).controls.valid());
    ui.update();
    UI_CHECK(!ui.computed(pop).visible);

    // ---- Escape closes and restores the prefix ----
    clear(ui, fruit);
    UI_CHECK(!w::completerPopupVisible(ui, fruit)); // programmatic text does not open the popup
    ui.textInput("av");
    UI_CHECK(w::completerPopupVisible(ui, fruit) && w::completionCount(ui, fruit) == 1);
    ui.keyDown(Key::Down);
    UI_CHECK(w::lineEditText(ui, fruit) == "Avocado");
    ui.keyDown(Key::Escape);
    UI_CHECK(!w::completerPopupVisible(ui, fruit) && w::lineEditText(ui, fruit) == "av");
    UI_CHECK(ui.focused() == fruit);
    // Down on a closed popup reopens it; with an empty field, every row (QCompleter::complete)
    ui.keyDown(Key::Down);
    UI_CHECK(w::completerPopupVisible(ui, fruit) && w::completionCount(ui, fruit) == 1);
    ui.keyDown(Key::Escape);
    clear(ui, fruit);
    ui.keyDown(Key::Down);
    UI_CHECK(w::completerPopupVisible(ui, fruit) && w::completionCount(ui, fruit) == 5);
    ui.keyDown(Key::Escape);

    // ---- no matches: no popup; erasing to empty closes it ----
    ui.textInput("zz");
    UI_CHECK(!w::completerPopupVisible(ui, fruit) && w::completionCount(ui, fruit) == 0);
    clear(ui, fruit);
    ui.textInput("b");
    UI_CHECK(w::completerPopupVisible(ui, fruit));
    ui.keyDown(Key::Backspace);
    UI_CHECK(!w::completerPopupVisible(ui, fruit));

    // ---- mouse: clicking an option chooses it, focus stays in the field ----
    ui.textInput("a");
    ui.update();
    UI_CHECK(w::completionCount(ui, fruit) == 4);
    const NodeHandle third = child(ui, pop, 2);
    UI_CHECK(ui.text(third) == "apple pie");
    ui.pointerDown(ctr(ui.rect(third)), MouseButton::Left);
    ui.pointerUp(ctr(ui.rect(third)), MouseButton::Left);
    UI_CHECK(actText == "apple pie" && w::lineEditText(ui, fruit) == "apple pie" && !w::completerPopupVisible(ui, fruit));
    UI_CHECK(ui.focused() == fruit);

    // ---- focus loss closes the popup ----
    clear(ui, fruit);
    ui.textInput("a");
    UI_CHECK(w::completerPopupVisible(ui, fruit));
    ui.setFocus(other);
    UI_CHECK(!w::completerPopupVisible(ui, fruit));
    ui.setFocus(fruit);

    // ---- case-sensitive, contains, ends-with ----
    w::setCompleter(ui, fruit, {.items = kItems, .caseSensitive = true});
    clear(ui, fruit);
    ui.textInput("a");
    UI_CHECK(w::completionCount(ui, fruit) == 1 && w::completionAt(ui, fruit, 0) == "apple pie");
    w::setCompleter(ui, fruit, {.items = kItems, .filter = w::MatchMode::Contains});
    clear(ui, fruit);
    ui.textInput("AN");
    UI_CHECK(w::completionCount(ui, fruit) == 1 && w::completionAt(ui, fruit, 0) == "Banana");
    w::setCompleter(ui, fruit, {.items = kItems, .filter = w::MatchMode::EndsWith});
    clear(ui, fruit);
    ui.textInput("e");
    UI_CHECK(w::completionCount(ui, fruit) == 2); // Apple, apple pie
    // non-ASCII case folding
    w::setCompleter(ui, fruit, {.items = "Ärger\nÖl\närmel"});
    clear(ui, fruit);
    ui.textInput("är");
    UI_CHECK(w::completionCount(ui, fruit) == 2);

    // ---- wrapAround off; maxVisibleItems + wheel scroll; the highlight scrolls into view ----
    w::setCompleter(ui, fruit, {.items = kItems, .maxVisibleItems = 2, .wrapAround = false});
    clear(ui, fruit);
    ui.textInput("a");
    ui.update();
    const NodeHandle pop2 = w::completerPopup(ui, fruit);
    UI_CHECK(children(ui, pop2) == 2 && ui.text(child(ui, pop2, 0)) == "Apple");
    ui.wheel(ctr(ui.rect(pop2)), {0.f, -1.f});
    UI_CHECK(ui.text(child(ui, pop2, 0)) == "apple pie" && ui.text(child(ui, pop2, 1)) == "Avocado");
    ui.keyDown(Key::Down); // row 0 scrolls back into view
    UI_CHECK(ui.text(child(ui, pop2, 0)) == "Apple" && ui.checked(child(ui, pop2, 0)));
    ui.keyDown(Key::End, kModCtrl);
    ui.keyDown(Key::Down); // no wrap: stays on the last row
    UI_CHECK(w::completerCurrentRow(ui, fruit) == 3 && ui.text(child(ui, pop2, 1)) == "Avocado");
    ui.keyDown(Key::Escape);

    // ---- inline completion ----
    w::setCompleter(ui, fruit, {.items = kItems, .mode = w::CompletionMode::Inline});
    UI_CHECK(ui.a11yState(fruit).autoComplete == A11yAutoComplete::Inline && ui.a11yState(fruit).expanded == -1);
    clear(ui, fruit);
    ui.textInput("Ba");
    UI_CHECK(w::lineEditText(ui, fruit) == "Banana" && !w::completerPopupVisible(ui, fruit));
    UI_CHECK(ui.editBuffer(fruit).selection() == "nana");
    ui.textInput("x"); // replaces the completed part
    UI_CHECK(w::lineEditText(ui, fruit) == "Bax");
    ui.keyDown(Key::Backspace);
    UI_CHECK(w::lineEditText(ui, fruit) == "Ba"); // no inline completion after a deletion
    clear(ui, fruit);
    ui.textInput("ap");
    UI_CHECK(w::lineEditText(ui, fruit) == "apple" && ui.editBuffer(fruit).selection() == "ple"); // typed case kept
    ui.keyDown(Key::Down); // cycle
    UI_CHECK(w::lineEditText(ui, fruit) == "apricot" && hiText == "Apricot");
    ui.keyDown(Key::Down);
    UI_CHECK(w::lineEditText(ui, fruit) == "apple pie");
    ui.keyDown(Key::Up);
    UI_CHECK(w::lineEditText(ui, fruit) == "apricot");
    const i32 before = actCount;
    ui.keyDown(Key::Enter);
    UI_CHECK(actCount == before + 1 && actText == "apricot" && !ui.editBuffer(fruit).hasSelection());

    // ---- unfiltered popup: every row listed, the best match highlighted, text untouched ----
    w::setCompleter(ui, fruit, {.items = kItems, .mode = w::CompletionMode::UnfilteredPopup});
    clear(ui, fruit);
    ui.textInput("ban");
    UI_CHECK(w::completerPopupVisible(ui, fruit) && w::completionCount(ui, fruit) == 5);
    UI_CHECK(w::completerCurrentRow(ui, fruit) == 2 && w::lineEditText(ui, fruit) == "ban");
    ui.keyDown(Key::Enter);
    UI_CHECK(w::lineEditText(ui, fruit) == "Banana");

    // ---- model-backed (column 1), model changes ----
    w::StringTableModel table("code\tcity");
    table.addRow("SYD\tSydney");
    table.addRow("MEL\tMelbourne");
    w::setCompleter(ui, fruit, {.model = &table.model(), .column = 1});
    clear(ui, fruit);
    ui.textInput("s");
    UI_CHECK(w::completionCount(ui, fruit) == 1 && w::completionAt(ui, fruit, 0) == "Sydney");
    table.addRow("SIN\tSingapore");
    w::completerModelChanged(ui, fruit);
    UI_CHECK(w::completionCount(ui, fruit) == 2 && w::completerPopupVisible(ui, fruit));
    ui.update();
    UI_CHECK(children(ui, w::completerPopup(ui, fruit)) == 2);
    ui.keyDown(Key::Down);
    ui.keyDown(Key::Down);
    ui.keyDown(Key::Tab); // Tab chooses and moves focus on
    UI_CHECK(w::lineEditText(ui, fruit) == "Singapore" && actRow == 2 && ui.focused() == other);

    // ---- removing the completer ----
    w::setCompleter(ui, fruit, {});
    ui.setFocus(fruit);
    clear(ui, fruit);
    ui.textInput("s");
    UI_CHECK(!w::completerPopupVisible(ui, fruit) && w::completionCount(ui, fruit) == 0);
    UI_CHECK(ui.a11yState(fruit).autoComplete == A11yAutoComplete::None);
    ui.update();

    return fuse::ui::test::finish("fuse_ui_completer");
}
