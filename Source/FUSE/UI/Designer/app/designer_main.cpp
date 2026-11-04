// fuse_ui_designer: the UI designer prototype in an application window.
//   fuse_ui_designer [file.fuseui] [--icons=pack.fuseicons] [--save=out.fuseui] [--screenshot=out.png]
// Ctrl+Z / Ctrl+Y undo / redo; Enter in the palette adds a widget under the selection; click the canvas to select.
// The icon picker lists the icon pack (default: the build's starter pack); the effect picker the built-in effects.
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"
#include "fuse/ui/designer/designer.hpp"
#include "fuse/ui/effects.hpp"
#include "fuse/ui/icons.hpp"

#include <cstdio>
#include <string>

#ifndef FUSE_UI_DESIGNER_ICONS
#define FUSE_UI_DESIGNER_ICONS ""
#endif

int main(int argc, char** argv) {
    fuse::app::Application app{{.name = "fuse_ui_designer", .argc = argc, .argv = argv}};
    auto& win = app.createWindow({.title = "FUSE UI Designer", .width = 1280, .height = 760});
    fuse::ui::UiContext& ui = win.ui();
    ui.setCss(ui.root(), "padding", "0px");
    // declared before the designer: it reads them while it lives
    std::expected<fuse::ui::IconSet, std::string> icons = std::unexpected(std::string("no icon pack"));
    if (const std::string pack(app.args().get("icons", FUSE_UI_DESIGNER_ICONS)); !pack.empty()) icons = fuse::ui::IconSet::fromFile(pack);
    if (icons) ui.setIcons(&*icons);
    else std::fprintf(stderr, "fuse_ui_designer: %s\n", icons.error().c_str());
    fuse::ui::EffectRegistry effects;
    effects.addBuiltins();
    ui.setEffects(&effects);
    fuse::ui::designer::Designer d(ui);
    std::string file;
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] != '-') file = argv[i];
    if (!file.empty()) {
        std::string err;
        if (!d.openFile(file, &err)) return app.fail(err.c_str());
    }
    if (const std::string_view out = app.args().get("save"); !out.empty())
        if (!d.saveFile(std::string(out))) return app.fail("cannot save");
    if (const std::string_view shot = app.args().get("screenshot"); !shot.empty()) {
        app.step(0.016f);
        return win.writePng(std::string(shot)) ? 0 : app.fail("cannot write screenshot");
    }
    fuse::app::NativePlatform native(app);
    native.show(win);
    return app.run();
}
