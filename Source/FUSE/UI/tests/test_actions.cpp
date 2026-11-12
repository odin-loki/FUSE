// Gate fuse_ui_actions: ActionRegistry (QAction / QShortcut; plan §4.7). Shortcut text round trip and aliases, the
// platform primary modifier; shortcut dispatch on KeyDown with the scope rules (window, focused subtree, modal scope
// blocks the window's actions, text fields keep plain typing, disabled actions do not match); widgets bound to an
// action stay in sync (text, enabled, checked) and trigger it; menus built from action lists show the shortcut and
// the state and trigger on pick; rebinding with conflict detection; save / load of the bindings.
#include "test_util.hpp"

#include "fuse/ui/actions.hpp"

#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
int g_fired[8] = {};
bool g_lastChecked = false;
template <int N>
void fire(UiContext&, ActionHandle, bool checked) {
    ++g_fired[N];
    g_lastChecked = checked;
}
NodeHandle menuItem(UiContext& ui, std::string_view labelStart) {
    const u32 cls = ui.registry().find("menuitem");
    for (u32 i = 0; i < 4096; ++i) {
        const NodeHandle n = ui.handleOf(i);
        if (ui.alive(n) && ui.classOf(n) == cls && ui.text(n).starts_with(labelStart) && ui.computed(n).visible) return n;
    }
    return {};
}
Vec2 centre(const Rect& r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
} // namespace

