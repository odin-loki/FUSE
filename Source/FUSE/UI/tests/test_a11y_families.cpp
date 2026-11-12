// fuse_ui_a11y_families: accessibility completeness (plan §4.14) per widget family - the tree dump (roles, names,
// values, states, virtual children of virtualised views, actions) compared as text, the actions performed through
// UiContext::performA11yAction, and the bridge (stable ids of virtual children, diffing, live-region announcements).
// `--gate=<name>` runs one family (ctest fuse_ui_a11y_family_<name>); `--print` prints the dumps instead of comparing.
#include "test_util.hpp"

#include "fuse/ui/a11y/a11y.hpp"
#include "fuse/ui/widgets/modelview.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;
using A = A11yAction;

namespace {
bool g_print = false;
struct Fixture {
    UiContext ui{{.viewport = {640, 480}, .font = &font()}};
    Fixture() { w::install(ui); }
    std::string dump(bool actions = true) {
        ui.update();
        std::string s;
        ui.dumpA11y(s, actions);
        return s;
    }
};
void expect(const char* what, const std::string& got, const char* want, int line) {
    if (g_print) {
        std::printf("==== %s ====\n%s", what, got.c_str());
        return;
    }
    if (got == want) return;
    std::fprintf(stderr, "%s:%d: a11y dump '%s' differs\n--- got ---\n%s--- want ---\n%s", __FILE__, line, what, got.c_str(), want);
    ++failures();
}
#define EXPECT_DUMP(what, got, want) expect(what, got, want, __LINE__)
/// The snapshot entry of `node` / `child` (a copy; role None when absent).
A11ySnapshotNode entry(const UiContext& ui, NodeHandle node, u32 child = 0) {
    std::vector<A11ySnapshotNode> snap;
    ui.snapshotA11y(snap);
    for (const A11ySnapshotNode& s : snap)
        if (s.node == node && s.child == child) return s;
    return {};
}

// ---- Tier A controls ------------------------------------------------------------------------------------------------
void gateControls() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle ok = w::button(ui, ui.root(), "Apply", "apply");
    const NodeHandle cb = w::checkBox(ui, ui.root(), "Shadows", false, "shadows");
    const NodeHandle sl = w::slider(ui, ui.root(), 0.f, 10.f, 4.f, 1.f, "exposure");
    ui.setA11y(sl, A11yRole::Slider, "Exposure");
    const NodeHandle sb = w::doubleSpinBox(ui, ui.root(), 0.f, 1.f, 0.5f, 0.1f, "gain");
    const NodeHandle combo = w::comboBox(ui, ui.root(), "Vulkan\nCPU raster\nNull", 0, "backend");
    ui.setA11y(combo, A11yRole::ComboBox, "Backend");
    const NodeHandle le = w::lineEdit(ui, ui.root(), "player_one", "name");
    ui.setA11y(le, A11yRole::TextField, "Player name");
    const NodeHandle pb = w::progressBar(ui, ui.root(), 0.25f, "progress");
    const NodeHandle off = w::button(ui, ui.root(), "Disabled", "off");
    ui.setDisabled(off, true);
    int clicks = 0;
    ui.connect(ok, SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
    std::string typed;
    ui.connect(le, SignalKind::TextChanged, [&typed](UiContext&, NodeHandle, const SignalArgs& a) { typed = std::string(a.text); });
    EXPECT_DUMP("controls", f.dump(),
                "window\n"
                "  button \"Apply\" focusable actions=focus,click\n"
                "  checkbox \"Shadows\" focusable actions=focus,click\n"
                "  slider \"Exposure\" focusable value=4 range=[0,10] actions=focus,increment,decrement,setvalue\n"
                "  spinbox focusable value=0.5 range=[0,1] actions=focus,increment,decrement,setvalue\n"
                "  combobox \"Backend\" focusable collapsed value=\"Vulkan\" pos=1/3 actions=focus,click,increment,decrement,setvalue,expand\n"
                "  textfield \"Player name\" focusable value=\"player_one\" actions=focus,setvalue\n"
                "  progressbar value=0.25 range=[0,1]\n"
                "  button \"Disabled\" focusable disabled actions=focus\n");
    // click / focus
    UI_CHECK(ui.performA11yAction(ok, A::Click) && clicks == 1);
    UI_CHECK(ui.performA11yAction(cb, A::Click) && ui.checked(cb));
    UI_CHECK(ui.performA11yAction(sl, A::Focus) && ui.focused() == sl);
    UI_CHECK(!ui.performA11yAction(off, A::Click) && !ui.performA11yAction(pb, A::Focus));
    // increment / decrement use the widget's step, set value clamps
    UI_CHECK(ui.performA11yAction(sl, A::Increment) && ui.value(sl) == 5.f);
    UI_CHECK(ui.performA11yAction(sl, A::Decrement) && ui.performA11yAction(sl, A::Decrement) && ui.value(sl) == 3.f);
    UI_CHECK(ui.performA11yAction(sl, A::SetValue, 99.f) && ui.value(sl) == 10.f);
    UI_CHECK(ui.performA11yAction(sb, A::Increment) && std::abs(ui.value(sb) - 0.6f) < 1e-4f);
    UI_CHECK(!ui.performA11yAction(pb, A::Increment) && !ui.performA11yAction(ok, A::Increment) && !ui.performA11yAction(ok, A::Expand));
    // combo box: next / previous / by index, and Expand opens the popup list
    UI_CHECK(ui.performA11yAction(combo, A::Increment) && w::comboText(ui, combo) == "CPU raster");
    UI_CHECK(ui.performA11yAction(combo, A::SetValue, 2.f) && w::comboText(ui, combo) == "Null");
    UI_CHECK(ui.performA11yAction(combo, A::Increment) && w::comboText(ui, combo) == "Null"); // the last one
    UI_CHECK(entry(ui, combo).valueText == "Null" && entry(ui, combo).posInSet == 3u);
    const u32 before = ui.nodeCount();
    UI_CHECK(ui.performA11yAction(combo, A::Expand));
    ui.update();
    UI_CHECK(ui.nodeCount() > before);
    ui.keyDown(Key::Escape);
    ui.update();
    ui.update();
    // text field: SetValue replaces the text (and the edit buffer), TextChanged fires
    UI_CHECK(ui.performA11yAction(le, A::SetValue, 0.f, 0, "odin") && ui.text(le) == "odin" && typed == "odin");
    UI_CHECK(ui.editBuffer(le).text() == "odin" && entry(ui, le).valueText == "odin" && entry(ui, le).name == "Player name");
    // a dead node, an unknown virtual child
    UI_CHECK(!ui.performA11yAction({}, A::Click) && !ui.performA11yAction(ok, A::Click, 0.f, 3));
    // a click on a node covered by a modal dialog is refused; one inside it works
    const NodeHandle dlg = w::dialog(ui, "Confirm", "dlg");
    const NodeHandle yes = w::button(ui, w::dialogBody(ui, dlg), "Yes", "yes");
    int yesClicks = 0;
    ui.connect(yes, SignalKind::Clicked, [&yesClicks](UiContext&, NodeHandle, const SignalArgs&) { ++yesClicks; });
    w::openDialog(ui, dlg);
    ui.update();
    UI_CHECK(!ui.performA11yAction(ok, A::Click) && clicks == 1);
    UI_CHECK(ui.performA11yAction(yes, A::Click) && yesClicks == 1);
}

// ---- menus ----------------------------------------------------------------------------------------------------------
void gateMenus() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle bar = w::menuBar(ui, ui.root(), "bar");
    const NodeHandle file = w::menuBarMenu(ui, bar, "File", "New\nOpen\n-\nRecent\n\tscene.fuse\n\tlevel2.fuse\nQuit", "file");
    w::menuBarMenu(ui, bar, "Edit", "Undo\nRedo", "edit");
    const NodeHandle tip = w::button(ui, ui.root(), "Hover me", "tipped");
    w::setTooltip(ui, tip, "Explains the button", 0.1f);
    int picked = -1;
    ui.connect(file, SignalKind::Submitted, [&picked](UiContext&, NodeHandle, const SignalArgs& a) { picked = int(a.value); });
    EXPECT_DUMP("menubar", f.dump(),
                "window\n"
                "  menubar\n"
                "    button \"File\" focusable collapsed actions=focus,click,expand\n"
                "    button \"Edit\" focusable collapsed actions=focus,click,expand\n"
                "  button \"Hover me\" focusable actions=focus,click\n");
    // Expand opens the menu: a menu of menu items, the one with a submenu expandable
    UI_CHECK(ui.performA11yAction(file, A::Expand));
    EXPECT_DUMP("menu open", f.dump(),
                "window\n"
                "  menubar\n"
                "    button \"File\" focusable collapsed actions=focus,click,expand\n"
                "    button \"Edit\" focusable collapsed actions=focus,click,expand\n"
                "  button \"Hover me\" focusable actions=focus,click\n"
                "  menu\n"
                "    menuitem \"New\" focusable focused actions=focus,click\n"
                "    menuitem \"Open\" focusable actions=focus,click\n"
                "    separator\n"
                "    menuitem \"Recent\" focusable collapsed actions=focus,click\n"
                "    menuitem \"Quit\" focusable actions=focus,click\n");
    // clicking a menu item through the action entry point picks it
    std::vector<A11ySnapshotNode> snap;
    ui.snapshotA11y(snap);
    NodeHandle open;
    for (const A11ySnapshotNode& s : snap)
        if (s.role == A11yRole::MenuItem && s.name == "Open") open = s.node;
    UI_CHECK(open.valid() && ui.performA11yAction(open, A::Click));
    ui.update();
    ui.update();
    UI_CHECK_EQ(picked, 1);
    // tooltip: shown after the hover delay, with the Tooltip role
    ui.pointerMove({ui.rect(tip).x + 5.f, ui.rect(tip).y + 5.f});
    for (int i = 0; i < 30; ++i) ui.update(1.f / 60.f);
    const std::string d = f.dump(false);
    UI_CHECK(d.find("tooltip \"Explains the button\"") != std::string::npos);
}

