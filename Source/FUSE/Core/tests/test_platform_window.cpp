#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/lifecycle.hpp>
#include <fuse/platform/poll_ms.hpp>
#include <fuse/platform/profile.hpp>
#include <fuse/platform/title_len.hpp>
#include <fuse/platform/window.hpp>
#include <fuse/platform/window_title.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../src/platform/platform_window_visibility.hpp"
#endif

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectEq(fuse::u32 actual, fuse::u32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected %u, got %u)\n", message, expected, actual);
        ++g_failures;
    }
}

/// CPU gate. Must run before any window skip; a failure exits instead of continuing.
void testPollMsOk() {
    expectTrue(fuse::platform::poll_ms_ok(1), "poll ms 1 is accepted");
    expectTrue(fuse::platform::poll_ms_ok(100), "poll ms 100 is accepted");
    expectTrue(!fuse::platform::poll_ms_ok(0), "poll ms 0 is rejected");
    expectTrue(!fuse::platform::poll_ms_ok(101), "poll ms 101 is rejected");
}

/// CPU gate. Must run before any window skip; a failure exits instead of continuing.
void testTitleLenOk() {
    expectTrue(fuse::platform::title_len_ok(1), "title length 1 is accepted");
    expectTrue(fuse::platform::title_len_ok(128), "title length 128 is accepted");
    expectTrue(!fuse::platform::title_len_ok(0), "title length 0 is rejected");
    expectTrue(!fuse::platform::title_len_ok(129), "title length 129 is rejected");
}

void testWindowTitleOk() {
    expectTrue(fuse::platform::window_title_ok("FUSE"), "FUSE is a valid window title");
    expectTrue(!fuse::platform::window_title_ok(""), "empty title is rejected");
    expectTrue(!fuse::platform::window_title_ok("FUSE\n"), "newline in title is rejected");

    expectTrue(fuse::platform::window_title_ok(" "), "space is printable ASCII");
    expectTrue(fuse::platform::window_title_ok("~"), "tilde is printable ASCII");
    expectTrue(!fuse::platform::window_title_ok("\x1f"), "code 31 is rejected");
    expectTrue(!fuse::platform::window_title_ok("\x7f"), "DEL is rejected");

    const std::string maxTitle(128, 'A');
    expectTrue(fuse::platform::window_title_ok(maxTitle), "128 character title is accepted");
    const std::string tooLong(129, 'A');
    expectTrue(!fuse::platform::window_title_ok(tooLong), "129 character title is rejected");
}

void testWindowStubStoresDescription() {
    fuse::platform::WindowDesc desc;
    desc.title = "Smoke";
    desc.width = 1280;
    desc.height = 720;
    desc.fullscreen = true;
    desc.borderless = true;
    desc.vsync = false;

    fuse::platform::Window window(desc);
    expectTrue(window.isValid(), "window stub is valid after construction");

    const fuse::platform::WindowDesc stored = window.description();
    expectTrue(stored.title != nullptr && std::string(stored.title) == "Smoke", "title stored");
    expectEq(window.width(), 1280u, "width stored");
    expectEq(window.height(), 720u, "height stored");
    expectTrue(window.isFullscreen(), "fullscreen flag stored");
    expectTrue(!window.vsyncEnabled(), "vsync flag stored");
    expectTrue(window.nativeHandle().value == nullptr, "native handle is null in stub");
}

void testWindowInputCaptureDefaultReleased() {
    fuse::platform::Window window;
    expectTrue(window.inputCapture() == fuse::platform::InputCaptureMode::Released,
               "default capture is Released");
    expectTrue(!window.isInputCaptured(), "default isInputCaptured is false");
}

void testWindowInputCaptureSetCaptured() {
    fuse::platform::Window window;
    window.setInputCapture(fuse::platform::InputCaptureMode::Captured);
    expectTrue(window.inputCapture() == fuse::platform::InputCaptureMode::Captured,
               "setInputCapture stores Captured");
    expectTrue(window.isInputCaptured(), "isInputCaptured is true when Captured");

    window.setInputCapture(fuse::platform::InputCaptureMode::Released);
    expectTrue(window.inputCapture() == fuse::platform::InputCaptureMode::Released,
               "setInputCapture restores Released");
    expectTrue(!window.isInputCaptured(), "isInputCaptured is false when Released");
}