int main() {
    // ---- shortcut text ----
    Shortcut s;
    UI_CHECK(parseShortcut("Ctrl+Shift+S", s) && s.key == Key::S && s.mods == (kModCtrl | kModShift));
    UI_CHECK(shortcutToString(s) == "Ctrl+Shift+S");
    UI_CHECK(parseShortcut(" shift + CONTROL + s ", s) && shortcutToString(s) == "Ctrl+Shift+S");
    UI_CHECK(parseShortcut("alt+f5", s) && s.key == Key::F5 && s.mods == kModAlt && shortcutToString(s) == "Alt+F5");
    UI_CHECK(parseShortcut("Escape", s) && s.key == Key::Escape && shortcutToString(s) == "Esc");
    UI_CHECK(parseShortcut("Ctrl+[", s) && s.key == Key::BracketLeft && shortcutToString(s) == "Ctrl+[");
    UI_CHECK(parseShortcut("Ctrl+Alt+Shift+PageDown", s) && shortcutToString(s) == "Ctrl+Alt+Shift+PageDown");
    UI_CHECK(parseShortcut("7", s) && s.key == Key::Num7 && s.mods == 0);
    UI_CHECK(!parseShortcut("", s) && !parseShortcut("Ctrl+", s) && !parseShortcut("Hyper+S", s) && !parseShortcut("Ctrl+Nope", s));
    UI_CHECK(shortcutToString({}).empty());
    for (int k = int(Key::Tab); k < int(Key::Other); ++k) { // every key has a name that parses back
        Key back = Key::None;
        UI_CHECK(!keyName(Key(k)).empty() && keyFromName(keyName(Key(k)), back) && back == Key(k));
    }
    // the primary modifier is the platform's; "Primary+" follows it
    setPrimaryModifier(kModSuper);
    UI_CHECK(parseShortcut("Primary+Z", s) && s.mods == kModSuper && primaryShortcut(Key::Z, kModShift).mods == (kModSuper | kModShift));
    setPrimaryModifier(kModCtrl);
    UI_CHECK(parseShortcut("CmdOrCtrl+Z", s) && s.mods == kModCtrl && shortcutToString(primaryShortcut(Key::Z)) == "Ctrl+Z");

    UiContext ui({.viewport = {640, 480}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "8px");
    ui.setCss(ui.root(), "gap", "6px");
    const NodeHandle bar = w::menuBar(ui, ui.root(), "bar");
    const NodeHandle saveBtn = w::button(ui, ui.root(), "?", "save");
    const NodeHandle gridBox = w::checkBox(ui, ui.root(), "?", false, "grid");
    const NodeHandle gridTool = w::toolButton(ui, ui.root(), "?", "gridtool");
    const NodeHandle edit = w::lineEdit(ui, ui.root(), "", "edit");
    const NodeHandle panelA = w::panel(ui, ui.root(), "panelA");
    const NodeHandle inA = w::button(ui, panelA, "Inside A", "inA");
    const NodeHandle outside = w::button(ui, ui.root(), "Outside", "outside");

    ActionRegistry reg(ui);
    const ActionHandle save = reg.add({.id = "file.save", .text = "Save", .shortcut = primaryShortcut(Key::S)}, &fire<0>);
    const ActionHandle open = reg.add({.id = "file.open", .text = "Open...", .shortcut = primaryShortcut(Key::O)}, &fire<1>);
    const ActionHandle brush = reg.add({.id = "tool.brush", .text = "Brush", .shortcut = {Key::B, 0}}, &fire<2>);
    const ActionHandle grid = reg.add({.id = "view.grid", .text = "Show grid", .shortcut = {Key::G, kModCtrl}, .checkable = true}, &fire<3>);
    const ActionHandle run = reg.add({.id = "run", .text = "Run", .shortcut = {Key::F5, 0}}, &fire<4>);
    const ActionHandle dupWin = reg.add({.id = "dup.window", .text = "Duplicate", .shortcut = {Key::D, kModCtrl}}, &fire<5>);
    const ActionHandle dupA = reg.add({.id = "dup.a", .text = "Duplicate in A", .shortcut = {Key::D, kModCtrl}, .scope = ActionScope::Subtree, .scopeNode = panelA}, &fire<6>);
    UI_CHECK(save.valid() && open.valid() && brush.valid() && grid.valid() && run.valid() && dupWin.valid() && dupA.valid());
    UI_CHECK(!reg.add({.id = "file.save", .text = "again"}).valid()); // ids are unique
    UI_CHECK(reg.find("file.open") == open && !reg.find("nope").valid());
    UI_CHECK_EQ(reg.count(), 7u);
    ui.update(0.f);
    UiTestDriver drv(ui);

    // ---- shortcut dispatch ----
    UI_CHECK(ui.keyDown(Key::S, kModCtrl));
    UI_CHECK_EQ(g_fired[0], 1);
    UI_CHECK(!ui.keyDown(Key::S, 0)); // not this shortcut
    UI_CHECK(!ui.keyDown(Key::S, kModCtrl | kModShift));
    UI_CHECK_EQ(g_fired[0], 1);
    UI_CHECK(ui.keyDown(Key::B, 0)); // a single-letter shortcut, nothing focused
    UI_CHECK_EQ(g_fired[2], 1);
    ui.setFocus(outside, FocusReason::Keyboard);
    ui.keyDown(Key::B, 0); // a button has the focus: still fires
    UI_CHECK_EQ(g_fired[2], 2);
    // a text field keeps plain typing; modified and function-key shortcuts still fire
    drv.click("edit");
    UI_CHECK(ui.focused() == edit);
    ui.keyDown(Key::B, 0);
    ui.textInput("b");
    UI_CHECK_EQ(g_fired[2], 2);
    ui.keyDown(Key::B, kModShift);
    UI_CHECK_EQ(g_fired[2], 2);
    ui.keyDown(Key::S, kModCtrl);
    UI_CHECK_EQ(g_fired[0], 2);
    ui.keyDown(Key::F5, 0);
    UI_CHECK_EQ(g_fired[4], 1);
    UI_CHECK(!reg.match(Key::B, 0).valid() && reg.match(Key::F5, 0) == run);

    // subtree scope beats the window action while the focus is inside it
    ui.setFocus(inA, FocusReason::Keyboard);
    UI_CHECK(reg.match(Key::D, kModCtrl) == dupA);
    ui.keyDown(Key::D, kModCtrl);
    UI_CHECK(g_fired[6] == 1 && g_fired[5] == 0);
    ui.setFocus(outside, FocusReason::Keyboard);
    ui.keyDown(Key::D, kModCtrl);
    UI_CHECK(g_fired[6] == 1 && g_fired[5] == 1);

    // disabled: no match, the key is not consumed by the registry
    reg.setEnabled(run, false);
    UI_CHECK(!reg.match(Key::F5, 0).valid());
    ui.setFocus({});
    UI_CHECK(!ui.keyDown(Key::F5, 0));
    UI_CHECK_EQ(g_fired[4], 1);
    UI_CHECK(!reg.trigger(run));
    reg.setEnabled(run, true);
    UI_CHECK(reg.trigger(run));
    UI_CHECK_EQ(g_fired[4], 2);

    // a modal dialog blocks the window's actions; an action owned by the dialog works in it
    const NodeHandle dlg = w::dialog(ui, "Confirm", "dlg");
    w::button(ui, w::dialogBody(ui, dlg), "OK", "dlg.ok");
    const ActionHandle accept = reg.add({.id = "dlg.accept", .text = "Accept", .shortcut = {Key::S, kModCtrl | kModAlt}, .scopeNode = dlg}, &fire<7>);
    w::openDialog(ui, dlg);
    ui.update(0.f);
    ui.keyDown(Key::S, kModCtrl);
    UI_CHECK_EQ(g_fired[0], 2);
    UI_CHECK(!reg.match(Key::S, kModCtrl).valid());
    ui.keyDown(Key::S, kModCtrl | kModAlt);
    UI_CHECK_EQ(g_fired[7], 1);
    w::closeDialog(ui, dlg);
    ui.update(0.f);
    ui.keyDown(Key::S, kModCtrl);
    UI_CHECK_EQ(g_fired[0], 3);

    // ---- bound widgets ----
    reg.bind(save, saveBtn);
    reg.bind(grid, gridBox);
    reg.bind(grid, gridTool);
    ui.update(0.f);
    UI_CHECK(ui.text(saveBtn) == "Save" && ui.text(gridBox) == "Show grid" && ui.text(gridTool) == "Show grid");
    drv.click("save");
    UI_CHECK_EQ(g_fired[0], 4);
    reg.setText(save, "Save scene");
    reg.setEnabled(save, false);
    UI_CHECK(ui.text(saveBtn) == "Save scene" && ui.disabled(saveBtn));
    drv.click("save");
    UI_CHECK_EQ(g_fired[0], 4);
    reg.setEnabled(save, true);
    UI_CHECK(!ui.disabled(saveBtn));
    // checkable: the check box, the tool button and the shortcut all toggle the one action
    drv.click("grid");
    UI_CHECK(reg.checked(grid) && ui.checked(gridBox) && ui.checked(gridTool) && g_fired[3] == 1 && g_lastChecked);
    drv.click("gridtool");
    UI_CHECK(!reg.checked(grid) && !ui.checked(gridBox) && !ui.checked(gridTool) && g_fired[3] == 2 && !g_lastChecked);
    ui.setFocus({});
    ui.keyDown(Key::G, kModCtrl);
    UI_CHECK(reg.checked(grid) && ui.checked(gridBox) && ui.checked(gridTool) && g_fired[3] == 3);
    reg.setChecked(grid, false); // state sync only: no callback
    UI_CHECK(!ui.checked(gridBox) && g_fired[3] == 3);
    reg.unbind(gridTool);
    reg.setChecked(grid, true);
    UI_CHECK(ui.checked(gridBox) && !ui.checked(gridTool));

    // ---- menus from action lists ----
    reg.setEnabled(run, false);
    const u32 fileList = reg.addList({save, open, ActionHandle{}, grid, run});
    UI_CHECK(w::actionMenuItems(reg, fileList) == "Save scene\tCtrl+S\nOpen...\tCtrl+O\n-\n\x02Show grid\tCtrl+G\n\x01Run\tF5");
    const NodeHandle fileMenu = w::actionMenuBarMenu(ui, reg, bar, "File", fileList, "menu.file");
    ui.update(0.f);
    UI_CHECK(ui.text(fileMenu).starts_with("File\nSave scene\tCtrl+S\n"));
    drv.click("menu.file");
    ui.update(0.f);
    const NodeHandle miSave = menuItem(ui, "Save scene"), miGrid = menuItem(ui, "Show grid"), miRun = menuItem(ui, "Run");
    UI_CHECK(ui.alive(miSave) && ui.alive(miGrid) && ui.alive(miRun));
    UI_CHECK(ui.checked(miGrid) && !ui.checked(miSave));
    UI_CHECK(ui.disabled(miRun) && !ui.disabled(miSave));
    // the item is wide enough for the label, a gap and the right-aligned shortcut text
    const f32 fs = ui.computed(miSave).fontSize;
    UI_CHECK(ui.contentRect(miSave).w >= ui.measureString("Save scene", fs) + ui.measureString("Ctrl+S", fs) + 20.f);
    bool glyphRight = false; // glyphs end at the content box's right edge
    for (const DrawCmd& c : ui.drawList().cmds())
        if (c.op == DrawOp::Glyph && c.node == miSave.index) glyphRight = glyphRight || c.rect.right() > ui.contentRect(miSave).right() - 6.f;
    UI_CHECK(glyphRight);
    drv.clickAt(centre(ui.rect(miRun))); // disabled: nothing happens, the menu stays
    UI_CHECK(ui.alive(miSave) && g_fired[4] == 2);
    drv.clickAt(centre(ui.rect(miSave)));
    ui.update(0.f);
    UI_CHECK_EQ(g_fired[0], 5);
    UI_CHECK(!ui.alive(miSave)); // the popup closed
    // the menu text follows the actions
    reg.setEnabled(run, true);
    reg.setChecked(grid, false);
    UI_CHECK(ui.text(fileMenu) == "File\nSave scene\tCtrl+S\nOpen...\tCtrl+O\n-\nShow grid\tCtrl+G\nRun\tF5");
    drv.click("menu.file");
    ui.update(0.f);
    drv.clickAt(centre(ui.rect(menuItem(ui, "Show grid"))));
    ui.update(0.f);
    UI_CHECK(reg.checked(grid) && ui.checked(gridBox));
    // a popup (context) menu from the same list
    const NodeHandle scope = w::actionPopupMenu(ui, reg, outside, fileList, {200.f, 200.f});
    ui.update(0.f);
    UI_CHECK(w::popupOpen(ui, scope));
    drv.clickAt(centre(ui.rect(menuItem(ui, "Open..."))));
    ui.update(0.f);
    UI_CHECK_EQ(g_fired[1], 1);
    UI_CHECK(!ui.alive(scope));

    // ---- rebinding ----
    UI_CHECK(reg.conflict(open, primaryShortcut(Key::S)) == save);
    auto r = reg.rebind(open, primaryShortcut(Key::S));
    UI_CHECK(!r && r.error() == save);
    UI_CHECK(reg.shortcut(open) == primaryShortcut(Key::O)); // unchanged
    UI_CHECK(!reg.conflict(dupA, {Key::D, kModCtrl}).valid() && !reg.conflict(dupWin, {Key::D, kModCtrl}).valid()); // different scopes
    UI_CHECK(reg.rebind(open, {Key::O, kModCtrl | kModShift}).has_value());
    ui.setFocus({});
    ui.keyDown(Key::O, kModCtrl);
    UI_CHECK_EQ(g_fired[1], 1);
    ui.keyDown(Key::O, kModCtrl | kModShift);
    UI_CHECK_EQ(g_fired[1], 2);
    UI_CHECK(ui.text(fileMenu).find("Open...\tCtrl+Shift+O") != std::string_view::npos); // menus show the new binding
    UI_CHECK(reg.rebind(open, primaryShortcut(Key::S), true).has_value()); // steal: Save loses its shortcut
    UI_CHECK(!reg.shortcut(save).valid() && reg.shortcut(open) == primaryShortcut(Key::S));
    ui.keyDown(Key::S, kModCtrl);
    UI_CHECK(g_fired[1] == 3 && g_fired[0] == 5);
    UI_CHECK(reg.rebind(open, {}).has_value()); // unbind
    UI_CHECK(!ui.keyDown(Key::S, kModCtrl));

    // ---- persistence ----
    const std::string saved = reg.saveShortcuts();
    UI_CHECK(saved.find("file.save=\n") != std::string::npos && saved.find("view.grid=Ctrl+G\n") != std::string::npos);
    UI_CHECK_EQ(reg.loadShortcuts("file.save=Ctrl+S\nfile.open = Ctrl+O\nunknown.id=Ctrl+U\nrun=NotAKey\ntool.brush=\n"), 3u);
    UI_CHECK((reg.shortcut(save) == Shortcut{Key::S, kModCtrl}) && (reg.shortcut(open) == Shortcut{Key::O, kModCtrl}));
    UI_CHECK(!reg.shortcut(brush).valid() && (reg.shortcut(run) == Shortcut{Key::F5, 0}));
    UI_CHECK_EQ(reg.loadShortcuts(saved), reg.count());
    UI_CHECK(reg.saveShortcuts() == saved);

    // ---- removal ----
    reg.remove(accept);
    reg.remove(dupA);
    UI_CHECK(!reg.alive(dupA) && reg.count() == 6u);
    ui.setFocus(inA, FocusReason::Keyboard);
    ui.keyDown(Key::D, kModCtrl);
    UI_CHECK(g_fired[6] == 1 && g_fired[5] == 2); // the window action takes over
    setPrimaryModifier(0);
    return finish("fuse_ui_actions");
}
