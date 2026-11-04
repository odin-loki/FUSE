// fuse_app_idle_native (plan UI5, §6 idle budget on a real window): an idle app with a real OS window (Win32, X11 under
// Xvfb) presents nothing and burns ~0 % CPU. The window shows a document, settles, then the loop runs for N seconds
// (--seconds, default 3) with nothing to do but a one-shot timer that ends it: frames presented must stay flat, the
// loop may wake only for OS messages (≤ 10 / s; a polling loop would be ≥ 60 / s), and process CPU time must stay
// under 2 % of the wall time.
// Exit 77 (skip) without a display.
#include "fuse/app/native.hpp"
#include "fuse/ui/doc/document.hpp"

#include <chrono>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif

using namespace fuse::ui;
namespace app = fuse::app;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
constexpr const char* kDoc = R"({ "fuseui": 1, "root": { "class": "panel", "id": "p", "children": [
    { "class": "label", "id": "l", "text": "Idle window" }, { "class": "button", "id": "b", "text": "Button" },
    { "class": "lineedit", "id": "e", "text": "text" } ] } })";

/// User + kernel CPU seconds of this process.
double processCpuSeconds() {
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
    const auto toSec = [](const FILETIME& f) {
        return double((static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7;
    };
    return toSec(kernel) + toSec(user);
#else
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return double(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + double(u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1e-6;
#endif
}
/// Voluntary + involuntary context switches (Linux; 0 elsewhere).
long contextSwitches() {
#if defined(_WIN32)
    return 0;
#else
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_nvcsw + u.ru_nivcsw;
#endif
}
} // namespace

int main(int argc, char** argv) {
    app::Application a{{.name = "gate_idle_native", .argc = argc, .argv = argv, .loadSettings = false}};
    const double seconds = a.args().number("seconds", 3.0);
    app::NativePlatform native(a);
    if (!native.available()) {
        std::printf("fuse_app_idle_native: skipped (%.*s)\n", int(native.lastError().size()), native.lastError().data());
        return 77;
    }
    auto& w = a.createWindow({.title = "fuse_app idle gate", .width = 320, .height = 200});
    CHECK(w.loadText(kDoc));
    const app::PresenterKind kind = native.show(w);
    static const char* const kKinds[] = {"headless", "vulkan", "gdi"};
    std::printf("fuse_app_idle_native: presenter %s%s%.*s%s%.*s\n", kKinds[int(kind)], native.gpuName().empty() ? "" : " on ",
                int(native.gpuName().size()), native.gpuName().data(), native.lastError().empty() ? "" : "; ",
                int(native.lastError().size()), native.lastError().data());
    if (kind == app::PresenterKind::Headless) {
        std::printf("fuse_app_idle_native: skipped, window could not be shown (%.*s)\n", int(native.lastError().size()),
                    native.lastError().data());
        return 77;
    }
    // settle: first frame, map / expose / focus noise
    for (int i = 0; i < 400 && w.framesPresented() == 0; ++i) a.step(0.01f);
    CHECK(w.framesPresented() >= 1);
    const auto settleEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < settleEnd) a.step(0.05f);

    // idle: one timer that only ends the loop (it does not touch the UI)
    const u64 frames0 = w.framesPresented();
    const u64 iter0 = a.stats().iterations;
    const long cs0 = contextSwitches();
    const double cpu0 = processCpuSeconds();
    const auto t0 = std::chrono::steady_clock::now();
    w.ui().startTimer(float(seconds), false, [&](UiContext&) { a.quit(); });
    a.run();
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double cpu = processCpuSeconds() - cpu0;
    const u64 frames = w.framesPresented() - frames0;
    const u64 wakeups = a.stats().iterations - iter0;
    const long switches = contextSwitches() - cs0;
    std::printf("fuse_app_idle_native: idle %.2f s: %llu frames presented, %llu loop wakeups, CPU %.1f ms (%.3f %%)%s\n",
                wall, static_cast<unsigned long long>(frames), static_cast<unsigned long long>(wakeups), cpu * 1e3,
                100.0 * cpu / wall, switches ? (", " + std::to_string(switches) + " context switches").c_str() : "");
    CHECK(wall >= seconds * 0.9);
    CHECK(frames == 0);
    // the timer wake plus stray OS messages (Win32 delivers a few system WM_TIMERs), never a polling loop (>= 60/s)
    CHECK(wakeups <= 2u + u64(10.0 * seconds));
    CHECK(cpu <= 0.02 * wall);
    std::printf("fuse_app_idle_native: %s\n", g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}
