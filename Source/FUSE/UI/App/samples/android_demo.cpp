// fuse_ui_android_demo (plan UI1 / UI5 mobile sample): widgets on a phone, safe-area padding, touch, soft keyboard,
// and lifecycle (the surface is released in the background and rebuilt on return; the UI state stays).
//   fuse_ui_android_demo [--draw=gpu|cpu] [--vulkan=0] [--state=fuse_demo_state.txt]
// Android gates (Tools: App/android/android_gate.py) drive it with adb and read the state file it keeps current:
// counters, widget values, lifecycle stats and an in-process check of the first presented frame of every surface
// (Vulkan read-back against the CPU raster). Process death: the tap count is app state saved through
// Application::onSaveState; focus, line-edit text, the tab page and the list scroll are the windows' UI state.
// Builds and runs on desktop too.
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#if defined(_WIN32)
#include <process.h>
static int processId() { return _getpid(); }
#else
#include <unistd.h>
static int processId() { return int(::getpid()); }
#endif
static int g_runs = 0; ///< main() runs in this process (Android recreates the activity in the same process)

namespace w = fuse::ui::widgets;
using fuse::ui::NodeHandle;
using fuse::ui::SignalArgs;
using fuse::ui::SignalKind;
using fuse::ui::UiContext;

namespace {
struct Demo {
    fuse::app::AppWindow* win = nullptr;
    NodeHandle root, info, taps, echo, list, page;
    int tapCount = 0;
    unsigned checkedSurface = 0; ///< last surface whose first frame was compared
    unsigned seenSurface = 0;
    unsigned long long framesAtSurface = 0;
    std::string text;
    std::string presentCheck = "pending";
    int maxErr = -1;
    double badFraction = 0.0;
};

/// Vulkan read-back of what the window shows vs the CPU raster of the same frame.
void checkPresent(fuse::app::NativePlatform& native, Demo& d) {
    fuse::ui::Image shown;
    if (!native.capture(*d.win, shown)) {
        d.presentCheck = "no-readback";
        return;
    }
    const fuse::ui::Image& ref = d.win->rasterize({});
    if (shown.width != ref.width || shown.height != ref.height) {
        d.presentCheck = "size-mismatch";
        return;
    }
    int maxErr = 0;
    std::size_t bad = 0;
    for (std::size_t i = 0; i < ref.pixels.size(); ++i) {
        int e = 0;
        for (int c = 0; c < 24; c += 8)
            e = std::max(e, std::abs(int((shown.pixels[i] >> c) & 0xFF) - int((ref.pixels[i] >> c) & 0xFF)));
        maxErr = std::max(maxErr, e);
        bad += e > 8;
    }
    d.maxErr = maxErr;
    d.badFraction = double(bad) / double(std::max<std::size_t>(1, ref.pixels.size()));
    d.presentCheck = d.badFraction < 0.005 ? "pass" : "fail";
    std::printf("fuse_ui_android_demo: present check %s (max error %d, %.4f%% pixels off)\n", d.presentCheck.c_str(),
                maxErr, d.badFraction * 100.0);
}

std::string stateText(fuse::app::NativePlatform& native, Demo& d) {
    UiContext& ui = d.win->ui();
    const auto ls = native.lifecycle();
    const auto in = native.safeAreaInsets(*d.win);
    static const char* const kKinds[] = {"headless", "vulkan", "software"};
    const auto px = d.win->pixelSize();
    const fuse::ui::Size lg = d.win->size();
    char buf[1024];
    std::snprintf(buf, sizeof buf,
                  "presenter=%s\ngpu=%.*s\ngpu_draw=%d\nscale=%.3f\npixels=%dx%d\nlogical=%dx%d\nframes=%llu\ntaps=%d\n"
                  "scroll=%.1f\nfocus=%d\nsurfaces_created=%u\nsurfaces_lost=%u\npauses=%u\nresumes=%u\nconfig_changes=%u\n"
                  "low_memory=%u\nforeground=%d\nhas_surface=%d\nkeyboard=%d\ninsets=%.1f,%.1f,%.1f,%.1f\n"
                  "page=%d\npid=%d\nruns=%d\nstates_saved=%u\nrestored=%d\n"
                  "present_check=%s\npresent_max_error=%d\npresent_bad_fraction=%.6f\npresent_checked_surface=%u\n",
                  kKinds[int(native.kind(*d.win))], int(native.gpuName().size()), native.gpuName().data(),
                  native.gpuDraws(*d.win) ? 1 : 0, double(d.win->scale()), int(px.w), int(px.h), int(lg.w), int(lg.h),
                  static_cast<unsigned long long>(d.win->framesPresented()), d.tapCount, double(ui.scroll(d.list).y),
                  ui.focused() == ui.find("edit") ? 1 : 0, ls.surfacesCreated, ls.surfacesLost, ls.pauses, ls.resumes,
                  ls.configChanges, ls.lowMemory, ls.foreground ? 1 : 0, ls.hasSurface ? 1 : 0, ls.softKeyboard ? 1 : 0,
                  double(in.left), double(in.top), double(in.right), double(in.bottom), int(ui.value(d.page)),
                  processId(), g_runs, ls.statesSaved, ls.stateRestored ? 1 : 0, d.presentCheck.c_str(), d.maxErr,
                  d.badFraction, d.checkedSurface);
    std::string out = std::string(buf) + "text=" + d.text + "\ncomposition=" + std::string(ui.composition()) + "\n";
    for (const char* id : {"tap", "edit", "list", "title", "check"}) { // device pixels, for adb input
        const fuse::ui::Rect r = ui.rect(ui.find(id));
        const float s = d.win->scale();
        std::snprintf(buf, sizeof buf, "rect_%s=%d,%d,%d,%d\n", id, int(r.x * s), int(r.y * s), int(r.w * s), int(r.h * s));
        out += buf;
    }
    { // the third tab of the page bar (tabs are text width + 12px padding each side, from the left)
        const fuse::ui::Rect r = ui.contentRect(d.page);
        const float fs = ui.computed(d.page).fontSize, s = d.win->scale();
        float x = r.x;
        for (const char* t : {"Home", "List"}) x += std::ceil(ui.measureString(t, fs)) + 24.f;
        const float w = std::ceil(ui.measureString("About", fs)) + 24.f;
        std::snprintf(buf, sizeof buf, "rect_page2=%d,%d,%d,%d\n", int(x * s), int(r.y * s), int(w * s), int(r.h * s));
        out += buf;
    }
    return out;
}

void writeState(const std::string& path, const std::string& s) {
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
    std::rename(tmp.c_str(), path.c_str());
}
} // namespace

