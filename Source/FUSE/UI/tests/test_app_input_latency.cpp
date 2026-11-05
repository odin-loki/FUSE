// fuse_app_input_latency (plan §6 "Input to presented frame ≤ 1 frame + present queue"): a real OS window (Win32,
// X11 under Xvfb) with the Vulkan swapchain presenter. A real left click is posted to the window (PostMessage /
// XSendEvent); its release clicks a button whose handler recolours a swatch. Measured per sample, from just before
// the release is posted:
//   - frames: the first frame presented after the input must show it (exactly one present, no frame of delay);
//   - latency: input posted -> that frame's GPU work complete and queued for present (NativePlatform::presentTiming,
//     fence waited after vkQueuePresentKHR). Budget: median ≤ 1 refresh period; every sample ≤ (1 + present queue)
//     periods, the present queue being swapchain images - 1 (the frames a queued image can wait before scan-out);
//   - the swatch colour read back from the OS window (capture) is the new colour.
// --samples=N (default 15), --vsync=0 for MAILBOX / IMMEDIATE. Exit 77 (skip) without a display or a Vulkan presenter.
#include "fuse/app/native.hpp"
#include "fuse/app/platform.hpp"
#include "fuse/ui/doc/document.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace fuse::ui;
namespace app = fuse::app;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
constexpr const char* kDoc = R"({ "fuseui": 1, "root": { "class": "panel", "id": "p", "children": [
    { "class": "button", "id": "b", "text": "Click me" },
    { "class": "panel", "id": "swatch", "style": { "width": "160px", "height": "80px", "background": "#C02040" } } ] } })";
constexpr u32 kColA = 0xC02040, kColB = 0x20C040;

double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

/// Steps until no frame was presented for `quietMs` (the app is idle again).
void settle(app::Application& a, app::AppWindow& w, int quietMs) {
    u64 seen = w.framesPresented();
    auto quietSince = std::chrono::steady_clock::now();
    for (int i = 0; i < 2000; ++i) {
        a.step(0.005f);
        if (w.framesPresented() != seen) {
            seen = w.framesPresented();
            quietSince = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - quietSince > std::chrono::milliseconds(quietMs)) {
            return;
        }
    }
}
bool near(u32 px, u32 rgb) { // either channel order (RGBA bytes = 0xAABBGGRR; BGRA captures)
    const auto same = [](u32 a, u32 b) {
        for (int sh = 0; sh < 24; sh += 8)
            if (std::abs(int((a >> sh) & 255) - int((b >> sh) & 255)) > 8) return false;
        return true;
    };
    const u32 bgr = ((rgb & 255) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 255);
    return same(px, rgb) || same(px, bgr);
}
} // namespace

