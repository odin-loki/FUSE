// gui_quickstart: the smallest complete FUSE GUI program against the public API (<fuse/gui.hpp>, fuse::gui).
//   gui_quickstart [--screenshot=out.png]   headless: clicks the button twice, writes a PNG, exits
#include <fuse/gui.hpp>

#include <cstdio>
#include <string>

namespace gui = fuse::gui;

int main(int argc, char** argv) {
    gui::Application app{{.name = "gui_quickstart", .argc = argc, .argv = argv, .loadSettings = false}};
    gui::AppWindow& win = app.createWindow({.title = "FUSE GUI quickstart", .width = 420, .height = 260});
    gui::UiContext& ui = win.ui();
    (void)ui.addStyleSheet("#form { padding: 16px; gap: 10px; } #count { font-size: 18px; }");

    const gui::NodeHandle form = gui::widgets::panel(ui, ui.root(), "form");
    const gui::NodeHandle count = gui::widgets::label(ui, form, "Clicked 0 times", "count");
    const gui::NodeHandle name = gui::widgets::lineEdit(ui, form, "", "name");
    const gui::NodeHandle dark = gui::widgets::checkBox(ui, form, "Dark theme", true, "dark");
    gui::widgets::slider(ui, form, 0.f, 100.f, 40.f, 1.f, "volume");
    const gui::NodeHandle button = gui::widgets::button(ui, form, "Click me", "click");
    (void)name; (void)dark;

    int clicks = 0;
    ui.connect(button, gui::SignalKind::Clicked, [&clicks, count](gui::UiContext& c, gui::NodeHandle, const gui::SignalArgs&) {
        c.setText(count, "Clicked " + std::to_string(++clicks) + " times");
    });

    if (const std::string_view shot = app.args().get("screenshot"); !shot.empty()) {
        app.step(0.f);
        const gui::Rect b = ui.rect(button);
        for (int i = 0; i < 2; ++i) {
            win.click({b.x + b.w * 0.5f, b.y + b.h * 0.5f});
            app.step(0.f);
        }
        if (!win.writePng(std::string(shot))) return app.fail("cannot write screenshot");
        std::printf("gui_quickstart: fuse gui %d.%d.%d, %d clicks, label \"%.*s\"\n", gui::version().major,
                    gui::version().minor, gui::version().patch, clicks, int(ui.text(count).size()), ui.text(count).data());
        return clicks == 2 && ui.text(count) == "Clicked 2 times" ? 0 : 1;
    }
    gui::NativePlatform native{app}; // OS window where a display exists, headless otherwise
    native.show(win);
    return app.run();
}