void testRequireCaptureForInputDoesNotDropSyntheticEvents() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    expectTrue(!pump.requireCaptureForInput(), "default requireCaptureForInput is false");
    expectTrue(window.inputCapture() == fuse::platform::InputCaptureMode::Released,
               "synthetic test window stays Released");

    pump.setRequireCaptureForInput(true);
    expectTrue(pump.requireCaptureForInput(), "requireCaptureForInput can be enabled");

    pump.pushKeyDown(65u);
    pump.pushKeyUp(65u);
    pump.pushMouseMove(10, 20);
    pump.pushMouseButton(1u, true, 10, 20);

    fuse::platform::PlatformEvent synthetic{};
    synthetic.type = fuse::platform::PlatformEventType::KeyDown;
    synthetic.keyCode = 66u;
    pump.pushSyntheticEvent(synthetic);

    expectEq(pump.pendingEventCount(), 5u, "pushSyntheticEvent bypasses capture filter");

    fuse::platform::PlatformEvent keyDown;
    expectTrue(pump.pollEvent(keyDown), "synthetic KeyDown polled with capture required");
    expectTrue(keyDown.type == fuse::platform::PlatformEventType::KeyDown,
               "synthetic KeyDown type preserved");
    expectEq(keyDown.keyCode, 65u, "synthetic KeyDown keyCode preserved");

    fuse::platform::PlatformEvent keyUp;
    expectTrue(pump.pollEvent(keyUp), "synthetic KeyUp polled with capture required");
    expectTrue(keyUp.type == fuse::platform::PlatformEventType::KeyUp, "synthetic KeyUp type");

    fuse::platform::PlatformEvent move;
    expectTrue(pump.pollEvent(move), "synthetic MouseMove polled with capture required");
    expectTrue(move.type == fuse::platform::PlatformEventType::MouseMove, "synthetic MouseMove type");

    fuse::platform::PlatformEvent button;
    expectTrue(pump.pollEvent(button), "synthetic MouseButton polled with capture required");
    expectTrue(button.type == fuse::platform::PlatformEventType::MouseButtonDown,
               "synthetic MouseButtonDown type");

    fuse::platform::PlatformEvent direct;
    expectTrue(pump.pollEvent(direct), "direct pushSyntheticEvent polled with capture required");
    expectTrue(direct.type == fuse::platform::PlatformEventType::KeyDown,
               "direct synthetic KeyDown type");
    expectEq(direct.keyCode, 66u, "direct synthetic KeyDown keyCode");
    expectTrue(!pump.hasPendingEvents(), "synthetic capture-bypass queue drained");

    pump.setRequireCaptureForInput(false);
    expectTrue(!pump.requireCaptureForInput(), "requireCaptureForInput restored to default");
}

void testWindowResizeAndCloseRequest() {
    fuse::platform::Window window;
    expectTrue(window.isValid(), "default window is valid");
    expectTrue(window.isFocused(), "default window starts focused");
    expectTrue(!window.isInputCaptured(), "default window starts Released");

    window.resize(800, 600);
    expectEq(window.width(), 800u, "resize updates width");
    expectEq(window.height(), 600u, "resize updates height");

    window.requestClose();
    expectTrue(window.closeRequest() == fuse::platform::WindowCloseRequest::Requested,
               "close request recorded");
    window.clearCloseRequest();
    expectTrue(window.closeRequest() == fuse::platform::WindowCloseRequest::None,
               "close request cleared");
}

void testWindowResizeNotifiesPump() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    window.resize(800, 600, &pump);
    expectEq(pump.pendingEventCount(), 1u, "resize enqueues one event");

    fuse::platform::PlatformEvent event;
    expectTrue(pump.pollEvent(event), "resize event polled");
    expectTrue(event.type == fuse::platform::PlatformEventType::WindowResized, "resize type");
    expectEq(event.width, 800u, "resize width in event");
    expectEq(event.height, 600u, "resize height in event");
    expectTrue(event.window == &window, "resize window pointer");

    window.resize(800, 600, &pump);
    expectEq(pump.pendingEventCount(), 0u, "no-op resize does not enqueue");
}

void testWindowFocusEventsNotifyPump() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    window.setFocused(false, &pump);
    expectTrue(!window.isFocused(), "focus cleared");

    fuse::platform::PlatformEvent lost;
    expectTrue(pump.pollEvent(lost), "focus lost polled");
    expectTrue(lost.type == fuse::platform::PlatformEventType::WindowFocusLost, "focus lost type");
    expectTrue(lost.window == &window, "focus lost window pointer");

    window.setFocused(false, &pump);
    expectEq(pump.pendingEventCount(), 0u, "duplicate focus lost does not enqueue");

    window.setFocused(true, &pump);
    fuse::platform::PlatformEvent gained;
    expectTrue(pump.pollEvent(gained), "focus gained polled");
    expectTrue(gained.type == fuse::platform::PlatformEventType::WindowFocusGained,
               "focus gained type");
}

