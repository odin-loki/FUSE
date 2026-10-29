// fuse_editor_fuse: the FUSE editor with FUSE UI chrome (UI9, FUSE_EDITOR_SHELL=fuse), beside fuse_editor_qt.
//   fuse_editor_fuse [scene.fscene] [--font-scale=1.25] [--screenshot=out.png]
#include "fuse/app/native.hpp"
#include "fuse/editor/fui/editor_shell.hpp"

#include <fuse/editor/editor_host.hpp>

#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    fuse::app::Application app{{.name = "fuse_editor_fuse", .argc = argc, .argv = argv}};
    auto& win = app.createWindow({.title = "FUSE Editor", .width = 1600, .height = 900});
    fuse::editor::EditorHost host;
    fuse::editor::fui::DialogServices services(app, win);
    const std::string dir = fuse::app::Settings::defaultDir("fuse_editor");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    fuse::editor::EditorController controller(host, services, {.settingsPath = dir + "/editor_settings.json"});
    fuse::editor::fui::EditorShellWindow shell(win, controller, {.fontScale = float(app.args().number("font-scale", 1.0))});
    for (const std::string& p : app.args().positional())
        if (!controller.fileOpenPath(p)) return app.fail(controller.lastError());
    if (const std::string_view shot = app.args().get("screenshot"); !shot.empty()) {
        app.step(0.016f);
        shell.sync();
        app.step(0.016f);
        return win.writePng(std::string(shot)) ? 0 : app.fail("cannot write screenshot");
    }
    controller.startGameLoop(60.f);
    fuse::app::NativePlatform native(app);
    native.show(win);
    const int code = app.run();
    controller.stopGameLoop();
    return code;
}
