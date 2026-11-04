// hello_app (plan UI5 sample): a .fuseui document, one bound counter, on-demand loop.
//   hello_app [--ui=path/hello.fuseui] [--icons=pack.fuseicons] [--screenshot=out.png] [--clicks=N] [--startup-probe]
//             [--frame-probe] [--window-shot=out.png]
// With --screenshot the app clicks the button N times (default 3), writes the frame and exits (headless).
// --startup-probe opens the OS window, prints the time from process creation to the first presented frame, exits.
// --frame-probe opens the OS window and prints median present times of small-change and whole-window frames.
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"
#include "fuse/ui/doc/document.hpp"
#include "fuse/ui/icons.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

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
#ifndef FUSE_APP_SAMPLE_ICONS
#define FUSE_APP_SAMPLE_ICONS "fuse_ui_icons.fuseicons"
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
    // the buttons' "icon" keys come from the cooked starter pack (fuse_ui_icon_cook); without it they show text only
    auto icons = fuse::ui::IconSet::fromFile(std::string(app.args().get("icons", FUSE_APP_SAMPLE_ICONS)));
    if (icons) window.ui().setIcons(&*icons);
    else std::fprintf(stderr, "hello_app: no icons (%s)\n", icons.error().c_str());
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
                                                                                   : fuse::app::VkDrawMode::Auto,
                                           .asyncVulkan = !app.args().has("sync-vulkan")});
    const fuse::app::PresenterKind kind = native.show(window);
    const double tShown = msSinceProcessStart();
    static const char* const kKinds[] = {"headless", "vulkan", "software"};
    std::fprintf(stderr, "hello_app: presenter %s%s%s%.*s%s\n", kKinds[int(kind)], native.presenterPending(window) ? " (Vulkan starting)" : "",
                 native.gpuName().empty() ? "" : " on ", int(native.gpuName().size()), native.gpuName().data(),
                 native.gpuDraws(window) ? " (GPU draw list)" : "");
    if (app.args().has("frame-probe")) {
        // steady frames on the real window: the pointer crossing the button (small damage), then whole-window redraws
        for (int i = 0; i < 2000 && window.framesPresented() < 1; ++i) app.step(0.005f);
        const double first = msSinceProcessStart();
        native.waitForPresenter(window); // the presenter the window keeps (Vulkan after an async start-up)
        app.step(0.f);
        auto& ctx = window.ui();
        const fuse::ui::Rect b = ctx.rect(ctx.find("click"));
        auto timed = [&](auto&& change) {
            std::vector<double> ms;
            for (int i = 0; i < 40; ++i) {
                change(i);
                const unsigned long long before = window.framesPresented();
                const double t0 = msSinceProcessStart();
                app.step(0.f);
                if (window.framesPresented() > before) ms.push_back(msSinceProcessStart() - t0);
            }
            std::sort(ms.begin(), ms.end());
            return ms.empty() ? -1.0 : ms[ms.size() / 2];
        };
        const double hover = timed([&](int i) { window.ui().pointerMove(i & 1 ? fuse::ui::Vec2{1.f, 1.f} : fuse::ui::Vec2{b.x + b.w * 0.5f, b.y + b.h * 0.5f}); });
        const double full = timed([&](int) { window.requestRedraw(); });
        std::printf("hello_app: frame_probe presenter=%s first_frame_ms=%.1f hover_frame_ms=%.3f full_frame_ms=%.3f (%ux%u px)\n",
                    kKinds[int(native.kind(window))], first, hover, full, window.pixelSize().w, window.pixelSize().h);
        return 0;
    }
    if (app.args().has("startup-probe")) {
        // launch-to-first-presented-frame on a real OS window (gate fuse_app_native_startup), from process creation
        for (int i = 0; i < 2000 && window.framesPresented() < 1; ++i) app.step(0.005f);
        const double ms = msSinceProcessStart();
        const fuse::app::PresenterKind first = native.kind(window);
        std::printf("hello_app: first_frame_ms=%.1f presenter=%s frames=%llu (main %.1f, document %.1f, window+presenter %.1f)\n",
                    ms, kKinds[int(first)], static_cast<unsigned long long>(window.framesPresented()), tMain, tLoaded, tShown);
        if (first == fuse::app::PresenterKind::Headless) return 77;
        // async Vulkan: the event loop switches presenters once the worker is done; the switch frame must show the UI
        for (int i = 0; i < 4000 && native.presenterPending(window); ++i) app.step(0.005f);
        const unsigned long long before = window.framesPresented();
        for (int i = 0; i < 400 && window.framesPresented() == before; ++i) {
            if (native.kind(window) != first) break; // switched: that present already happened in step()
            window.requestRedraw();
            app.step(0.005f);
        }
        const double switchMs = msSinceProcessStart();
        const fuse::app::PresenterKind final = native.kind(window);
        fuse::ui::Image shown;
        int maxErr = -1;
        if (native.capture(window, shown)) {
            const fuse::ui::Image& ref = window.rasterize({});
            if (shown.width == ref.width && shown.height == ref.height) {
                maxErr = 0;
                for (std::size_t p = 0; p < ref.pixels.size(); ++p)
                    for (int c = 0; c < 3; ++c)
                        maxErr = std::max(maxErr, std::abs(int((shown.pixels[p] >> (8 * c)) & 0xFF) - int((ref.pixels[p] >> (8 * c)) & 0xFF)));
            }
        }
        std::printf("hello_app: switch_ms=%.1f final=%s gpu=%d max_err=%d\n", final != first ? switchMs : -1.0, kKinds[int(final)],
                    native.gpuDraws(window) ? 1 : 0, maxErr);
        return 0;
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