// ---- dialogs, toasts, status bar: modal state and live regions ------------------------------------------------------
void gateDialogsLive() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle tb = w::toolBar(ui, ui.root(), "tb");
    w::toolBarAction(ui, tb, "Save", "Save the scene", "save");
    const NodeHandle sb = w::statusBar(ui, ui.root(), "status");
    w::statusBarPermanent(ui, sb, "Ln 1, Col 1");
    a11y::Bridge bridge(ui);
    ui.update();
    bridge.sync();
    UI_CHECK(bridge.takeAnnouncements().empty()); // nothing is announced for a UI that is merely there
    EXPECT_DUMP("bars", f.dump(),
                "window\n"
                "  toolbar \"Tool bar\"\n"
                "    button \"Save\" focusable actions=focus,click\n"
                "  statusbar\n"
                "    text live=polite\n"
                "    text \"Ln 1, Col 1\"\n");
    // a status message is a live region: announced when it changes, once
    w::statusMessage(ui, sb, "Saved scene.fuse");
    ui.update();
    UI_CHECK(bridge.sync() > 0u);
    std::vector<a11y::Announcement> said = bridge.takeAnnouncements();
    UI_CHECK(said.size() == 1 && said[0].text == "Saved scene.fuse" && !said[0].assertive);
    ui.update();
    bridge.sync();
    UI_CHECK(bridge.takeAnnouncements().empty());
    w::statusMessage(ui, sb, "Build finished");
    ui.update();
    bridge.sync();
    said = bridge.takeAnnouncements();
    UI_CHECK(said.size() == 1 && said[0].text == "Build finished");
    w::statusMessage(ui, sb, ""); // clearing says nothing
    ui.update();
    bridge.sync();
    UI_CHECK(bridge.takeAnnouncements().empty());
    // a toast is an alert, announced when it appears
    const NodeHandle t = w::toast(ui, "Project saved", 30.f, "toast");
    ui.update(1.f / 60.f);
    bridge.sync();
    said = bridge.takeAnnouncements();
    UI_CHECK(said.size() == 1 && said[0].text == "Project saved" && said[0].id == a11y::idOf(t));
    UI_CHECK(entry(ui, t).role == A11yRole::Alert && entry(ui, t).live == A11yLive::Polite);
    // an app's own live node, assertive
    const NodeHandle err = w::label(ui, ui.root(), "", "error");
    ui.setA11yLive(err, A11yLive::Assertive);
    ui.update();
    bridge.sync();
    UI_CHECK(bridge.takeAnnouncements().empty()); // empty: nothing to say yet
    ui.setText(err, "Disk full");
    ui.update();
    bridge.sync();
    said = bridge.takeAnnouncements();
    UI_CHECK(said.size() == 1 && said[0].text == "Disk full" && said[0].assertive);

    // a message box is a modal dialog
    Fixture g;
    const NodeHandle box = w::messageBox(g.ui, "Delete asset", "This cannot be undone.", "Delete\nCancel", w::MessageIcon::Warning, "box");
    EXPECT_DUMP("message box", g.dump(),
                "window\n"
                "  dialog \"Delete asset\" modal\n"
                "    text \"Delete asset\"\n"
                "    text \"!\"\n"
                "    text \"This cannot be undone.\"\n"
                "    button \"Delete\" focusable focused actions=focus,click\n"
                "    button \"Cancel\" focusable actions=focus,click\n");
    std::vector<A11ySnapshotNode> snap;
    g.ui.snapshotA11y(snap);
    NodeHandle cancel;
    for (const A11ySnapshotNode& s : snap)
        if (s.role == A11yRole::Button && s.name == "Cancel") cancel = s.node;
    UI_CHECK(cancel.valid() && g.ui.performA11yAction(cancel, A::Click));
    g.ui.update();
    UI_CHECK(g.ui.value(box) == 1.f && !w::popupOpen(g.ui, g.ui.parent(box)));
    EXPECT_DUMP("message box closed", g.dump(), "window\n");
}