void testWindowCloseRequestedNotifiesPump() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    window.requestClose(&pump);
    expectTrue(window.closeRequest() == fuse::platform::WindowCloseRequest::Requested,
               "close request recorded with pump");

    fuse::platform::PlatformEvent event;
    expectTrue(pump.pollEvent(event), "close requested polled");
    expectTrue(event.type == fuse::platform::PlatformEventType::WindowCloseRequested,
               "close requested type");
    expectTrue(event.window == &window, "close requested window pointer");

    window.requestClose(&pump);
    expectEq(pump.pendingEventCount(), 0u, "duplicate close request does not enqueue");
}

void testWindowFocusEdgeTransitions() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    window.setFocused(false, &pump);
    window.setFocused(false, &pump);
    expectEq(pump.pendingEventCount(), 1u, "only one focus-lost when already blurred");

    window.setFocused(true, &pump);
    window.setFocused(true, &pump);
    expectEq(pump.pendingEventCount(), 2u, "only one focus-gained when already focused");

    fuse::platform::PlatformEvent first;
    expectTrue(pump.pollEvent(first), "focus lost polled first");
    expectTrue(first.type == fuse::platform::PlatformEventType::WindowFocusLost,
               "first edge event is focus lost");

    fuse::platform::PlatformEvent second;
    expectTrue(pump.pollEvent(second), "focus gained polled second");
    expectTrue(second.type == fuse::platform::PlatformEventType::WindowFocusGained,
               "second edge event is focus gained");
    expectTrue(!pump.hasPendingEvents(), "focus edge sequence fully drained");
}

void testVulkanSurfaceWireIsHeadless() {
    fuse::platform::Window window;
    const fuse::platform::VulkanSurfaceWire wire = window.vulkanSurfaceWire();

    expectTrue(wire.nativeSurface == nullptr, "stub Vulkan surface is null");
    expectTrue(!wire.presentable, "stub surface is not presentable");
    expectTrue(window.nativeVulkanSurface() == nullptr, "nativeVulkanSurface returns null");
}

void testEventPumpSyntheticEvents() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    fuse::platform::PlatformEvent resizeEvent;
    resizeEvent.type = fuse::platform::PlatformEventType::WindowResized;
    resizeEvent.window = &window;
    resizeEvent.width = 1024;
    resizeEvent.height = 768;
    pump.pushSyntheticEvent(resizeEvent);

    fuse::platform::PlatformEvent polled;
    expectTrue(pump.pollEvent(polled), "synthetic resize event polled");
    expectTrue(polled.type == fuse::platform::PlatformEventType::WindowResized, "resize type preserved");
    expectEq(polled.width, 1024u, "resize width preserved");
    expectEq(polled.height, 768u, "resize height preserved");
    expectTrue(polled.window == &window, "window pointer preserved");
}

void testEventPumpQuitFlow() {
    fuse::platform::EventPump pump;
    expectTrue(pump.pumpOnce(), "pumpOnce succeeds with no events");
    expectTrue(!pump.quitRequested(), "quit not requested initially");

    pump.requestQuit();
    expectTrue(pump.quitRequested(), "quit flag set");
    expectTrue(!pump.pumpOnce(), "pumpOnce returns false after quit");

    pump.resetQuit();
    expectTrue(!pump.quitRequested(), "quit reset");
    expectTrue(pump.pumpOnce(), "pumpOnce succeeds after reset");
}

void testEventPumpOsDrainIsNoOp() {
    fuse::platform::EventPump pump;
    pump.processOsEvents();

    fuse::platform::PlatformEvent event;
    expectTrue(!pump.pollEvent(event), "OS drain produces no events in stub");
    expectTrue(!pump.hasPendingEvents(), "queue empty after OS drain");
    expectEq(pump.pendingEventCount(), 0u, "pending count zero after OS drain");
}

void testEventPumpOsDrainDefaultWindowIsNoOp() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    expectTrue(window.nativeHandle().value == nullptr, "default window has no native handle");
    pump.processOsEvents();
    expectTrue(!pump.hasPendingEvents(), "default window OS drain enqueues nothing");
    expectEq(pump.pendingEventCount(), 0u, "pending count zero after default window OS drain");

    fuse::platform::PlatformEvent synthetic;
    synthetic.type = fuse::platform::PlatformEventType::KeyDown;
    synthetic.keyCode = 65u;
    pump.pushSyntheticEvent(synthetic);

    fuse::platform::PlatformEvent polled;
    expectTrue(pump.pollEvent(polled), "synthetic event works after OS drain");
    expectTrue(polled.type == fuse::platform::PlatformEventType::KeyDown,
               "synthetic KeyDown preserved after OS drain");
    expectEq(polled.keyCode, 65u, "synthetic keyCode preserved after OS drain");
    expectTrue(!pump.hasPendingEvents(), "synthetic KeyDown consumed after OS drain");
}