int main(int argc, char** argv) {
    ++g_runs;
    fuse::app::Application app{{.name = "fuse_ui_android_demo", .argc = argc, .argv = argv, .loadSettings = false}};
    auto& window = app.createWindow({.title = "FUSE UI", .width = 400, .height = 720});
    Demo d;
    d.win = &window;
    UiContext& ui = window.ui();
    (void)ui.addStyleSheet(".demo { gap: 10px; padding: 16px; } .big { font-size: 26px; } .grow { flex-grow: 1; } .fill { flex-grow: 1; flex-basis: 0px; }");

    d.root = w::panel(ui, ui.root(), "demo");
    ui.addClass(d.root, "demo");
    ui.addClass(d.root, "grow");
    ui.addClass(w::label(ui, d.root, "FUSE UI on Android", "title"), "big");
    d.info = w::label(ui, d.root, "", "info");
    const NodeHandle tap = w::button(ui, d.root, "Tap me", "tap");
    d.taps = w::label(ui, d.root, "Taps: 0", "taps");
    const NodeHandle edit = w::lineEdit(ui, d.root, "", "edit");
    d.echo = w::label(ui, d.root, "Text:", "echo");
    w::checkBox(ui, d.root, "Check box", false, "check");
    w::slider(ui, d.root, 0.f, 100.f, 40.f, 0.f, "slider");
    w::comboBox(ui, d.root, "Overlay popup\nSecond item\nThird item", 0, "combo");
    d.page = w::tabBar(ui, d.root, "Home\nList\nAbout", 0, "page");
    d.list = w::scrollArea(ui, d.root, "list");
    ui.addClass(d.list, "fill"); // takes what is left, scrolls the rest
    for (int i = 0; i < 100; ++i) w::label(ui, d.list, "Row " + std::to_string(i + 1), "row" + std::to_string(i));

    ui.connect(tap, SignalKind::Clicked, [&d](UiContext& c, NodeHandle, const SignalArgs&) {
        ++d.tapCount;
        c.setText(d.taps, "Taps: " + std::to_string(d.tapCount));
    });
    ui.connect(edit, SignalKind::TextChanged, [&d](UiContext& c, NodeHandle, const SignalArgs& a) {
        d.text = std::string(a.text);
        c.setText(d.echo, "Text: " + d.text);
    });

    // app state beyond the widgets survives process death through the saved state
    app.onSaveState([&d](fuse::app::Application&, fuse::app::Settings& s) { s.set("demo.taps", double(d.tapCount)); });
    app.onRestoreState([&d](fuse::app::Application&, const fuse::app::Settings& s) {
        d.tapCount = int(s.number("demo.taps"));
        d.win->ui().setText(d.taps, "Taps: " + std::to_string(d.tapCount));
    });

    fuse::app::NativeOptions opts;
    const std::string_view draw = app.args().get("draw");
    opts.drawMode = draw == "cpu" ? fuse::app::VkDrawMode::CpuUpload : draw == "gpu" ? fuse::app::VkDrawMode::GpuDraw
                                                                                     : fuse::app::VkDrawMode::Auto;
    opts.vulkan = app.args().number("vulkan", 1.0) != 0.0;
    fuse::app::NativePlatform native(app, opts);
    fuse::app::NativeHooks hooks;
    hooks.user = &d;
    hooks.insetsChanged = [](void* user, fuse::app::AppWindow& win, const fuse::app::Insets& in) {
        auto& dd = *static_cast<Demo*>(user);
        UiContext& c = win.ui();
        auto px = [](float v) { return std::to_string(int(v + 16.5f)) + "px"; }; // 16px base padding + the inset
        c.setCss(dd.root, "padding-left", px(in.left));
        c.setCss(dd.root, "padding-top", px(in.top));
        c.setCss(dd.root, "padding-right", px(in.right));
        c.setCss(dd.root, "padding-bottom", px(in.bottom));
    };
    native.setHooks(hooks);
    const fuse::app::PresenterKind kind = native.show(window);
    std::printf("fuse_ui_android_demo: presenter %d on %.*s, scale %.3f, %s\n", int(kind), int(native.gpuName().size()),
                native.gpuName().data(), double(window.scale()), std::string(native.lastError()).c_str());
    {
        const auto px = window.pixelSize();
        char buf[192];
        std::snprintf(buf, sizeof buf, "%s%s%.*s, %dx%d px, scale %.2f",
                      kind == fuse::app::PresenterKind::Vulkan ? "Vulkan" : kind == fuse::app::PresenterKind::Software ? "Software" : "Headless",
                      native.gpuName().empty() ? "" : " on ", int(native.gpuName().size()), native.gpuName().data(),
                      int(px.w), int(px.h), double(window.scale()));
        ui.setText(d.info, buf);
    }

    const std::string statePath(app.args().get("state", "fuse_demo_state.txt"));
    // --ime-test: the gate drives the IME bridge through files/fuse_demo_ime.txt ("compose <text>" / "commit <text>")
    const bool imeTest = app.args().has("ime-test");
    std::string last;
    while (app.step(imeTest ? 0.1f : 1e30f)) {
        if (imeTest) {
            if (std::FILE* f = std::fopen("fuse_demo_ime.txt", "rb")) {
                char line[256] = {};
                const std::size_t n = std::fread(line, 1, sizeof line - 1, f);
                std::fclose(f);
                std::remove("fuse_demo_ime.txt");
                std::string cmd(line, n);
                while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) cmd.pop_back();
                const std::size_t sp = cmd.find(' ');
                if (sp != std::string::npos)
                    native.imeTestInput(cmd.substr(sp + 1), cmd.compare(0, sp, "commit") == 0);
            }
        }
        const auto ls = native.lifecycle();
        if (ls.surfacesCreated != d.seenSurface) { // a new surface: compare its next presented frame
            d.seenSurface = ls.surfacesCreated;
            d.framesAtSurface = window.framesPresented();
            window.requestRedraw();
        }
        // with asyncVulkan the first frames are software: compare the first one of the final presenter
        if (ls.hasSurface && ls.surfacesCreated != d.checkedSurface && !native.presenterPending(window) &&
            window.framesPresented() > d.framesAtSurface) {
            d.checkedSurface = ls.surfacesCreated;
            checkPresent(native, d);
        }
        std::string s = stateText(native, d);
        if (s != last) {
            writeState(statePath, s);
            last = std::move(s);
        }
    }
    return 0;
}