// ---- list view: visible rows as children with stable ids ------------------------------------------------------------
void gateList() {
    Fixture f;
    UiContext& ui = f.ui;
    std::string items;
    for (int i = 0; i < 100; ++i) items += (i ? "\n" : "") + std::string("Item ") + std::to_string(i);
    const NodeHandle list = w::listView(ui, ui.root(), items, 2, "list");
    ui.setA11y(list, A11yRole::List, "Assets");
    ui.setCss(list, "height", "110px");
    ui.update();
    const std::string top = f.dump();
    EXPECT_DUMP("list", top,
                "window\n"
                "  list \"Assets\" focusable actions=focus,click,scrollup,scrolldown\n"
                "    listitem #1 \"Item 0\" pos=1/100 actions=click,select,scrollintoview\n"
                "    listitem #2 \"Item 1\" pos=2/100 actions=click,select,scrollintoview\n"
                "    listitem #3 \"Item 2\" selected pos=3/100 actions=click,select,scrollintoview\n"
                "    listitem #4 \"Item 3\" pos=4/100 actions=click,select,scrollintoview\n"
                "    listitem #5 \"Item 4\" pos=5/100 actions=click,select,scrollintoview\n");
    // only the visible rows are exposed, whatever the item count; their bounds are the painted rows
    std::vector<A11ySnapshotNode> snap;
    ui.snapshotA11y(snap);
    UI_CHECK_EQ(snap.size(), std::size_t(7));
    UI_CHECK(snap[2].child == 1u && snap[3].bounds.y == snap[2].bounds.bottom() && snap[2].bounds.x == ui.contentRect(list).x);
    UI_CHECK(snap[2].parent == 1u && snap[1].setSize == 100u);
    // Select through the action entry point selects the row (ValueChanged + TextChanged, as a click does)
    int changes = 0;
    std::string text;
    ui.connect(list, SignalKind::ValueChanged, [&changes](UiContext&, NodeHandle, const SignalArgs&) { ++changes; });
    ui.connect(list, SignalKind::TextChanged, [&text](UiContext&, NodeHandle, const SignalArgs& a) { text = std::string(a.text); });
    UI_CHECK(ui.performA11yAction(list, A::Select, 0.f, 2) && ui.value(list) == 1.f && changes == 1 && text == "Item 1");
    // scrolling keeps the ids: row 3 is still #4 after one notch (3 rows), and the window moved
    UI_CHECK(ui.performA11yAction(list, A::ScrollDown));
    ui.update();
    UI_CHECK_EQ(w::listFirstVisible(ui, list), 3u);
    EXPECT_DUMP("list scrolled", f.dump(false),
                "window\n"
                "  list \"Assets\" focusable\n"
                "    listitem #4 \"Item 3\" pos=4/100\n"
                "    listitem #5 \"Item 4\" pos=5/100\n"
                "    listitem #6 \"Item 5\" pos=6/100\n"
                "    listitem #7 \"Item 6\" pos=7/100\n"
                "    listitem #8 \"Item 7\" pos=8/100\n");
    // ScrollIntoView on a row far away (it need not be in the snapshot) brings it in; Select works on it too
    UI_CHECK(ui.performA11yAction(list, A::ScrollIntoView, 0.f, 91));
    ui.update();
    UI_CHECK(entry(ui, list, 91).name == "Item 90" && w::listFirstVisible(ui, list) == 86u);
    UI_CHECK(ui.performA11yAction(list, A::Select, 0.f, 100) && ui.value(list) == 99.f && text == "Item 99");
    UI_CHECK(entry(ui, list, 100).selected && !ui.performA11yAction(list, A::Select, 0.f, 101));
    UI_CHECK(ui.performA11yAction(list, A::ScrollUp));
    ui.setDisabled(list, true);
    UI_CHECK(!ui.performA11yAction(list, A::Select, 0.f, 1) && entry(ui, list, 98).actions == 0u);
}

// ---- tree view ------------------------------------------------------------------------------------------------------
void gateTree() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle tree = w::treeView(ui, ui.root(), "Scene\n\tCamera\n\tLights\n\t\tSun\n\t\tFill\nAssets\n\tTextures", "tree");
    ui.setA11y(tree, A11yRole::Tree, "Outline");
    ui.setCss(tree, "height", "220px");
    ui.setValue(tree, 2.f);
    EXPECT_DUMP("tree", f.dump(),
                "window\n"
                "  tree \"Outline\" focusable actions=focus,click\n"
                "    treeitem #1 \"Scene\" expanded level=1 pos=1/7 actions=click,collapse,select,scrollintoview\n"
                "    treeitem #2 \"Camera\" level=2 pos=2/7 actions=click,select,scrollintoview\n"
                "    treeitem #3 \"Lights\" expanded selected level=2 pos=3/7 actions=click,collapse,select,scrollintoview\n"
                "    treeitem #4 \"Sun\" level=3 pos=4/7 actions=click,select,scrollintoview\n"
                "    treeitem #5 \"Fill\" level=3 pos=5/7 actions=click,select,scrollintoview\n"
                "    treeitem #6 \"Assets\" expanded level=1 pos=6/7 actions=click,collapse,select,scrollintoview\n"
                "    treeitem #7 \"Textures\" level=2 pos=7/7 actions=click,select,scrollintoview\n");
    int toggles = 0;
    ui.connect(tree, SignalKind::Toggled, [&toggles](UiContext&, NodeHandle, const SignalArgs&) { ++toggles; });
    // Collapse hides the rows below; the ids of the others do not change
    UI_CHECK(ui.performA11yAction(tree, A::Collapse, 0.f, 3) && !w::treeExpanded(ui, tree, 2) && toggles == 1);
    EXPECT_DUMP("tree collapsed", f.dump(false),
                "window\n"
                "  tree \"Outline\" focusable\n"
                "    treeitem #1 \"Scene\" expanded level=1 pos=1/7\n"
                "    treeitem #2 \"Camera\" level=2 pos=2/7\n"
                "    treeitem #3 \"Lights\" collapsed selected level=2 pos=3/7\n"
                "    treeitem #6 \"Assets\" expanded level=1 pos=6/7\n"
                "    treeitem #7 \"Textures\" level=2 pos=7/7\n");
    UI_CHECK(entry(ui, tree, 3).hasAction(A::Expand) && !entry(ui, tree, 3).hasAction(A::Collapse));
    UI_CHECK(!ui.performA11yAction(tree, A::Expand, 0.f, 4));  // hidden under the collapsed row
    UI_CHECK(!ui.performA11yAction(tree, A::Expand, 0.f, 2));  // a leaf
    UI_CHECK(ui.performA11yAction(tree, A::Expand, 0.f, 3) && w::treeExpanded(ui, tree, 2));
    UI_CHECK(ui.performA11yAction(tree, A::Select, 0.f, 7) && ui.value(tree) == 6.f);
    UI_CHECK(entry(ui, tree, 7).selected && !entry(ui, tree, 3).selected);
}

