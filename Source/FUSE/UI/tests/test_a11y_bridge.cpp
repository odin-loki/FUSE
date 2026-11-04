// fuse_ui_a11y gate: snapshot / diff / action mapping of the platform accessibility bridge, plus the AccessKit
// tree update (when built with FUSE_UI_ACCESSKIT_DIR) and the AT-SPI adapter lifecycle (no AT bus needed: the
// adapter stays inactive and update() is a no-op).
#include "test_util.hpp"

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
    // extras: text field value, SetText; menu kinds and Expand / Collapse of a menu bar title
    auto extra = [&](NodeHandle h) -> const a::NodeExtra* {
        for (u32 i = 0; i < b.nodes().size(); ++i)
            if (b.nodes()[i].node == h) return &b.extras()[i];
        return nullptr;
    };
    UI_CHECK(b.extras().size() == b.nodes().size());
    UI_CHECK(extra(le) && extra(le)->text == "hello" && !extra(le)->labelled);
    UI_CHECK(b.perform({a::Action::SetText, a::idOf(le), 0.f, "typed"}));
    UI_CHECK(ui.text(le) == "typed" && ui.editBuffer(le).text() == "typed");
    const NodeHandle bar = w::menuBar(ui, ui.root());
    const NodeHandle file = w::menuBarMenu(ui, bar, "File", "Open\nQuit");
    ui.update(1.f / 60.f);
    UI_CHECK(b.sync() >= 2u);
    UI_CHECK(extra(bar) && extra(bar)->kind == a::Kind::MenuBar);
    UI_CHECK(extra(file) && extra(file)->kind == a::Kind::MenuItem && extra(file)->expanded == 0);
    UI_CHECK(find(file) && find(file)->name == "File");
    UI_CHECK(b.perform({a::Action::Expand, a::idOf(file)}));
    ui.update(1.f / 60.f);
    b.sync();
    UI_CHECK(extra(file) && extra(file)->expanded == 1);
    bool menuItem = false;
    for (u32 i = 0; i < b.nodes().size(); ++i) menuItem |= b.extras()[i].kind == a::Kind::MenuItem && b.nodes()[i].name == "Open";
    UI_CHECK(menuItem);
    UI_CHECK(b.perform({a::Action::Collapse, a::idOf(file)}));
    ui.update(1.f / 60.f);
    ui.update(1.f / 60.f);
    b.sync();
    UI_CHECK(extra(file) && extra(file)->expanded == 0);
    UI_CHECK(!b.perform({a::Action::Expand, a::idOf(cb)})); // not expandable

    // dead nodes are rejected; removal shows up in the diff
    const u64 gone = a::idOf(ok);
    ui.destroy(ok);
    ui.update(1.f / 60.f);
    UI_CHECK(b.sync() >= 1u);
    UI_CHECK(!b.perform({a::Action::Click, gone}));

#if FUSE_UI_HAVE_ACCESSKIT
    UI_CHECK(a::accessKitAvailable());
    void* up = a::buildAccessKitUpdate(b);
    const std::string dbg = a::accessKitDebug(up);
    UI_CHECK(dbg.find("Autosave") != std::string::npos);
    UI_CHECK(dbg.find("Volume") != std::string::npos);
    UI_CHECK(dbg.find("CheckBox") != std::string::npos && dbg.find("Slider") != std::string::npos);
    UI_CHECK(dbg.find("FUSE UI") != std::string::npos);
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
#endif
    std::printf("a11y: AccessKit backend linked (%zu-byte debug dump)\n", dbg.size());
#else
    UI_CHECK(!a::accessKitAvailable());
    std::printf("a11y: built without AccessKit (FUSE_UI_ACCESSKIT_DIR unset)\n");
#endif
    return finish("fuse_ui_a11y_bridge");
}
