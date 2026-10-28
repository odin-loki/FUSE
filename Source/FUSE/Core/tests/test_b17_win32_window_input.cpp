#include <fuse/core/track_b.hpp>
#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/input.hpp>
#include <fuse/platform/mouse_button.hpp>
#include <fuse/platform/win_button.hpp>
#include <fuse/platform/window.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testWinButtonOk() {
    expectTrue(fuse::platform::win_button_ok(0), "win_button_ok(0)");
    expectTrue(fuse::platform::win_button_ok(7), "win_button_ok(7)");
    expectTrue(!fuse::platform::win_button_ok(-1), "win_button_ok(-1)");
    expectTrue(!fuse::platform::win_button_ok(8), "win_button_ok(8)");
}

void testMouseButtonOk() {
    expectTrue(fuse::platform::mouse_button_ok(0), "mouse_button_ok(0)");
    expectTrue(fuse::platform::mouse_button_ok(4), "mouse_button_ok(4)");
    expectTrue(!fuse::platform::mouse_button_ok(-1), "mouse_button_ok(-1)");
    expectTrue(!fuse::platform::mouse_button_ok(5), "mouse_button_ok(5)");
}

void testTrackBUnlockIndependentOfThisBinary() {
    const bool unlocked = fuse::core::trackBUnlocked();
    const bool unlockedRuntime = fuse::core::trackBUnlockedRuntime();
    expectTrue(unlocked == unlockedRuntime, "runtime Track B query matches compile-time gate");

#if defined(FUSE_TRACK_B_UNLOCK) && FUSE_TRACK_B_UNLOCK
    expectTrue(unlocked, "FUSE_TRACK_B_UNLOCK compiles production defaults on");
#else
    (void)unlocked;
#endif
}

void testEventPumpFeedsInputState() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;
    fuse::platform::InputState input;
    expectTrue(window.isValid(), "window is valid");
    expectTrue(window.nativeHandle().value == nullptr, "default window has no native handle");

    window.resize(640, 480, &pump);
    pump.pushKeyDown(65u);

    input.beginFrame();
    fuse::platform::PlatformEvent event;
    while (pump.pollEvent(event)) {
        input.apply(event);
    }

    expectTrue(input.keyDown(fuse::platform::Key::A), "EventPump KeyDown reaches InputState");
    expectTrue(input.keyPressed(fuse::platform::Key::A), "EventPump KeyDown is pressed this frame");
    expectTrue(!pump.hasPendingEvents(), "queue drained into InputState");
}

void testProcessOsEventsWithoutNativeHandle() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    pump.processOsEvents();
    expectTrue(!pump.hasPendingEvents(), "OS drain is a no-op without a native handle");
}

#if defined(_WIN32)
void testWin32OptionalCreateNativeProcessOsEvents() {
    fuse::platform::WindowDesc desc{};
    desc.title = "FUSE B17";
    desc.width = 64;
    desc.height = 64;
    desc.createNative = true;

    fuse::platform::EventPump pump;
    fuse::platform::Window window(desc);
    expectTrue(window.isValid(), "createNative window is valid");

    if (window.nativeHandle().value == nullptr) {
        std::fprintf(stderr, "SKIP: createNative did not produce a Win32 HWND\n");
        pump.processOsEvents();
        return;
    }

    pump.processOsEvents();
    expectTrue(!pump.quitRequested(), "idle createNative pump does not request quit");
}
#endif

} // namespace

int main() {
    testWinButtonOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core_b17_win32_window_input: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testMouseButtonOk();
    testTrackBUnlockIndependentOfThisBinary();
    testEventPumpFeedsInputState();
    testProcessOsEventsWithoutNativeHandle();
#if defined(_WIN32)
    testWin32OptionalCreateNativeProcessOsEvents();
#endif

    if (g_failures == 0) {
        std::printf("fuse_core_b17_win32_window_input: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core_b17_win32_window_input: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
