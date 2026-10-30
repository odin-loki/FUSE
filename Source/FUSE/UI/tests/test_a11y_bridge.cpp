// fuse_ui_a11y gate: snapshot / diff / action mapping of the platform accessibility bridge, plus the AccessKit
// tree update (when built with FUSE_UI_ACCESSKIT_DIR) and the AT-SPI adapter lifecycle (no AT bus needed: the
// adapter stays inactive and update() is a no-op).
#include "test_util.hpp"
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <vector>

#include "fuse/ui/a11y/a11y.hpp"

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;
namespace a = fuse::ui::a11y;

int main() {
    UiContext ui{{.viewport = {640, 400}, .font = &font()}};
    w::install(ui);
    const NodeHandle panel = w::panel(ui, ui.root());
    const NodeHandle ok = w::button(ui, panel, "Save", "save");
    const NodeHandle cb = w::checkBox(ui, panel, "Autosave", false, "autosave");
    const NodeHandle sl = w::slider(ui, panel, 0.f, 100.f, 40.f, 0.f, "volume");
    const NodeHandle le = w::lineEdit(ui, panel, "hello", "name");
    const NodeHandle pin = w::lineEdit(ui, panel, "s3cr3t", "pin");
    w::setLineEditOptions(ui, pin, {.echo = w::EchoMode::Password});
    const NodeHandle city = w::lineEdit(ui, panel, "", "city");
    w::setCompleter(ui, city, {.items = "Sydney\nSingapore"});
    ui.setA11y(sl, A11yRole::Slider, "Volume");
    ui.update(1.f / 60.f);

    a::Bridge b(ui);
    const u32 first = b.sync();
    UI_CHECK(first == b.nodes().size() && first >= 5u);
    UI_CHECK(b.nodes()[0].parent == ~0u && b.nodes()[0].role == A11yRole::Window);
    UI_CHECK_EQ(b.sync(), 0u); // nothing changed
    // ids round-trip and are never 0
    for (const auto& n : b.nodes()) UI_CHECK(a::idOf(n.node) != 0 && a::nodeOf(a::idOf(n.node)) == n.node);
    auto find = [&](NodeHandle h) -> const A11ySnapshotNode* {
        for (const auto& n : b.nodes())
            if (n.node == h) return &n;
        return nullptr;
    };
    UI_CHECK(find(ok) && find(ok)->role == A11yRole::Button && find(ok)->name == "Save" && find(ok)->focusable);
    UI_CHECK(find(sl) && find(sl)->name == "Volume" && find(sl)->hi == 100.f && find(sl)->value == 40.f);
    UI_CHECK(find(cb) && !find(cb)->checked);
    UI_CHECK(find(pin) && find(pin)->protectedText && find(pin)->name.find("s3cr3t") == std::string::npos);
    UI_CHECK(find(le) && !find(le)->protectedText);
    // parents precede children; every child is listed under its parent
    for (u32 i = 1; i < b.nodes().size(); ++i) {
        const u32 p = b.nodes()[i].parent;
        UI_CHECK(p < i);
        bool listed = false;
        for (u32 c : b.children(p)) listed |= c == i;
        UI_CHECK(listed);
    }

    // actions: Click toggles the checkbox (one changed node), Increment steps the slider, Focus moves focus
    UI_CHECK(b.perform({a::Action::Click, a::idOf(cb)}));
    ui.update(1.f / 60.f);
    UI_CHECK(ui.checked(cb));
    UI_CHECK(b.sync() >= 1u && find(cb)->checked);
    UI_CHECK(b.perform({a::Action::Increment, a::idOf(sl)}));
    UI_CHECK(ui.value(sl) == 41.f);
    UI_CHECK(b.perform({a::Action::SetValue, a::idOf(sl), 75.f}));
    UI_CHECK(ui.value(sl) == 75.f);
    // queued from "another thread", applied on drain
    b.enqueue({a::Action::Focus, a::idOf(le)});
    UI_CHECK(ui.focused() != le);
    UI_CHECK_EQ(b.drain(), 1u);
    UI_CHECK(ui.focused() == le);
    ui.update(1.f / 60.f);
    b.sync();
    UI_CHECK(a::idOf(le) == b.focusId());
    // dead nodes are rejected; removal shows up in the diff
    const u64 gone = a::idOf(ok);
    ui.destroy(ok);
    ui.update(1.f / 60.f);
    UI_CHECK(b.sync() >= 1u);
    UI_CHECK(!b.perform({a::Action::Click, gone}));

    // virtual parts: table / tree views expose Grid > Row > ColumnHeader / GridCell with unique ids; AT click on a
    // cell selects its row; the current cell follows the view's focus
    static w::StringTableModel tm("Name\tSize");
    tm.addRow("alpha\t1"); tm.addRow("beta\t2"); tm.addRow("gamma\t3");
    const NodeHandle tbl = w::tableView(ui, panel, &tm.model(), "tbl");
    const NodeHandle tree = w::treeView(ui, panel, "root\n\tleaf a\n\tleaf b\nother", "tree");
    ui.update(1.f / 60.f);
    b.sync();
    {
        std::vector<u64> ids;
        u32 cells = 0, heads = 0, treeRows = 0;
        u64 beta = 0, leafB = 0;
        bool sizes = false, levels = false, expanded = false;
        for (const auto& n : b.nodes()) {
            ids.push_back(a::idOf(n));
            if (n.part) UI_CHECK((n.node == tbl || n.node == tree) && !a::nodeOf(a::idOf(n)).valid());
            if (!n.part && n.node == tbl) sizes = n.role == A11yRole::Grid && n.rowCount == 4u && n.colCount == 2u;
            if (n.node == tbl && n.role == A11yRole::GridCell) ++cells;
            if (n.node == tbl && n.role == A11yRole::ColumnHeader) heads += n.name == "Name" || n.name == "Size";
            if (n.node == tbl && n.role == A11yRole::GridCell && n.name == "beta") { beta = a::idOf(n); UI_CHECK(n.rowIndex == 2 && n.colIndex == 0); }
            if (n.node == tree && n.role == A11yRole::Row) {
                ++treeRows;
                if (n.name == "leaf a") levels = n.level == 2u && n.state.expanded == -1;
                if (n.name == "root") expanded = n.level == 1u && n.state.expanded == 1;
            }
            if (n.node == tree && n.role == A11yRole::GridCell && n.name == "leaf b") leafB = a::idOf(n);
        }
        std::sort(ids.begin(), ids.end());
        UI_CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
        UI_CHECK(sizes && cells == 6u && heads == 2u && beta != 0);
        UI_CHECK(treeRows == 4u && levels && expanded && leafB != 0);
        UI_CHECK(b.perform({a::Action::Click, beta}));
        UI_CHECK(ui.value(tbl) == 1.f);
        UI_CHECK(b.perform({a::Action::Click, leafB}));
        UI_CHECK(ui.value(tree) == 2.f);
        ui.setFocus(tbl);
        ui.update(1.f / 60.f);
        b.sync();
        bool cellFocused = false;
        for (const auto& n : b.nodes())
            if (a::idOf(n) == b.focusId()) cellFocused = n.role == A11yRole::GridCell && n.name == "beta" && n.state.selected;
        UI_CHECK(cellFocused);
        w::setTreeExpanded(ui, tree, 0, false);
        ui.update(1.f / 60.f);
        b.sync();
        treeRows = 0;
        for (const auto& n : b.nodes()) {
            if (n.node == tree && n.role == A11yRole::Row) ++treeRows;
            if (n.node == tree && n.role == A11yRole::Row && n.name == "root") UI_CHECK(n.state.expanded == 0);
        }
        UI_CHECK(treeRows == 2u);
    }

#if FUSE_UI_HAVE_ACCESSKIT
    UI_CHECK(a::accessKitAvailable());
    void* up = a::buildAccessKitUpdate(b);
    const std::string dbg = a::accessKitDebug(up);
    UI_CHECK(dbg.find("Autosave") != std::string::npos);
    UI_CHECK(dbg.find("Volume") != std::string::npos);
    UI_CHECK(dbg.find("CheckBox") != std::string::npos && dbg.find("Slider") != std::string::npos);
    UI_CHECK(dbg.find("FUSE UI") != std::string::npos);
    // password field: PasswordInput role (UIA IsPassword, AT-SPI password text), secret absent from the tree
    UI_CHECK(dbg.find("PasswordInput") != std::string::npos && dbg.find("s3cr3t") == std::string::npos);
    // table / tree grids: Grid / Row / ColumnHeader / GridCell with table indices and tree levels
    UI_CHECK(dbg.find("Grid") != std::string::npos && dbg.find("GridCell") != std::string::npos && dbg.find("ColumnHeader") != std::string::npos);
    UI_CHECK(dbg.find("row_index") != std::string::npos && dbg.find("column_count") != std::string::npos && dbg.find("level") != std::string::npos);
    a::freeAccessKitUpdate(up);
    { // completer: autoComplete List, expanded, controls + activeDescendant -> the highlighted ListItem (selected)
        const NodeHandle prevFocus = ui.focused();
        ui.setFocus(city);
        ui.textInput("s");
        ui.keyDown(Key::Down);
        ui.update(1.f / 60.f);
        b.sync();
        void* up2 = a::buildAccessKitUpdate(b);
        const std::string d2 = a::accessKitDebug(up2);
        if (std::getenv("FUSE_AK_DUMP")) std::fprintf(stderr, "%s\n", d2.c_str());
        if (d2.find("auto_complete") == std::string::npos) std::fprintf(stderr, "%s\n", d2.c_str());
        UI_CHECK(d2.find("auto_complete: List") != std::string::npos);
        UI_CHECK(d2.find("expanded: true") != std::string::npos && d2.find("active_descendant: #") != std::string::npos);
        UI_CHECK(d2.find("controls: [#") != std::string::npos && d2.find("selected: true") != std::string::npos && d2.find("selected: false") != std::string::npos);
        if (d2.find("active_descendant") == std::string::npos) std::fprintf(stderr, "%s\n", d2.c_str());
        a::freeAccessKitUpdate(up2);
        ui.keyDown(Key::Escape);
        ui.setFocus(prevFocus);
        ui.update(1.f / 60.f);
        b.sync();
    }
    up = a::buildAccessKitUpdate(b);
    a::freeAccessKitUpdate(up);
#if defined(__linux__)
    {
        a::UnixAdapter ad(b);
        UI_CHECK(ad.valid());
        ui.setValue(sl, 10.f, true);
        ui.update(1.f / 60.f);
        ad.update(); // no AT listening: inactive, nothing pushed, no crash
        ad.setWindowFocused(true);
    }
    {
        a::PlatformAdapter pa(b, nullptr);
        UI_CHECK(pa.valid() && std::string(a::PlatformAdapter::backendName()) == "atspi");
        ui.setValue(sl, 20.f, true);
        ui.update(1.f / 60.f);
        pa.update();
        pa.setWindowFocused(false);
    }
#endif
    std::printf("a11y: AccessKit backend linked (%zu-byte debug dump)\n", dbg.size());
#else
    UI_CHECK(!a::accessKitAvailable());
    {
        a::PlatformAdapter pa(b, nullptr);
        UI_CHECK(!pa.valid() && std::string(a::PlatformAdapter::backendName()) == "none");
        pa.update();
    }
    std::printf("a11y: built without AccessKit (FUSE_UI_ACCESSKIT_DIR unset)\n");
#endif
    return finish("fuse_ui_a11y_bridge");
}