#if defined(_WIN32)
void testWindowCreateNativeWin32OwnsHwnd() {
    fuse::platform::WindowDesc desc;
    desc.title = "FUSE Native";
    desc.width = 320;
    desc.height = 240;
    desc.createNative = true;

    HWND hwnd = nullptr;
    {
        fuse::platform::Window window(desc);
        expectTrue(window.isValid(), "createNative window is valid");
        expectTrue(window.nativeHandle().value != nullptr, "createNative HWND is non-null");
        hwnd = static_cast<HWND>(window.nativeHandle().value);
        expectTrue(IsWindow(hwnd) != FALSE, "createNative HWND is a live overlapped window");
        expectTrue((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE) == 0,
                   "createNative HWND stays hidden");
        expectTrue(window.nativeVulkanSurface() == nullptr,
                   "nativeVulkanSurface stays null with owned HWND");
        expectTrue(window.vulkanSurfaceWire().presentable,
                   "vulkanSurfaceWire is presentable with owned HWND");
        expectTrue(window.vulkanSurfaceWire().nativeSurface == nullptr,
                   "vulkanSurfaceWire nativeSurface stays null");
    }
    expectTrue(IsWindow(hwnd) == FALSE, "DestroyWindow ran on Window destructor");
}

void testWindowCreateNativeFalseStillNull() {
    fuse::platform::WindowDesc desc;
    desc.createNative = false;
    fuse::platform::Window window(desc);
    expectTrue(window.isValid(), "createNative=false window is valid");
    expectTrue(window.nativeHandle().value == nullptr, "createNative=false HWND stays null");
    expectTrue(!window.vulkanSurfaceWire().presentable,
               "createNative=false surface is not presentable");
}

void testWindowDefaultConstructorStillNull() {
    fuse::platform::Window window;
    expectTrue(window.isValid(), "default window is valid");
    expectTrue(window.nativeHandle().value == nullptr, "default window HWND stays null");
}

void testWindowDescriptionRoundTripsCreateNative() {
    fuse::platform::WindowDesc enabled;
    enabled.createNative = true;
    fuse::platform::Window nativeWindow(enabled);
    expectTrue(nativeWindow.description().createNative, "description round-trips createNative true");

    fuse::platform::WindowDesc disabled;
    disabled.createNative = false;
    fuse::platform::Window headless(disabled);
    expectTrue(!headless.description().createNative, "description round-trips createNative false");

    fuse::platform::Window defaultWindow;
    expectTrue(!defaultWindow.description().createNative,
               "default description createNative stays false");
}

void testEventPumpWin32MessageOnlyHwndMapsKeyDown() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    const wchar_t* className = L"FUSE_EventPumpMsgOnly";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;

    ATOM atom = RegisterClassW(&wc);
    if (atom == 0) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            expectTrue(false, "RegisterClassW for message-only HWND");
            return;
        }
    }

    HWND hwnd = CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) {
        std::fprintf(stderr, "SKIP: message-only HWND unavailable (GetLastError=%lu)\n",
                     static_cast<unsigned long>(GetLastError()));
        return;
    }

    window.setNativeHandleForPump(hwnd);
    expectTrue(window.nativeHandle().value == static_cast<void*>(hwnd), "pump handle stores HWND");

    pump.processOsEvents();
    expectTrue(!pump.hasPendingEvents(), "idle HWND pump enqueues nothing");

    const BOOL posted = PostMessageW(hwnd, WM_KEYDOWN, 'A', 0);
    expectTrue(posted != FALSE, "PostMessageW WM_KEYDOWN");

    pump.processOsEvents();

    fuse::platform::PlatformEvent event;
    expectTrue(pump.pollEvent(event), "WM_KEYDOWN mapped after processOsEvents");
    expectTrue(event.type == fuse::platform::PlatformEventType::KeyDown, "mapped KeyDown type");
    expectEq(event.keyCode, static_cast<fuse::u32>('A'), "mapped KeyDown virtual key");
    expectTrue(event.window == &window, "mapped KeyDown bound to pumped window");
    expectTrue(!pump.hasPendingEvents(), "mapped KeyDown consumed");

    window.setNativeHandleForPump(nullptr);
    DestroyWindow(hwnd);
    UnregisterClassW(className, GetModuleHandleW(nullptr));
}