// ---- table view, model list view ------------------------------------------------------------------------------------
void gateTable() {
    Fixture f;
    UiContext& ui = f.ui;
    w::StringTableModel m("Name\tSize");
    for (int i = 0; i < 1000; ++i) m.addRow("file" + std::to_string(i) + "\t" + std::to_string(i * 3));
    const NodeHandle table = w::tableView(ui, ui.root(), &m.model(), "table");
    ui.setA11y(table, A11yRole::Table, "Files");
    ui.setCss(table, "height", "100px");
    ui.setCss(table, "width", "320px");
    ui.update();
    ui.setValue(table, 1.f);
    EXPECT_DUMP("table", f.dump(),
                "window\n"
                "  table \"Files\" focusable actions=focus,click,scrollup,scrolldown\n"
                "    row #2147418112\n"
                "      columnheader #2147418113 \"Name\" pos=1/2 actions=click\n"
                "      columnheader #2147418114 \"Size\" pos=2/2 actions=click\n"
                "    row #1 pos=1/1000 actions=click,select,scrollintoview\n"
                "      cell #2 \"file0\" pos=1/2\n"
                "      cell #3 \"0\" pos=2/2\n"
                "    row #34 selected pos=2/1000 actions=click,select,scrollintoview\n"
                "      cell #35 \"file1\" pos=1/2\n"
                "      cell #36 \"3\" pos=2/2\n"
                "    row #67 pos=3/1000 actions=click,select,scrollintoview\n"
                "      cell #68 \"file2\" pos=1/2\n"
                "      cell #69 \"6\" pos=2/2\n"
                "    row #100 pos=4/1000 actions=click,select,scrollintoview\n"
                "      cell #101 \"file3\" pos=1/2\n"
                "      cell #102 \"9\" pos=2/2\n");
    std::vector<A11ySnapshotNode> snap;
    ui.snapshotA11y(snap);
    UI_CHECK(snap.size() < 20); // 1000 rows, a handful of entries
    // cells sit in their columns
    const A11ySnapshotNode c0 = entry(ui, table, 2), c1 = entry(ui, table, 3);
    UI_CHECK(c1.bounds.x == c0.bounds.x + w::tableColumnWidth(ui, table, 0) && c0.bounds.w == w::tableColumnWidth(ui, table, 0));
    // a column header's Click sorts (Toggled, as a pointer click on the header)
    int sorts = 0;
    bool asc = false;
    ui.connect(table, SignalKind::Toggled, [&sorts, &asc](UiContext&, NodeHandle, const SignalArgs& a) { ++sorts; asc = a.flag; });
    UI_CHECK(ui.performA11yAction(table, A::Click, 0.f, 2147418114u) && sorts == 1 && asc);
    UI_CHECK(entry(ui, table, 2147418114u).valueText == "ascending");
    UI_CHECK(ui.performA11yAction(table, A::Click, 0.f, 2147418114u) && sorts == 2 && !asc);
    UI_CHECK(!ui.performA11yAction(table, A::Click, 0.f, 2147418112u + 9u));
    // rows: Select (also by one of its cells), ScrollIntoView far away; ids survive the scroll
    UI_CHECK(ui.performA11yAction(table, A::Select, 0.f, 68) && ui.value(table) == 2.f);
    UI_CHECK(ui.performA11yAction(table, A::ScrollIntoView, 0.f, 1u + 500u * 33u));
    ui.update();
    UI_CHECK(w::tableFirstVisible(ui, table) >= 497u && entry(ui, table, 1u + 500u * 33u + 1u).name == "file500");
    UI_CHECK(entry(ui, table, 1u + 500u * 33u).posInSet == 501u);
    UI_CHECK(!ui.performA11yAction(table, A::Select, 0.f, 1u + 1000u * 33u)); // past the last row
    UI_CHECK(ui.performA11yAction(table, A::ScrollUp));

    // a model list view is a list of list items
    Fixture g;
    w::StringTableModel names("name");
    for (int i = 0; i < 50; ++i) names.addRow("entry " + std::to_string(i));
    const NodeHandle ml = w::modelListView(g.ui, g.ui.root(), &names.model(), "ml");
    g.ui.setCss(ml, "height", "70px");
    EXPECT_DUMP("model list", g.dump(false),
                "window\n"
                "  list focusable\n"
                "    listitem #1 \"entry 0\" pos=1/50\n"
                "    listitem #34 \"entry 1\" pos=2/50\n"
                "    listitem #67 \"entry 2\" pos=3/50\n");
    UI_CHECK(g.ui.performA11yAction(ml, A::Select, 0.f, 34) && g.ui.value(ml) == 1.f);
}

