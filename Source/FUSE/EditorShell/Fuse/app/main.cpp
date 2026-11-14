// fuse_editor_fuse: the FUSE editor (FUSE UI chrome; UI9, plan §8). The package installs it as bin/fuse_editor.
//   fuse_editor_fuse [scene.fscene] [--font-scale=1.25] [--screenshot=out.png]
#include "fuse/app/native.hpp"
#if defined(FUSE_EDITOR_HAS_A11Y)
#include "fuse/app/accessibility.hpp"
#endif
#include "fuse/editor/fui/app_icon.hpp"
#include "fuse/editor/fui/editor_shell.hpp"

#include <fuse/editor/editor_host.hpp>

#include <cstdio>
#include <filesystem>
#include <memory>
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
    // --gpu-viewport: live 3D viewport from SceneViewportHost. On screen the presenter adopts the host's device and
    // the frame is sampled in place (zero-copy); for --screenshot it is read back into the viewport image.
    const bool gpuViewport = app.args().has("gpu-viewport");
    std::unique_ptr<fuse::editor::SceneViewportHost> scene;
    if (gpuViewport) {
        scene = fuse::editor::SceneViewportHost::create({.validation = app.args().has("validation"), .readback = true});
        if (!scene->valid()) {
            std::fprintf(stderr, "fuse_editor_fuse: live viewport unavailable (%s)\n", scene->reason().c_str());
            scene.reset();
        }
    }
    if (const std::string_view shot = app.args().get("screenshot"); !shot.empty()) {
        if (scene) shell.setSceneViewport(scene.get());
        app.step(0.016f);
        shell.sync();
        app.step(0.016f);
        return win.writePng(std::string(shot)) ? 0 : app.fail("cannot write screenshot");
    }
    controller.startGameLoop(60.f);
    fuse::app::NativeOptions nopt;
    nopt.osPopups = true;  // menus / combo lists as their own OS windows (Qt QMenu)
    nopt.osWindows = true; // floated panels as their own OS windows (Qt floating QDockWidget)
    if (scene) {
        nopt.adoptDevice = {scene->instanceHandle(), scene->physicalDeviceHandle(), scene->deviceHandle(),
                            scene->queueHandle(), scene->queueFamily()};
        nopt.drawMode = fuse::app::VkDrawMode::GpuDraw;
    }
    int code = 0;
    {
        fuse::app::NativePlatform native(app, nopt);
        native.show(win);
        native.setIcon(win, fuse::editor::fui::applicationIcon()); // window icon (Branding rasters; exe icon: .rc)
        services.setNative(&native); // OS file dialogs (FUSE UI's own when the OS has none)
#if defined(FUSE_EDITOR_HAS_A11Y)
        // screen readers (UI Automation / AT-SPI through AccessKit): menus, panels and inspector fields
        fuse::app::WindowAccessibility a11y(native, win);
#endif
        if (scene) {
            if (native.gpuDraws(win))
                shell.setSceneViewport(scene.get(), [&](const fuse::editor::SceneViewportFrame& f) {
                    native.setExternalTexture(win, 0, {f.image, f.view, f.layout});
                });
            else
                shell.setSceneViewport(scene.get()); // CPU-presented window: readback path
        }
        code = app.run();
        controller.stopGameLoop();
        shell.setSceneViewport(nullptr);
        services.setNative(nullptr);
    } // presenter released before the device it adopted
    return code;
}