void testEventPumpWin32RequireCaptureDropsReleasedInput() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    expectTrue(!window.isInputCaptured(), "HWND pump window starts Released");
    pump.setRequireCaptureForInput(true);

    const wchar_t* className = L"FUSE_EventPumpCapture";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;

    ATOM atom = RegisterClassW(&wc);
    if (atom == 0) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            pump.setRequireCaptureForInput(false);
            expectTrue(false, "RegisterClassW for capture HWND");
            return;
        }
    }

    HWND hwnd = CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) {
        pump.setRequireCaptureForInput(false);
        std::fprintf(stderr, "SKIP: capture HWND unavailable (GetLastError=%lu)\n",
                     static_cast<unsigned long>(GetLastError()));
        return;
    }

    window.setNativeHandleForPump(hwnd);

    const BOOL postedReleased = PostMessageW(hwnd, WM_KEYDOWN, 'A', 0);
    expectTrue(postedReleased != FALSE, "PostMessageW WM_KEYDOWN while Released");
    pump.processOsEvents();
    expectTrue(!pump.hasPendingEvents(), "Released window drops OS KeyDown when capture required");

    const BOOL postedMouse = PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(4, 8));
    expectTrue(postedMouse != FALSE, "PostMessageW WM_MOUSEMOVE while Released");
    pump.processOsEvents();
    expectTrue(!pump.hasPendingEvents(), "Released window drops OS MouseMove when capture required");

    window.setInputCapture(fuse::platform::InputCaptureMode::Captured);
    const BOOL postedCaptured = PostMessageW(hwnd, WM_KEYDOWN, 'B', 0);
    expectTrue(postedCaptured != FALSE, "PostMessageW WM_KEYDOWN while Captured");
    pump.processOsEvents();

    fuse::platform::PlatformEvent event;
    expectTrue(pump.pollEvent(event) && event.type == fuse::platform::PlatformEventType::InputCaptureChanged &&
                   event.window == &window && event.inputCaptured,
               "capture change reported (InputCaptureChanged) ahead of the OS input");
    expectTrue(pump.pollEvent(event), "Captured window enqueues OS KeyDown");
    expectTrue(event.type == fuse::platform::PlatformEventType::KeyDown, "captured OS KeyDown type");
    expectEq(event.keyCode, static_cast<fuse::u32>('B'), "captured OS KeyDown virtual key");
    expectTrue(event.window == &window, "captured OS KeyDown bound to window");
    expectTrue(!pump.hasPendingEvents(), "captured OS KeyDown consumed");

    window.setNativeHandleForPump(nullptr);
    DestroyWindow(hwnd);
    UnregisterClassW(className, GetModuleHandleW(nullptr));
    pump.setRequireCaptureForInput(false);
}