// ---- model / view: multi-selection, cell selection, check / editable cells, tree model view, the in-place editor ----
// (and the classes that arrived with the vector drawing and action work: svg images, icon buttons, action menus)
struct FlagModel { // 3 x 3: column 0 text, column 1 a check box, column 2 editable text; row 2 is disabled
    std::string cells[3][3] = {{"alpha", "", "1"}, {"beta", "", "2"}, {"gamma", "", "3"}};
    int checks[3] = {0, 1, 0};
    w::ItemModel m;
    FlagModel() {
        m.user = this;
        m.columns = 3;
        m.rows = [](const void*) { return 3u; };
        m.data = [](const void* u, u32 r, u32 c, std::string&) { return std::string_view(static_cast<const FlagModel*>(u)->cells[r][c]); };
        m.header = [](const void*, u32 c) { return std::string_view(c == 0 ? "Name" : c == 1 ? "On" : "Count"); };
        m.flags = [](const void*, w::ItemId item, u32 c) {
            u32 f = w::kItemSelectable;
            if (item != 2) f |= w::kItemEnabled;
            if (c == 1) f |= w::kItemCheckable;
            if (c == 2) f |= w::kItemEditable;
            return f;
        };
        m.role = [](const void* u, w::ItemId item, u32 c, w::ItemRole role, w::ItemValue& out, std::string&) {
            if (c != 1 || role != w::ItemRole::CheckState) return false;
            out.integer = static_cast<const FlagModel*>(u)->checks[item];
            return true;
        };
        m.setData = [](const void* u, w::ItemId item, u32 c, w::ItemRole role, const w::ItemValue& v) {
            auto* self = const_cast<FlagModel*>(static_cast<const FlagModel*>(u));
            if (role == w::ItemRole::CheckState) { self->checks[item] = v.integer; return true; }
            self->cells[item][c].assign(v.text);
            return true;
        };
    }
};
void gateModelView() {
    { // extended (multi) row selection: every selected row says so; Select is idempotent, Click follows the mode
        Fixture f;
        UiContext& ui = f.ui;
        FlagModel fm;
        const NodeHandle table = w::tableView(ui, ui.root(), &fm.m, "table");
        ui.setCss(table, "height", "120px");
        ui.setCss(table, "width", "420px");
        w::setViewSelectionMode(ui, table, w::SelectionMode::Extended);
        ui.update();
        w::SelectionModel* sel = w::viewSelectionModel(ui, table);
        UI_CHECK(sel != nullptr);
        if (!sel) return;
        sel->selectRows(0, 1);
        EXPECT_DUMP("table multi-selection", f.dump(),
                    "window\n"
                    "  table focusable actions=focus,click\n"
                    "    row #2147418112\n"
                    "      columnheader #2147418113 \"Name\" pos=1/3 actions=click\n"
                    "      columnheader #2147418114 \"On\" pos=2/3 actions=click\n"
                    "      columnheader #2147418115 \"Count\" pos=3/3 actions=click\n"
                    "    row #1 selected pos=1/3 actions=click,select,scrollintoview\n"
                    "      cell #2 \"alpha\" pos=1/3\n"
                    "      cell #3 pos=2/3 actions=click\n"
                    "      cell #4 \"1\" pos=3/3 actions=setvalue\n"
                    "    row #34 selected pos=2/3 actions=click,select,scrollintoview\n"
                    "      cell #35 \"beta\" pos=1/3\n"
                    "      cell #36 checked pos=2/3 actions=click\n"
                    "      cell #37 \"2\" pos=3/3 actions=setvalue\n"
                    "    row #67 pos=3/3 actions=click,select,scrollintoview\n"
                    "      cell #68 \"gamma\" disabled pos=1/3\n"
                    "      cell #69 disabled pos=2/3\n"
                    "      cell #70 \"3\" disabled pos=3/3\n");
        // Select on a selected row keeps the selection (it only becomes current); on another row it selects that one
        UI_CHECK(ui.performA11yAction(table, A::Select, 0.f, 34) && sel->selectedRows() == 2 && sel->currentRow() == 1);
        UI_CHECK(ui.performA11yAction(table, A::Select, 0.f, 67) && sel->selectedRows() == 1 && sel->rowSelected(2));
        // a check cell: Click toggles it through the model; an editable cell takes SetValue as an edit
        UI_CHECK(ui.performA11yAction(table, A::Click, 0.f, 3) && fm.checks[0] == 1 && entry(ui, table, 3).checked);
        UI_CHECK(ui.performA11yAction(table, A::SetValue, 0.f, 4, "41") && fm.cells[0][2] == "41" && entry(ui, table, 4).name == "41");
        UI_CHECK(!ui.performA11yAction(table, A::SetValue, 0.f, 2, "x") && fm.cells[0][0] == "alpha"); // not editable
        UI_CHECK(!ui.performA11yAction(table, A::SetValue, 0.f, 70, "x") && fm.cells[2][2] == "3");   // disabled row
        UI_CHECK(!ui.performA11yAction(table, A::Expand, 0.f, 1));                                    // not a tree
        // a hidden column leaves the header and the cells; the others keep their ids
        w::setViewColumnHidden(ui, table, 1, true);
        UI_CHECK(entry(ui, table, 3).role == A11yRole::None && entry(ui, table, 4).posInSet == 2u && entry(ui, table, 4).setSize == 2u);
        UI_CHECK(entry(ui, table, 2147418114u).role == A11yRole::None && !ui.performA11yAction(table, A::Click, 0.f, 2147418114u));
        w::setViewColumnHidden(ui, table, 1, false);
        // the in-place editor is a real child node, named after its column
        UI_CHECK(w::viewEdit(ui, table, 1, 2));
        ui.update();
        const NodeHandle ed = w::viewEditor(ui, table);
        const A11ySnapshotNode e = entry(ui, ed);
        UI_CHECK(e.role == A11yRole::TextField && e.name == "Count" && e.valueText == "2" && e.focused && e.hasAction(A::SetValue));
        UI_CHECK(ui.performA11yAction(ed, A::SetValue, 0.f, 0, "22") && w::viewCommitEdit(ui, table) && fm.cells[1][2] == "22");
        // cell selection: the cells are what is selectable
        w::setViewSelectionMode(ui, table, w::SelectionMode::Extended, w::SelectionUnit::Cells);
        UI_CHECK(ui.performA11yAction(table, A::Select, 0.f, 35));
        const A11ySnapshotNode c = entry(ui, table, 35), r = entry(ui, table, 34);
        UI_CHECK(c.selectable && c.selected && c.hasAction(A::Select) && !r.selectable && !r.selected && !entry(ui, table, 37).selected);
        UI_CHECK(w::viewCurrentRow(ui, table) == 1 && w::viewCurrentColumn(ui, table) == 0u);
        // no selection at all: nothing is selectable
        w::setViewSelectionMode(ui, table, w::SelectionMode::None);
        UI_CHECK(!entry(ui, table, 34).selectable && !entry(ui, table, 35).selectable);
    }
    { // a wide table scrolls sideways: the view says so, a column header / cell scrolls into view
        Fixture f;
        UiContext& ui = f.ui;
        w::StringTableModel m("A\tB\tC\tD");
        for (int i = 0; i < 4; ++i) m.addRow("a\tb\tc\td");
        const NodeHandle table = w::tableView(ui, ui.root(), &m.model(), "wide");
        ui.setCss(table, "height", "120px");
        ui.setCss(table, "width", "260px");
        ui.update();
        const A11ySnapshotNode t = entry(ui, table);
        UI_CHECK(t.hasAction(A::ScrollLeft) && t.hasAction(A::ScrollRight) && !t.hasAction(A::ScrollDown));
        UI_CHECK(entry(ui, table, 2147418113u + 3u).role == A11yRole::None); // column D is scrolled out
        UI_CHECK(ui.performA11yAction(table, A::ScrollIntoView, 0.f, 1u + 4u) && w::viewScrollX(ui, table) > 0.f); // cell D of row 0
        UI_CHECK(entry(ui, table, 2147418113u + 3u).role == A11yRole::ColumnHeader);
        UI_CHECK(ui.performA11yAction(table, A::ScrollLeft) && ui.performA11yAction(table, A::ScrollIntoView, 0.f, 2147418113u));
        UI_CHECK(w::viewScrollX(ui, table) == 0.f);
    }
    { // tree model view: a tree of tree items keyed by item id (ids survive expanding above them)
        Fixture f;
        UiContext& ui = f.ui;
        w::StringTreeModel tm("Name\tKind");
        const w::ItemId scene = tm.add(w::kRootItem, "Scene\tgroup");
        const w::ItemId cam = tm.add(scene, "Camera\tcamera");
        const w::ItemId lights = tm.add(scene, "Lights\tgroup");
        tm.add(lights, "Sun\tlight");
        const w::ItemId assets = tm.add(w::kRootItem, "Assets\tgroup");
        tm.add(assets, "Textures\tfolder");
        tm.setEditable(true);
        const NodeHandle tree = w::treeModelView(ui, ui.root(), &tm.model(), "tree");
        ui.setCss(tree, "height", "200px");
        ui.setCss(tree, "width", "420px");
        ui.update();
        UI_CHECK(w::setTreeItemExpanded(ui, tree, scene, true));
        ui.setValue(tree, 1.f);
        const u32 idScene = 1u + u32(scene) * 33u, idCam = 1u + u32(cam) * 33u, idLights = 1u + u32(lights) * 33u, idAssets = 1u + u32(assets) * 33u;
        std::string want =
            "window\n"
            "  tree focusable actions=focus,click\n"
            "    row #2147418112\n"
            "      columnheader #2147418113 \"Name\" pos=1/2 actions=click\n"
            "      columnheader #2147418114 \"Kind\" pos=2/2 actions=click\n"
            "    treeitem #" + std::to_string(idScene) + " \"Scene\" expanded level=1 pos=1/4 actions=click,collapse,select,scrollintoview\n"
            "      cell #" + std::to_string(idScene + 1) + " \"Scene\" pos=1/2 actions=setvalue\n"
            "      cell #" + std::to_string(idScene + 2) + " \"group\" pos=2/2 actions=setvalue\n"
            "    treeitem #" + std::to_string(idCam) + " \"Camera\" selected level=2 pos=2/4 actions=click,select,scrollintoview\n"
            "      cell #" + std::to_string(idCam + 1) + " \"Camera\" pos=1/2 actions=setvalue\n"
            "      cell #" + std::to_string(idCam + 2) + " \"camera\" pos=2/2 actions=setvalue\n"
            "    treeitem #" + std::to_string(idLights) + " \"Lights\" collapsed level=2 pos=3/4 actions=click,expand,select,scrollintoview\n"
            "      cell #" + std::to_string(idLights + 1) + " \"Lights\" pos=1/2 actions=setvalue\n"
            "      cell #" + std::to_string(idLights + 2) + " \"group\" pos=2/2 actions=setvalue\n"
            "    treeitem #" + std::to_string(idAssets) + " \"Assets\" collapsed level=1 pos=4/4 actions=click,expand,select,scrollintoview\n"
            "      cell #" + std::to_string(idAssets + 1) + " \"Assets\" pos=1/2 actions=setvalue\n"
            "      cell #" + std::to_string(idAssets + 2) + " \"group\" pos=2/2 actions=setvalue\n";
        EXPECT_DUMP("tree model view", f.dump(), want.c_str());
        int toggles = 0;
        ui.connect(tree, SignalKind::Toggled, [&toggles](UiContext&, NodeHandle, const SignalArgs& a) { toggles += a.text == "expand"; });
        // Expand / Collapse drive the view; a leaf and an already expanded row refuse
        UI_CHECK(ui.performA11yAction(tree, A::Expand, 0.f, idLights) && w::treeItemExpanded(ui, tree, lights) && toggles == 1);
        UI_CHECK(!ui.performA11yAction(tree, A::Expand, 0.f, idLights) && !ui.performA11yAction(tree, A::Expand, 0.f, idCam));
        const A11ySnapshotNode a = entry(ui, tree, idAssets);
        UI_CHECK(a.role == A11yRole::TreeItem && a.posInSet == 5u && a.setSize == 5u && a.level == 1u); // same id, one row further down
        UI_CHECK(ui.performA11yAction(tree, A::Select, 0.f, idAssets) && ui.value(tree) == 4.f && entry(ui, tree, idAssets).selected);
        UI_CHECK(ui.performA11yAction(tree, A::Collapse, 0.f, idScene) && !w::treeItemExpanded(ui, tree, scene) && toggles == 2);
        UI_CHECK(entry(ui, tree, idCam).role == A11yRole::None && !ui.performA11yAction(tree, A::Select, 0.f, idCam)); // not shown
        UI_CHECK(entry(ui, tree, idAssets).posInSet == 2u && entry(ui, tree, idAssets).selected);
        // SetValue on a cell goes to the model
        UI_CHECK(ui.performA11yAction(tree, A::SetValue, 0.f, idAssets + 2, "library") && tm.cell(assets, 1) == "library");
        // multi-selection in a tree
        w::setViewSelectionMode(ui, tree, w::SelectionMode::Multi);
        UI_CHECK(ui.performA11yAction(tree, A::Click, 0.f, idScene));
        UI_CHECK(entry(ui, tree, idScene).selected && entry(ui, tree, idAssets).selected);
        UI_CHECK(ui.performA11yAction(tree, A::Click, 0.f, idScene) && !entry(ui, tree, idScene).selected); // a click toggles in Multi

        // a one-column tree is a plain tree of items, no cells
        Fixture g;
        w::StringTreeModel one("Name");
        const w::ItemId root = one.add(w::kRootItem, "Root");
        one.add(root, "Leaf");
        const NodeHandle t1 = w::treeModelView(g.ui, g.ui.root(), &one.model(), "t1");
        g.ui.setCss(t1, "height", "100px");
        g.ui.update();
        const std::string want1 = "window\n  tree focusable\n    treeitem #" + std::to_string(1u + u32(root) * 33u) + " \"Root\" collapsed level=1 pos=1/1\n";
        EXPECT_DUMP("one-column tree", g.dump(false), want1.c_str());
    }
    { // svg image, icon buttons, an action-style menu (shortcut texts, disabled / checked items)
        Fixture f;
        UiContext& ui = f.ui;
        const NodeHandle img = w::svgImage(ui, ui.root(), w::iconHandle(Icon::Folder), 24.f, 24.f, "img");
        ui.setA11y(img, A11yRole::Image, "Project folder");
        w::iconButton(ui, ui.root(), Icon::Save, {}, "save");
        const NodeHandle b = w::button(ui, ui.root(), "Open", "open");
        w::setIcon(ui, b, Icon::Folder);
        const NodeHandle owner = w::button(ui, ui.root(), "Edit", "edit");
        ui.update();
        w::popupMenu(ui, owner, "Undo\tCtrl+Z\n\x01Redo\tCtrl+Y\n-\n\x02Snap to grid\tCtrl+G\nMore\n\tOne\n\tTwo", {40.f, 200.f});
        EXPECT_DUMP("svg / icons / action menu", f.dump(),
                    "window\n"
                    "  image \"Project folder\"\n"
                    "  button \"save\" focusable actions=focus,click\n"
                    "  button \"Open\" focusable actions=focus,click\n"
                    "  button \"Edit\" focusable actions=focus,click\n"
                    "  menu\n"
                    "    menuitem \"Undo\" focusable focused actions=focus,click\n"
                    "    menuitem \"Redo\" focusable disabled actions=focus\n"
                    "    separator\n"
                    "    menuitem \"Snap to grid\" focusable checked actions=focus,click\n"
                    "    menuitem \"More\" focusable collapsed actions=focus,click\n");
    }
}

