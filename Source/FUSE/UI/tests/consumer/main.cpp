// Out-of-tree consumer of the installed FUSE UI package: builds a small UI from a document, clicks through it and
// rasterises a frame with the headless application shell. Exit code 0 = everything worked.
#include <fuse/app/app.hpp>
#include <fuse/app/services.hpp>
#include <fuse/text/font_family.hpp>
#include <fuse/text/i18n.hpp>
#include <fuse/ui/actions.hpp>
#include <fuse/ui/doc/document.hpp>
#include <fuse/ui/doc/hot_reload.hpp>
#include <fuse/ui/icons.hpp>
#include <fuse/ui/painter.hpp>
#include <fuse/ui/path.hpp>
#include <fuse/ui/svg.hpp>
#include <fuse/ui/ui.hpp>
#include <fuse/ui/widgets/interaction.hpp>
#include <fuse/ui/widgets/modelview.hpp>
#include <fuse/ui/widgets/widgets.hpp>

#include <cstdio>

int main(int argc, char** argv) {
    fuse::app::Application app{{.name = "fuse_ui_consumer", .argc = argc, .argv = argv, .loadSettings = false}};
    auto& window = app.createWindow({.title = "consumer", .width = 320, .height = 200});
    fuse::ui::doc::Model model;
    model.set("clicks", 0.0);
    const char* doc = R"({ "fuseui": 1, "root": { "class": "groupbox", "id": "box", "text": "Installed", "children": [
        { "class": "label", "id": "count", "text": "clicked {clicks:0} times" },
        { "class": "button", "id": "go", "text": "Click", "icon": "check", "on": { "clicked": "click" } },
        { "class": "svgimage", "id": "logo", "icon": "folder", "style": { "width": 24, "height": 24 } },
        { "class": "treemodelview", "id": "tree", "columns": ["Name", "Kind"], "items": ["Root\tgroup", "\tLeaf\titem"], "expanded": true } ] } })";
    if (auto loaded = window.loadText(doc, &model); !loaded) return app.fail(loaded.error());
    window.document()->onAction([&](std::string_view action, fuse::ui::NodeHandle, const fuse::ui::SignalArgs&) {
        if (action == "click") model.set("clicks", model.number("clicks") + 1.0);
    });
    app.step(0.f);
    fuse::ui::UiContext& ui = window.ui();
    const fuse::ui::Rect r = ui.rect(ui.find("go"));
    window.click({r.x + r.w * 0.5f, r.y + r.h * 0.5f});
    window.click({r.x + r.w * 0.5f, r.y + r.h * 0.5f});
    app.step(0.f);
    window.document()->refresh();
    app.step(0.f);
    const fuse::ui::Image& img = window.rasterize({});
    bool ok = model.number("clicks") == 2.0 && ui.text(ui.find("count")) == "clicked 2 times" && img.width == 320 &&
              ui.drawList().size() > 4;
    // the libraries behind the newer headers are in the install tree too: model / view, actions, vector drawing,
    // font families, plural rules, the application services
    namespace w = fuse::ui::widgets;
    ok = ok && w::viewRowCount(ui, ui.find("tree")) == 2u && w::treeRowExpanded(ui, ui.find("tree"), 0);
    ok = ok && w::iconOf(ui, ui.find("go")).valid() && w::svgGet(w::svgImageOf(ui, ui.find("logo"))) != nullptr;
    fuse::ui::ActionRegistry actions(ui);
    int saved = 0;
    const fuse::ui::ActionHandle save = actions.add({.id = "file.save", .text = "Save", .shortcut = fuse::ui::primaryShortcut(fuse::ui::Key::S)},
                                                    [&saved](fuse::ui::UiContext&, fuse::ui::ActionHandle, bool) { ++saved; });
    ok = ok && actions.trigger(save) && saved == 1 && w::actionMenuItems(actions, actions.addList({save})).starts_with("Save\t");
    fuse::ui::Icon icon{};
    ok = ok && fuse::ui::findIcon("save", icon) && fuse::ui::iconName(icon) == "save";
    ok = ok && fuse::text::familyHash("Open Sans") != 0 && fuse::text::formatPlural("one {# file} other {# files}", "en", 3) == "3 files";
    std::vector<fuse::ui::u32> ids;
    fuse::app::MenuItem file{.title = "File", .items = {{.title = "Quit", .id = 7}}};
    ok = ok && fuse::app::menuItemsText(file, ids) == "Quit" && ids.size() == 1 && ids[0] == 7u;
    std::printf("fuse_ui_consumer: %s (%g clicks, %zu draw commands)\n", ok ? "OK" : "FAILED", model.number("clicks"), ui.drawList().size());
    return ok ? 0 : 1;
}