int main(int argc, char** argv) {
    app::Application a{{.name = "gate_input_latency", .argc = argc, .argv = argv, .loadSettings = false}};
    const int samples = std::max(3, int(a.args().number("samples", 15.0)));
    app::NativeOptions o;
    o.vsync = a.args().number("vsync", 1.0) != 0.0;
    o.asyncVulkan = false; // measure the Vulkan presenter from the first frame
    app::NativePlatform native(a, o);
    if (!native.available()) {
        std::printf("fuse_app_input_latency: skipped (%.*s)\n", int(native.lastError().size()), native.lastError().data());
        return 77;
    }
    auto& w = a.createWindow({.title = "fuse_app input latency gate", .width = 360, .height = 220});
    CHECK(w.loadText(kDoc));
    const app::PresenterKind kind = native.show(w);
    if (kind != app::PresenterKind::Vulkan) {
        std::printf("fuse_app_input_latency: skipped, no Vulkan presenter (%.*s)\n", int(native.lastError().size()),
                    native.lastError().data());
        return 77;
    }
    for (int i = 0; i < 400 && w.framesPresented() == 0; ++i) a.step(0.01f);
    CHECK(w.framesPresented() >= 1);
    settle(a, w, 400);

    UiContext& ui = w.ui();
    const NodeHandle button = ui.find("b"), swatch = ui.find("swatch");
    CHECK(ui.alive(button) && ui.alive(swatch));
    u32 clicks = 0;
    ui.connect(button, SignalKind::Clicked, [&](UiContext& u, NodeHandle, const SignalArgs&) {
        ++clicks;
        u.setCss(swatch, "background", clicks % 2 ? "#20C040" : "#C02040");
    });

    f32 hz = 0.f;
    for (const app::DisplayInfo& d : native.displays())
        if (d.primary || hz == 0.f) hz = d.refreshHz > 0.f ? d.refreshHz : hz;
    if (!(hz >= 20.f && hz <= 1000.f)) hz = 60.f; // unknown (Xvfb): the 60 Hz the plan budgets
    const double period = 1.0 / double(hz);
    const unsigned long long hwnd = native.nativeWindow(w);
    const f32 s = w.scale();
    const Rect br = ui.rect(button), sr = ui.rect(swatch);
    const Vec2 at{(br.x + br.w * 0.5f) * s, (br.y + br.h * 0.5f) * s};

    std::vector<double> lat, start;
    u32 extraFrames = 0, wrongColour = 0;
    app::PresentTiming pt{};
    for (int k = 0; k < samples; ++k) {
        CHECK(native.postPointerButton(hwnd, at, true)); // press: its own (pressed look) frame, then idle again
        settle(a, w, 60);
        const u32 clicks0 = clicks;
        const u64 f0 = w.framesPresented();
        const double t0 = nowSec();
        CHECK(native.postPointerButton(hwnd, at, false)); // release -> Clicked -> swatch recoloured
        for (int i = 0; i < 4000 && w.framesPresented() == f0; ++i) a.step(0.f);
        pt = native.presentTiming(w, true);
        const u64 f1 = w.framesPresented();
        CHECK(clicks == clicks0 + 1);
        if (f1 != f0 + 1) ++extraFrames;
        lat.push_back(pt.gpuDoneAt - t0);
        start.push_back(pt.acquireAt - t0);
        // the OS window shows the new colour (DWM / X server composite: retry briefly)
        bool shown = false;
        for (int i = 0; i < 50 && !shown; ++i) {
            Image cap;
            if (native.capture(w, cap) && cap.width > 0) {
                const int cx = int((sr.x + sr.w * 0.5f) * s), cy = int((sr.y + sr.h * 0.5f) * s);
                shown = cx < cap.width && cy < cap.height && near(cap.at(cx, cy), clicks % 2 ? kColB : kColA);
            }
            if (!shown) a.step(0.002f);
        }
        if (!shown) ++wrongColour;
        settle(a, w, 40);
    }
    std::vector<double> sorted = lat;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2], worst = sorted.back();
    std::sort(start.begin(), start.end());
    static const char* const kModes[] = {"IMMEDIATE", "MAILBOX", "FIFO", "FIFO_RELAXED"};
    const unsigned queue = pt.queueDepth();
    const double bound = (1.0 + double(queue)) * period;
    std::printf("fuse_app_input_latency: %s on %.*s, %s%s, %u swapchain images (present queue %u), refresh %.1f Hz "
                "(frame %.2f ms)\n",
                native.gpuDraws(w) ? "GPU draw" : "CPU upload", int(native.gpuName().size()), native.gpuName().data(),
                pt.presentMode >= 0 && pt.presentMode < 4 ? kModes[pt.presentMode] : "?", pt.dwmPaced ? " (DWM paced)" : "",
                pt.images, queue, double(hz), period * 1e3);
    std::printf("fuse_app_input_latency: %d samples, input -> frame start median %.2f ms; input -> frame on GPU and "
                "queued: median %.2f ms, min %.2f, max %.2f ms (budget: median <= %.2f ms = 1 frame, max <= %.2f ms = 1 "
                "frame + present queue); %u extra frames, %u captures without the change\n",
                samples, start[start.size() / 2] * 1e3, median * 1e3, sorted.front() * 1e3, worst * 1e3, period * 1e3,
                bound * 1e3, extraFrames, wrongColour);
    CHECK(pt.images >= 2 && pt.presents >= u64(samples));
    CHECK(extraFrames == 0);  // the first frame after the input shows it
    CHECK(wrongColour == 0);  // and the OS window displays it
    CHECK(median > 0.0 && median <= period);
    CHECK(worst <= bound);
    std::printf("fuse_app_input_latency: %s\n", g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}