// ---- grid view ------------------------------------------------------------------------------------------------------
void gateGrid() {
    Fixture f;
    UiContext& ui = f.ui;
    w::StringTableModel m("name");
    for (int i = 0; i < 200; ++i) m.addRow("tile " + std::to_string(i));
    const NodeHandle grid = w::gridView(ui, ui.root(), &m.model(), 100.f, 80.f, "grid");
    ui.setA11y(grid, A11yRole::List, "Thumbnails");
    ui.setCss(grid, "width", "320px");
    ui.setCss(grid, "height", "170px");
    ui.update();
    UI_CHECK_EQ(w::gridColumns(ui, grid), 3u);
    EXPECT_DUMP("grid", f.dump(false),
                "window\n"
                "  list \"Thumbnails\" focusable\n"
                "    listitem #1 \"tile 0\" pos=1/200\n"
                "    listitem #2 \"tile 1\" pos=2/200\n"
                "    listitem #3 \"tile 2\" pos=3/200\n"
                "    listitem #4 \"tile 3\" pos=4/200\n"
                "    listitem #5 \"tile 4\" pos=5/200\n"
                "    listitem #6 \"tile 5\" pos=6/200\n");
    const A11ySnapshotNode t4 = entry(ui, grid, 5); // second row, second column
    UI_CHECK(t4.bounds.w == 100.f && t4.bounds.h == 80.f && t4.bounds.x == ui.contentRect(grid).x + 100.f &&
             t4.bounds.y == ui.contentRect(grid).y + 80.f);
    UI_CHECK(entry(ui, grid).hasAction(A::ScrollDown) && t4.hasAction(A::Select));
    UI_CHECK(ui.performA11yAction(grid, A::Select, 0.f, 5) && ui.value(grid) == 4.f);
    UI_CHECK(ui.performA11yAction(grid, A::ScrollIntoView, 0.f, 151));
    ui.update();
    UI_CHECK(w::gridFirstVisibleRow(ui, grid) == 49u && entry(ui, grid, 151).name == "tile 150");
    UI_CHECK(ui.performA11yAction(grid, A::ScrollUp));
    ui.update();
    UI_CHECK_EQ(w::gridFirstVisibleRow(ui, grid), 48u);
    UI_CHECK(!ui.performA11yAction(grid, A::Select, 0.f, 201));
}

