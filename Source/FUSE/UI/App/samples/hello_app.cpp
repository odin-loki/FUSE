// hello_app (plan UI5 sample): a .fuseui document, one bound counter, on-demand loop.
//   hello_app [--ui=path/hello.fuseui] [--screenshot=out.png] [--clicks=N] [--startup-probe] [--window-shot=out.png]
// With --screenshot the app clicks the button N times (default 3), writes the frame and exits (headless).
// --startup-probe opens the OS window, prints the time from process creation to the first presented frame, exits.
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"
#include "fuse/ui/doc/document.hpp"

#include <cstdio>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#endif

#ifndef FUSE_APP_SAMPLE_UI_DIR
#define FUSE_APP_SAMPLE_UI_DIR "ui"
#endif

namespace {
/// Milliseconds since the OS created this process (includes loader and static init time).
double msSinceProcessStart() {
#if defined(_WIN32)
    FILETIME created, exited, kernel, user, now;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimePreciseAsFileTime(&now);
    auto ticks = [](const FILETIME& f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return double(ticks(now) - ticks(created)) / 1e4; // 100 ns units
#else
    // /proc/self/stat field 22: start time in clock ticks after boot (CLOCK_BOOTTIME)
    std::ifstream f("/proc/self/stat");
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::size_t rp = s.rfind(')');
    if (rp == std::string::npos) return -1.0;
    std::size_t pos = rp + 2;
    for (int field = 3; field < 22 && pos < s.size(); ++field) pos = s.find(' ', pos) + 1;
    const double startTicks = std::strtod(s.c_str() + pos, nullptr);
    timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9 - startTicks / double(sysconf(_SC_CLK_TCK))) * 1000.0;
#endif
}
} // namespace

int main(int argc, char** argv) {
    const double tMain = msSinceProcessStart();
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
    // a real OS window (X11 / Win32) with the Vulkan swapchain presenter; headless when there is no display
    const double tLoaded = msSinceProcessStart();
    fuse::app::NativePlatform native(app, {.vulkan = !app.args().has("no-vulkan"),
                                           .drawMode = app.args().has("cpu-upload") ? fuse::app::VkDrawMode::CpuUpload
                                                                                   : fuse::app::VkDrawMode::Auto});
    const fuse::app::PresenterKind kind = native.show(window);
    const double tShown = msSinceProcessStart();
    static const char* const kKinds[] = {"headless", "vulkan", "gdi"};
    std::fprintf(stderr, "hello_app: presenter %s%s%.*s%s\n", kKinds[int(kind)], native.gpuName().empty() ? "" : " on ",
                 int(native.gpuName().size()), native.gpuName().data(), native.gpuDraws(window) ? " (GPU draw list)" : "");
    if (app.args().has("startup-probe")) {
        // launch-to-first-presented-frame on a real OS window (gate fuse_app_native_startup), from process creation
        for (int i = 0; i < 2000 && window.framesPresented() < 1; ++i) app.step(0.005f);
        const double ms = msSinceProcessStart();
        std::printf("hello_app: first_frame_ms=%.1f presenter=%s frames=%llu (main %.1f, document %.1f, window+presenter %.1f)\n",
                    ms, kKinds[int(kind)], static_cast<unsigned long long>(window.framesPresented()), tMain, tLoaded, tShown);
        return kind == fuse::app::PresenterKind::Headless ? 77 : 0;
    }
    if (const std::string_view shot = app.args().get("window-shot"); !shot.empty()) {
        // smoke run: present a few frames into the OS window, read the window back, write it, exit
        for (int i = 0; i < 200 && window.framesPresented() < 3; ++i) {
            window.requestRedraw();
            app.step(0.02f);
        }
        fuse::app::AppWindow& shown = window;
        fuse::ui::Image img;
        if (!native.capture(shown, img)) return app.fail("cannot read the OS window back");
        const bool wrote = fuse::app::writePng(img, std::string(shot));
        return wrote ? 0 : app.fail("cannot write window shot");
    }
    return app.run(); // sleeps until input, a timer or an animation needs a frame
}
