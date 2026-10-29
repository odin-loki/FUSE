// hello_app (plan UI5 sample): a .fuseui document, one bound counter, on-demand loop.
//   hello_app [--ui=path/hello.fuseui] [--screenshot=out.png] [--clicks=N]
// With --screenshot the app clicks the button N times (default 3), writes the frame and exits (headless).
#include "fuse/app/app.hpp"
#include "fuse/ui/doc/document.hpp"

#ifndef FUSE_APP_SAMPLE_UI_DIR
#define FUSE_APP_SAMPLE_UI_DIR "ui"
#endif

int main(int argc, char** argv) {
    fuse::app::Application app{{.name = "hello_app", .argc = argc, .argv = argv, .loadSettings = false}};
    auto& window = app.createWindow({.title = "Hello", .width = 360, .height = 220});
    fuse::ui::doc::Model model;
    model.set("clicks", 0.0);
    const std::string ui(app.args().get("ui", FUSE_APP_SAMPLE_UI_DIR "/hello.fuseui"));
    if (auto loaded = window.load(ui, &model); !loaded) return app.fail(loaded.error());
    window.document()->onAction([&](std::string_view action, fuse::ui::NodeHandle, const fuse::ui::SignalArgs&) {
        if (action == "click") model.set("clicks", model.number("clicks") + 1.0);
        if (action == "quit") app.quit();
    });
    if (const std::string_view shot = app.args().get("screenshot"); !shot.empty()) {
        app.step(0.f);
        auto& ctx = window.ui();
        const fuse::ui::Rect b = ctx.rect(ctx.find("click"));
        for (int i = 0, n = int(app.args().number("clicks", 3)); i < n; ++i) {
            window.click({b.x + b.w * 0.5f, b.y + b.h * 0.5f});
            app.step(0.f);
        }
        window.ui().pointerMove({1.f, 1.f});
        app.step(0.f);
        if (!window.writePng(std::string(shot))) return app.fail("cannot write screenshot");
        std::printf("hello_app: %llu frames, wrote %.*s\n", static_cast<unsigned long long>(window.framesPresented()),
                    int(shot.size()), shot.data());
        return 0;
    }
    return app.run(); // sleeps until input, a timer or an animation needs a frame
}