// ---- tabs: tab bar, tab widget, dock tab strip ----------------------------------------------------------------------
void gateTabs() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle tabs = w::tabWidget(ui, ui.root(), "General\nRender\nAudio", "tabs");
    w::label(ui, w::tabPage(ui, tabs, 0), "general page");
    w::label(ui, w::tabPage(ui, tabs, 1), "render page");
    const NodeHandle bar = w::tabWidgetBar(ui, tabs);
    EXPECT_DUMP("tab widget", f.dump(),
                "window\n"
                "  tablist focusable actions=focus,click\n"
                "    tab #1 \"General\" selected pos=1/3 actions=click,select\n"
                "    tab #2 \"Render\" pos=2/3 actions=click,select\n"
                "    tab #3 \"Audio\" pos=3/3 actions=click,select\n"
                "  tabpanel \"General\"\n"
                "    text \"general page\"\n");
    UI_CHECK(ui.performA11yAction(bar, A::Select, 0.f, 2) && ui.value(bar) == 1.f);
    const std::string d = f.dump(false);
    UI_CHECK(d.find("tab #2 \"Render\" selected") != std::string::npos && d.find("tabpanel \"Render\"") != std::string::npos);
    UI_CHECK(d.find("general page") == std::string::npos && !ui.performA11yAction(bar, A::Select, 0.f, 4));
    const A11ySnapshotNode t2 = entry(ui, bar, 2), t3 = entry(ui, bar, 3);
    UI_CHECK(t3.bounds.x == t2.bounds.right() && t2.bounds.w > 20.f);

    // docks tabbed together: the strip is a tab list of the open docks
    Fixture g;
    const NodeHandle area = w::dockArea(g.ui, g.ui.root(), "area");
    const NodeHandle outline = w::dockWidget(g.ui, area, "Outline", w::DockSide::Left, "outline");
    const NodeHandle history = w::dockWidget(g.ui, area, "History", w::DockSide::Left, "history");
    w::label(g.ui, w::dockBody(g.ui, outline), "outline body");
    w::label(g.ui, w::dockBody(g.ui, history), "history body");
    w::dockTabify(g.ui, history, outline);
    g.ui.update();
    const NodeHandle strip = w::dockTabBar(g.ui, outline);
    char want[512];
    std::snprintf(want, sizeof want,
                  "window\n"
                  "  tablist focusable\n"
                  "    tab #%u \"Outline\" selected pos=1/2\n"
                  "    tab #%u \"History\" pos=2/2\n"
                  "  group \"Outline\"\n"
                  "    text \"outline body\"\n"
                  "  separator focusable\n",
                  outline.index + 1, history.index + 1);
    EXPECT_DUMP("dock tabs", g.dump(false), want);
    UI_CHECK(g.ui.performA11yAction(strip, A::Select, 0.f, history.index + 1) && w::dockCurrent(g.ui, outline) == history);
    UI_CHECK(g.dump(false).find("history body") != std::string::npos);
    UI_CHECK(!g.ui.performA11yAction(strip, A::Select, 0.f, area.index + 1));
}

// ---- text widgets ---------------------------------------------------------------------------------------------------
void gateText() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle ed = w::textEdit(ui, ui.root(), "first line", "notes");
    const NodeHandle code = w::codeEdit(ui, ui.root(), "local x = 1", w::CodeLanguage::Lua, "code");
    const NodeHandle rt = w::richText(ui, ui.root(), "See the <a href=\"docs/ui\">docs</a> or <a href=\"https://example.org\">the site</a>.", "rich");
    EXPECT_DUMP("text", f.dump(),
                "window\n"
                "  textfield \"Text\" focusable value=\"first line\" actions=focus,setvalue\n"
                "  textfield \"Code\" focusable value=\"local x = 1\" actions=focus,setvalue\n"
                "  text \"See the docs or the site.\" actions=click\n"
                "    link #1 \"docs/ui\" actions=click\n"
                "    link #2 \"https://example.org\" actions=click\n");
    int edits = 0;
    ui.connect(ed, SignalKind::TextChanged, [&edits](UiContext&, NodeHandle, const SignalArgs&) { ++edits; });
    UI_CHECK(ui.performA11yAction(ed, A::SetValue, 0.f, 0, "replaced\ntwo lines") && w::textEditText(ui, ed) == "replaced\ntwo lines");
    UI_CHECK(edits == 1 && entry(ui, ed).valueText == "replaced\ntwo lines");
    UI_CHECK(ui.performA11yAction(code, A::SetValue, 0.f, 0, "return 2") && w::codeEditText(ui, code) == "return 2");
    UI_CHECK_EQ(w::codeEditLineCount(ui, code), 1u);
    // a link's Click reports the link like a pointer click on it
    int link = -1;
    std::string target;
    ui.connect(rt, SignalKind::Submitted, [&link, &target](UiContext&, NodeHandle, const SignalArgs& a) { link = int(a.value); target = std::string(a.text); });
    UI_CHECK(ui.performA11yAction(rt, A::Click, 0.f, 2) && link == 1 && target == "https://example.org");
    UI_CHECK(!ui.performA11yAction(rt, A::Click, 0.f, 3));
    const A11ySnapshotNode l1 = entry(ui, rt, 1);
    UI_CHECK(l1.bounds.w > 10.f && l1.bounds.y >= ui.rect(rt).y && l1.bounds.bottom() <= ui.rect(rt).bottom() + 1.f);
}