void testEventPumpWin32AppVisibilityMessages() {
    using fuse::platform::AppVisibility;

    const auto activated = fuse::platform::visibilityFromWin32(WM_ACTIVATEAPP, TRUE);
    const auto deactivated = fuse::platform::visibilityFromWin32(WM_ACTIVATEAPP, FALSE);
    const auto minimized = fuse::platform::visibilityFromWin32(WM_SIZE, SIZE_MINIMIZED);
    const auto restored = fuse::platform::visibilityFromWin32(WM_SIZE, SIZE_RESTORED);
    const auto maximized = fuse::platform::visibilityFromWin32(WM_SIZE, SIZE_MAXIMIZED);
    const auto maxShow = fuse::platform::visibilityFromWin32(WM_SIZE, SIZE_MAXSHOW);
    const auto maxHide = fuse::platform::visibilityFromWin32(WM_SIZE, SIZE_MAXHIDE);

    expectTrue(activated.has_value() && *activated == AppVisibility::Foreground,
               "WM_ACTIVATEAPP TRUE -> Foreground");
    expectTrue(deactivated.has_value() && *deactivated == AppVisibility::Background,
               "WM_ACTIVATEAPP FALSE -> Background");
    expectTrue(minimized.has_value() && *minimized == AppVisibility::Background,
               "WM_SIZE SIZE_MINIMIZED -> Background");
    expectTrue(restored.has_value() && *restored == AppVisibility::Foreground,
               "WM_SIZE SIZE_RESTORED -> Foreground");
    expectTrue(maximized.has_value() && *maximized == AppVisibility::Foreground,
               "WM_SIZE SIZE_MAXIMIZED -> Foreground");
    expectTrue(maxShow.has_value() && *maxShow == AppVisibility::Foreground,
               "WM_SIZE SIZE_MAXSHOW -> Foreground");
    expectTrue(maxHide.has_value() && *maxHide == AppVisibility::Foreground,
               "WM_SIZE SIZE_MAXHIDE -> Foreground");
    expectTrue(!fuse::platform::visibilityFromWin32(WM_KEYDOWN, static_cast<WPARAM>('A')).has_value(),
               "WM_KEYDOWN does not map to visibility");
    expectTrue(!fuse::platform::visibilityFromWin32(WM_SETFOCUS, 0).has_value(),
               "WM_SETFOCUS does not map to visibility");
    expectTrue(!fuse::platform::visibilityFromWin32(WM_KILLFOCUS, 0).has_value(),
               "WM_KILLFOCUS does not map to visibility");

    // Message-only HWND: PostMessage reaches the pump with no display.
    fuse::platform::notifyAppVisibility(AppVisibility::Foreground);

    fuse::platform::EventPump pump;
    fuse::platform::Window window;
    expectTrue(window.isFocused(), "pump window starts focused");

    const wchar_t* className = L"FUSE_EventPumpVisibility";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;

    ATOM atom = RegisterClassW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        expectTrue(false, "RegisterClassW for visibility HWND");
        return;
    }

    HWND hwnd = CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) {
        std::fprintf(stderr, "SKIP: visibility HWND unavailable (GetLastError=%lu)\n",
                     static_cast<unsigned long>(GetLastError()));
        return;
    }

    window.setNativeHandleForPump(hwnd);

    expectTrue(PostMessageW(hwnd, WM_ACTIVATEAPP, FALSE, 0) != FALSE, "PostMessageW WM_ACTIVATEAPP FALSE");
    expectTrue(PostMessageW(hwnd, WM_KEYDOWN, static_cast<WPARAM>('A'), 0) != FALSE,
               "PostMessageW WM_KEYDOWN after deactivate");
    pump.processOsEvents();
    expectTrue(fuse::platform::getAppVisibility() == AppVisibility::Background,
               "WM_ACTIVATEAPP FALSE backgrounds the app");

    fuse::platform::PlatformEvent event;
    expectTrue(pump.pollEvent(event), "key still queued after visibility message");
    expectTrue(event.type == fuse::platform::PlatformEventType::KeyDown, "key type preserved");
    expectEq(event.keyCode, static_cast<fuse::u32>('A'), "WM_KEYDOWN keyCode preserved");
    expectTrue(!pump.hasPendingEvents(), "visibility message did not enqueue an input event");

    expectTrue(PostMessageW(hwnd, WM_ACTIVATEAPP, TRUE, 0) != FALSE, "PostMessageW WM_ACTIVATEAPP TRUE");
    pump.processOsEvents();
    expectTrue(fuse::platform::getAppVisibility() == AppVisibility::Foreground,
               "WM_ACTIVATEAPP TRUE foregrounds the app");
    expectTrue(!pump.hasPendingEvents(), "WM_ACTIVATEAPP enqueues no platform event");

    expectTrue(PostMessageW(hwnd, WM_SIZE, SIZE_MINIMIZED, 0) != FALSE, "PostMessageW WM_SIZE minimized");
    pump.processOsEvents();
    expectTrue(fuse::platform::getAppVisibility() == AppVisibility::Background,
               "WM_SIZE SIZE_MINIMIZED backgrounds the focused window");
    expectTrue(window.isFocused(), "minimize does not clear focus");
    expectTrue(!pump.hasPendingEvents(), "SIZE_MINIMIZED enqueues no resize");

    expectTrue(PostMessageW(hwnd, WM_KILLFOCUS, 0, 0) != FALSE, "PostMessageW WM_KILLFOCUS");
    pump.processOsEvents();
    expectTrue(pump.pollEvent(event) && event.type == fuse::platform::PlatformEventType::WindowFocusLost,
               "WM_KILLFOCUS still reports focus loss");
    expectTrue(!window.isFocused(), "WM_KILLFOCUS clears focus");
    expectTrue(fuse::platform::getAppVisibility() == AppVisibility::Background,
               "WM_KILLFOCUS leaves the app backgrounded");
    expectTrue(!pump.hasPendingEvents(), "WM_KILLFOCUS enqueues only focus loss");

    expectTrue(PostMessageW(hwnd, WM_SIZE, SIZE_RESTORED, 0) != FALSE,
               "PostMessageW WM_SIZE restored while unfocused");
    pump.processOsEvents();
    expectTrue(fuse::platform::getAppVisibility() == AppVisibility::Background,
               "non-minimized WM_SIZE while unfocused stays Background");
    expectTrue(!pump.hasPendingEvents(), "zero-size WM_SIZE enqueues no resize");

    expectTrue(PostMessageW(hwnd, WM_SETFOCUS, 0, 0) != FALSE, "PostMessageW WM_SETFOCUS");
    pump.processOsEvents();
    expectTrue(pump.pollEvent(event) && event.type == fuse::platform::PlatformEventType::WindowFocusGained,
               "WM_SETFOCUS still reports focus gain");
    expectTrue(window.isFocused(), "WM_SETFOCUS sets focus");
    expectTrue(fuse::platform::getAppVisibility() == AppVisibility::Background,
               "WM_SETFOCUS does not foreground by itself");
    expectTrue(!pump.hasPendingEvents(), "WM_SETFOCUS enqueues only focus gain");

    expectTrue(PostMessageW(hwnd, WM_SIZE, SIZE_RESTORED, 0) != FALSE,
               "PostMessageW WM_SIZE restored while focused");
    pump.processOsEvents();
    expectTrue(fuse::platform::getAppVisibility() == AppVisibility::Foreground,
               "non-minimized WM_SIZE while focused foregrounds the app");
    expectTrue(!pump.hasPendingEvents(), "focused restore enqueues no resize when size is zero");

    expectTrue(PostMessageW(hwnd, WM_SIZE, SIZE_MINIMIZED, 0) != FALSE, "PostMessageW WM_SIZE minimized again");
    pump.processOsEvents();
    expectTrue(fuse::platform::getAppVisibility() == AppVisibility::Background,
               "second SIZE_MINIMIZED backgrounds again");

    expectTrue(PostMessageW(hwnd, WM_SIZE, SIZE_MAXIMIZED, 0) != FALSE,
               "PostMessageW WM_SIZE maximized while focused");
    pump.processOsEvents();
    expectTrue(fuse::platform::getAppVisibility() == AppVisibility::Foreground,
               "SIZE_MAXIMIZED while focused foregrounds the app");

    window.setNativeHandleForPump(nullptr);
    DestroyWindow(hwnd);
    UnregisterClassW(className, GetModuleHandleW(nullptr));
    fuse::platform::notifyAppVisibility(AppVisibility::Foreground);
}
#endif