// ---- header, calendar, date-time, colour picker, scrolling ----------------------------------------------------------
void gatePro() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle hdr = w::headerView(ui, ui.root(), "Name\nSize\nType", 100.f, "hdr");
    const NodeHandle cal = w::calendar(ui, ui.root(), 2026, 11, 12, "cal");
    const NodeHandle dt = w::dateTimeEdit(ui, ui.root(), 2026, 11, 12, 9, 30, "when");
    const NodeHandle cp = w::colorPicker(ui, ui.root(), Color::rgb(0xFF8800), "tint");
    ui.update();
    const std::string d = f.dump();
    UI_CHECK(d.find("  row actions=click\n"
                    "    columnheader #1 \"Name\" pos=1/3 actions=click\n"
                    "    columnheader #2 \"Size\" pos=2/3 actions=click\n"
                    "    columnheader #3 \"Type\" pos=3/3 actions=click\n") != std::string::npos);
    UI_CHECK(d.find("group \"Calendar\" focusable value=\"2026-11-12\" actions=focus,click,increment,decrement,setvalue\n") != std::string::npos);
    UI_CHECK(d.find("spinbox \"Date and time\" focusable value=\"2026-11-12 09:30\"") != std::string::npos);
    UI_CHECK(d.find("group \"Colour\" focusable value=\"#FF8800FF\"") != std::string::npos || d.find("value=\"#ff8800ff\"") != std::string::npos);
    if (g_print) std::printf("==== pro ====\n%s", d.c_str());
    // header: Click sorts by the section
    UI_CHECK(ui.performA11yAction(hdr, A::Click, 0.f, 2) && w::headerSortSection(ui, hdr) == 1);
    UI_CHECK(entry(ui, hdr, 2).valueText == "ascending" && entry(ui, hdr, 2).bounds.x == ui.contentRect(hdr).x + 100.f);
    bool asc = true;
    UI_CHECK(ui.performA11yAction(hdr, A::Click, 0.f, 2) && w::headerSortSection(ui, hdr, &asc) == 1 && !asc);
    // calendar: a day forward / back, and a date by value
    UI_CHECK(ui.performA11yAction(cal, A::Increment) && entry(ui, cal).valueText == "2026-11-13");
    UI_CHECK(ui.performA11yAction(cal, A::SetValue, f32(w::daysFromCivil(2027, 1, 1))) && entry(ui, cal).valueText == "2027-01-01");
    UI_CHECK(ui.performA11yAction(cal, A::Decrement) && entry(ui, cal).valueText == "2026-12-31");
    UI_CHECK(ui.performA11yAction(dt, A::Increment) && entry(ui, dt).valueText == "2026-11-13 09:30");
    (void)cp;

    // scroll containers: Scroll* move by half a page, ScrollIntoView reveals a node in nested scrolling ancestors
    Fixture g;
    const NodeHandle area = w::scrollArea(g.ui, g.ui.root(), "area");
    g.ui.setCss(area, "height", "200px");
    NodeHandle last;
    for (int i = 0; i < 60; ++i) last = w::button(g.ui, area, "Row " + std::to_string(i));
    g.ui.update();
    UI_CHECK(entry(g.ui, area).role == A11yRole::ScrollArea && entry(g.ui, area).hasAction(A::ScrollDown) && !entry(g.ui, area).hasAction(A::ScrollLeft));
    UI_CHECK(!g.ui.performA11yAction(area, A::ScrollUp)); // already at the top
    UI_CHECK(g.ui.performA11yAction(area, A::ScrollDown));
    g.ui.update();
    UI_CHECK(std::abs(g.ui.scroll(area).y - g.ui.contentRect(area).h * 0.5f) < 1.f);
    UI_CHECK(g.ui.rect(last).y > g.ui.rect(area).bottom());
    UI_CHECK(g.ui.performA11yAction(last, A::ScrollIntoView));
    g.ui.update();
    UI_CHECK(g.ui.rect(last).bottom() <= g.ui.contentRect(area).bottom() + 0.5f && g.ui.rect(last).y >= g.ui.contentRect(area).y);
    // and now a click on it is a real pointer click (it is on screen)
    int clicks = 0;
    g.ui.connect(last, SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
    UI_CHECK(g.ui.performA11yAction(last, A::Click) && clicks == 1 && g.ui.focused() == last);
    // a node scrolled out of view is still clickable through its action (no pointer position needed)
    g.ui.scrollTo(area, {0.f, 0.f});
    g.ui.update();
    UI_CHECK(g.ui.performA11yAction(last, A::Click) && clicks == 2);
}

// ---- bridge: ids of virtual children, diffing, actions by id --------------------------------------------------------
void gateBridge() {
    Fixture f;
    UiContext& ui = f.ui;
    std::string items;
    for (int i = 0; i < 40; ++i) items += (i ? "\n" : "") + std::string("Item ") + std::to_string(i);
    const NodeHandle list = w::listView(ui, ui.root(), items, -1, "list");
    ui.setCss(list, "height", "110px");
    const NodeHandle tree = w::treeView(ui, ui.root(), "Root\n\tLeaf", "tree");
    ui.update();
    a11y::Bridge b(ui);
    const u32 firstSync = b.sync(); // sequenced before nodes(): operand order of == is unspecified (MSVC read size() first)
    UI_CHECK(firstSync == u32(b.nodes().size()) && b.sync() == 0u);
    // ids: real nodes keep the old scheme, virtual children get distinct ids that resolve back
    const u64 listId = a11y::idOf(list), row2 = a11y::idOf(list, 3);
    UI_CHECK(!a11y::isVirtual(listId) && a11y::isVirtual(row2) && row2 != a11y::idOf(list, 4) && row2 != a11y::idOf(tree, 3));
    NodeHandle n;
    u32 child = 9;
    UI_CHECK(b.resolve(row2, n, child) && n == list && child == 3u);
    UI_CHECK(b.resolve(listId, n, child) && n == list && child == 0u && !b.resolve(0, n, child));
    const u32 at = b.indexOf(row2);
    UI_CHECK(at != ~0u && b.nodes()[at].name == "Item 2" && b.nodes()[b.nodes()[at].parent].node == list);
    UI_CHECK_EQ(b.children(b.indexOf(listId)).size(), std::size_t(5));
    // every id in the snapshot is unique and non-zero
    for (u32 i = 0; i < b.nodes().size(); ++i)
        for (u32 k = i + 1; k < b.nodes().size(); ++k) UI_CHECK(a11y::idOf(b.nodes()[i]) != a11y::idOf(b.nodes()[k]) && a11y::idOf(b.nodes()[i]) != 0);
    // actions by id; the diff sees exactly what changed
    UI_CHECK(b.perform({a11y::Action::Select, row2, 0.f, {}}) && ui.value(list) == 2.f);
    ui.update();
    UI_CHECK_EQ(b.sync(), 2u); // the row became selected, the list's value changed
    UI_CHECK(b.perform({a11y::Action::ScrollDown, listId, 0.f, {}}));
    ui.update();
    UI_CHECK_EQ(b.sync(), 8u); // three rows left the window, three entered, the two that stayed moved up (same ids)
    UI_CHECK(b.indexOf(a11y::idOf(list, 4)) != ~0u && b.indexOf(row2) == ~0u);
    UI_CHECK(b.perform({a11y::Action::Collapse, a11y::idOf(tree, 1), 0.f, {}}) && !w::treeExpanded(ui, tree, 0));
    ui.update();
    UI_CHECK_EQ(b.sync(), 2u); // the root row changed state, the leaf row went away
    // queued actions (adapter threads) drain on the UI thread; ids of dead owners are refused
    b.enqueue({a11y::Action::Expand, a11y::idOf(tree, 1), 0.f, {}});
    b.enqueue({a11y::Action::Select, a11y::idOf(list, 40), 0.f, {}});
    UI_CHECK(b.drain() == 2u && w::treeExpanded(ui, tree, 0) && ui.value(list) == 39.f);
    ui.destroy(tree);
    ui.update();
    UI_CHECK(!b.perform({a11y::Action::Expand, a11y::idOf(tree, 1), 0.f, {}}));
    // a text value through the bridge
    const NodeHandle le = w::lineEdit(ui, ui.root(), "", "name");
    ui.update();
    UI_CHECK(b.perform({a11y::Action::SetValue, a11y::idOf(le), 0.f, "typed by AT"}) && ui.text(le) == "typed by AT");
}
} // namespace

int main(int argc, char** argv) {
    struct Gate { const char* name; void (*fn)(); };
    static const Gate kGates[] = {{"controls", &gateControls}, {"menus", &gateMenus}, {"dialogs_live", &gateDialogsLive},
                                  {"list", &gateList}, {"tree", &gateTree}, {"table", &gateTable}, {"modelview", &gateModelView}, {"grid", &gateGrid},
                                  {"tabs", &gateTabs}, {"text", &gateText}, {"pro", &gatePro}, {"bridge", &gateBridge}};
    const char* only = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--gate=", 7) == 0) only = argv[i] + 7;
        else if (std::strcmp(argv[i], "--print") == 0) g_print = true;
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
    return finish("fuse_ui_a11y_families");
}