void testEventPumpPollQueueFifoOrder() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    pump.pushWindowResized(window);
    pump.pushWindowFocusLost(window);
    pump.pushWindowFocusGained(window);
    pump.pushWindowCloseRequested(window);
    pump.requestQuit();

    expectEq(pump.pendingEventCount(), 5u, "five distinct events queued");

    const fuse::platform::PlatformEventType expectedOrder[] = {
        fuse::platform::PlatformEventType::WindowResized,
        fuse::platform::PlatformEventType::WindowFocusLost,
        fuse::platform::PlatformEventType::WindowFocusGained,
        fuse::platform::PlatformEventType::WindowCloseRequested,
        fuse::platform::PlatformEventType::Quit,
    };

    for (fuse::u32 index = 0; index < 5u; ++index) {
        fuse::platform::PlatformEvent polled;
        expectTrue(pump.pollEvent(polled), "fifo poll succeeds");
        expectTrue(polled.type == expectedOrder[index], "fifo event type order preserved");
    }

    expectTrue(!pump.hasPendingEvents(), "queue drained");
}

void testEventPumpPollQueueOverflowDropsTail() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    pump.pushWindowResized(window);

    fuse::platform::PlatformEvent marker;
    marker.type = fuse::platform::PlatformEventType::WindowFocusLost;
    marker.window = &window;
    pump.pushSyntheticEvent(marker);

    for (fuse::u32 index = 0; index < 32u; ++index) {
        fuse::platform::PlatformEvent event;
        event.type = fuse::platform::PlatformEventType::WindowFocusGained;
        event.window = &window;
        pump.pushSyntheticEvent(event);
    }

    expectEq(pump.pendingEventCount(), 31u, "ring buffer caps at capacity minus one slot");

    fuse::platform::PlatformEvent first;
    expectTrue(pump.pollEvent(first), "oldest event still available");
    expectTrue(first.type == fuse::platform::PlatformEventType::WindowResized,
               "marker resize event preserved at head");
}

void testEventPumpClearSyntheticEvents() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    pump.pushWindowResized(window);
    pump.pushWindowFocusLost(window);
    expectTrue(pump.hasPendingEvents(), "events queued before clear");

    pump.clearSyntheticEvents();
    expectEq(pump.pendingEventCount(), 0u, "clear empties queue");

    fuse::platform::PlatformEvent event;
    expectTrue(!pump.pollEvent(event), "poll returns false after clear");
}

void testEventPumpCoalesceResizeEvents() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    window.resize(800, 600, &pump);
    window.resize(1024, 768, &pump);
    expectEq(pump.pendingEventCount(), 1u, "duplicate resize coalesces to one event");

    fuse::platform::PlatformEvent coalesced;
    expectTrue(pump.pollEvent(coalesced), "coalesced resize polled");
    expectEq(coalesced.width, 1024u, "coalesced resize keeps latest width");
    expectEq(coalesced.height, 768u, "coalesced resize keeps latest height");

    pump.pushWindowResized(window);
    pump.pushWindowFocusLost(window);
    window.resize(640, 480, &pump);
    expectEq(pump.pendingEventCount(), 2u, "resize coalesces without reordering focus event");

    fuse::platform::PlatformEvent resized;
    expectTrue(pump.pollEvent(resized), "coalesced resize before focus event");
    expectEq(resized.width, 640u, "coalesced resize before focus keeps latest width");
    expectEq(resized.height, 480u, "coalesced resize before focus keeps latest height");

    fuse::platform::PlatformEvent focusLost;
    expectTrue(pump.pollEvent(focusLost), "focus event preserved after resize coalesce");
    expectTrue(focusLost.type == fuse::platform::PlatformEventType::WindowFocusLost,
               "focus event type preserved after resize coalesce");
}

void testEventPumpDrainEventsHelper() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    pump.pushWindowResized(window);
    pump.pushWindowFocusGained(window);
    pump.requestQuit();

    std::vector<fuse::platform::PlatformEvent> drained;
    expectEq(pump.drainEvents(drained), 3u, "drainEvents returns moved count");
    expectEq(drained.size(), 3u, "drainEvents appends all queued events");
    expectTrue(drained[0].type == fuse::platform::PlatformEventType::WindowResized,
               "drain preserves fifo resize first");
    expectTrue(drained[1].type == fuse::platform::PlatformEventType::WindowFocusGained,
               "drain preserves fifo focus second");
    expectTrue(drained[2].type == fuse::platform::PlatformEventType::Quit,
               "drain preserves fifo quit last");
    expectEq(pump.pendingEventCount(), 0u, "drain empties queue");
    expectTrue(pump.quitRequested(), "drain still marks quit when Quit event polled");
}

void testMobileProfileStillUsesWindowStub() {
    const fuse::platform::PlatformProfile previous =
        fuse::platform::setActiveProfileOverride(fuse::platform::PlatformProfile::Mobile);

    fuse::platform::Window window;
    expectTrue(window.isValid(), "mobile profile uses same window stub");
    expectTrue(window.nativeHandle().value == nullptr, "mobile native handle remains null");

    fuse::platform::EventPump pump;
    expectTrue(pump.pumpOnce(), "mobile event pump remains no-op");

    fuse::platform::setActiveProfileOverride(previous);
    fuse::platform::clearActiveProfileOverride();
}

} // namespace

int main() {
    testPollMsOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core platform window tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testTitleLenOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core platform window tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testWindowTitleOk();
    testWindowStubStoresDescription();
    testWindowInputCaptureDefaultReleased();
    testWindowInputCaptureSetCaptured();
    testRequireCaptureForInputDoesNotDropSyntheticEvents();
    testWindowResizeAndCloseRequest();
    testWindowResizeNotifiesPump();
    testWindowFocusEventsNotifyPump();
    testWindowCloseRequestedNotifiesPump();
    testWindowFocusEdgeTransitions();
    testVulkanSurfaceWireIsHeadless();
    testEventPumpSyntheticEvents();
    testEventPumpQuitFlow();
    testEventPumpOsDrainIsNoOp();
    testEventPumpOsDrainDefaultWindowIsNoOp();
#if defined(_WIN32)
    testWindowCreateNativeWin32OwnsHwnd();
    testWindowCreateNativeFalseStillNull();
    testWindowDefaultConstructorStillNull();
    testWindowDescriptionRoundTripsCreateNative();
    testEventPumpWin32MessageOnlyHwndMapsKeyDown();
    testEventPumpWin32RequireCaptureDropsReleasedInput();
    testEventPumpWin32AppVisibilityMessages();
#endif
    testEventPumpPollQueueFifoOrder();
    testEventPumpPollQueueOverflowDropsTail();
    testEventPumpClearSyntheticEvents();
    testEventPumpCoalesceResizeEvents();
    testEventPumpDrainEventsHelper();
    testMobileProfileStillUsesWindowStub();

    if (g_failures == 0) {
        std::printf("fuse_core platform window tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core platform window tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
