// B1.8 platform gate (FUSE_MASTER_PLAN "B1.8 — Phase 1 Deliverables & Test Suite"):
//   "Platform opens a window, displays title, receives and dispatches events cleanly"
//
// Runs against a real X server (Xvfb via run_with_display.sh). Proves on the native X11 backend:
//   * WindowDesc::createNative opens a mapped (IsViewable) X11 window of the requested size;
//   * the title is readable back through XFetchName (WM_NAME) and _NET_WM_NAME (UTF-8), and
//     Window::setTitle updates both;
//   * XSendEvent-injected key / mouse / focus / ConfigureNotify / WM_DELETE_WINDOW events are
//     drained by EventPump::processOsEvents and dispatched as the matching PlatformEvents
//     targeting the right Window (and InputState sees key_pressed for the key);
//   * a real server-side resize (XResizeWindow) and Window::resize both dispatch exactly one
//     WindowResized with the new extent;
//   * the headless path (default Window, no createNative) keeps a null native handle;
//   * B1.8 "Raw mouse delta is unaffected by OS cursor acceleration settings": with strong
//     pointer acceleration configured (XChangePointerControl 10/1, threshold 1) and a
//     per-event, magnitude-dependent gain on the XTEST pointer (XInput2 "Coordinate
//     Transformation Matrix" — the server-side cursor transform Xvfb applies to XTest motion),
//     XTestFakeRelativeMotionEvent device deltas move the real cursor by *different*
//     (accelerated) amounts, while the engine's RawMouseDelta (XI_RawMotion raw_values) equals
//     every injected device delta exactly, per event and in sum, and InputState's mouse delta
//     equals the injected sum even though accelerated MouseMove events are applied alongside.
//     Raw motion is delivered only while the window is captured and focused; with XInput2
//     forced off (FUSE_X11_NO_XI2=1, re-exec'd child) capture falls back to cursor deltas.
// Exit 77 (ctest SKIP) when the X11 backend is not compiled in or no display is reachable.

#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/input.hpp>
#include <fuse/platform/window.hpp>
#include <fuse/platform/window_wsi.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(FUSE_PLATFORM_WINDOW_X11)
// Xlib last: it defines macros (None, KeyPress, FocusIn, ...) that collide with FUSE names.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#if defined(FUSE_TEST_HAS_XTEST_XI2)
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#endif

namespace {

constexpr int kSkip = 77;
int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    } else {
        std::printf("  ok: %s\n", message);
    }
}

#if defined(FUSE_PLATFORM_WINDOW_X11)

using fuse::platform::EventPump;
using fuse::platform::PlatformEvent;
using fuse::platform::PlatformEventType;

::Window xidOf(const fuse::platform::Window& window) {
    return static_cast<::Window>(reinterpret_cast<std::uintptr_t>(window.nativeHandle().value));
}

std::vector<PlatformEvent> pumpNative(Display* dpy, EventPump& pump) {
    XSync(dpy, False);
    pump.processOsEvents();
    std::vector<PlatformEvent> events;
    pump.drainEvents(events);
    return events;
}

fuse::u32 countType(const std::vector<PlatformEvent>& events, PlatformEventType type,
                    const fuse::platform::Window* window) {
    fuse::u32 n = 0;
    for (const PlatformEvent& e : events) {
        if (e.type == type && e.window == window) {
            ++n;
        }
    }
    return n;
}

const PlatformEvent* findType(const std::vector<PlatformEvent>& events, PlatformEventType type) {
    for (const PlatformEvent& e : events) {
        if (e.type == type) {
            return &e;
        }
    }
    return nullptr;
}

std::string fetchName(Display* dpy, ::Window xid) {
    char* name = nullptr;
    std::string out;
    if (XFetchName(dpy, xid, &name) != 0 && name != nullptr) {
        out = name;
        XFree(name);
    }
    return out;
}

std::string netWmName(Display* dpy, ::Window xid) {
    const Atom netWmName = XInternAtom(dpy, "_NET_WM_NAME", False);
    const Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom actualType = 0;
    int actualFormat = 0;
    unsigned long items = 0;
    unsigned long after = 0;
    unsigned char* data = nullptr;
    std::string out;
    if (XGetWindowProperty(dpy, xid, netWmName, 0, 1024, False, utf8, &actualType, &actualFormat,
                           &items, &after, &data) == Success &&
        data != nullptr) {
        if (actualType == utf8 && actualFormat == 8) {
            out.assign(reinterpret_cast<const char*>(data), items);
        }
        XFree(data);
    }
    return out;
}

void sendKey(Display* dpy, ::Window xid, KeySym keysym, bool press) {
    XEvent ev{};
    ev.xkey.type = press ? KeyPress : KeyRelease;
    ev.xkey.display = dpy;
    ev.xkey.window = xid;
    ev.xkey.root = DefaultRootWindow(dpy);
    ev.xkey.time = CurrentTime;
    ev.xkey.same_screen = True;
    ev.xkey.keycode = XKeysymToKeycode(dpy, keysym);
    XSendEvent(dpy, xid, False, press ? KeyPressMask : KeyReleaseMask, &ev);
}

void sendButton(Display* dpy, ::Window xid, unsigned int button, bool press, int x, int y) {
    XEvent ev{};
    ev.xbutton.type = press ? ButtonPress : ButtonRelease;
    ev.xbutton.display = dpy;
    ev.xbutton.window = xid;
    ev.xbutton.root = DefaultRootWindow(dpy);
    ev.xbutton.time = CurrentTime;
    ev.xbutton.same_screen = True;
    ev.xbutton.button = button;
    ev.xbutton.x = x;
    ev.xbutton.y = y;
    XSendEvent(dpy, xid, False, press ? ButtonPressMask : ButtonReleaseMask, &ev);
}

void sendMotion(Display* dpy, ::Window xid, int x, int y) {
    XEvent ev{};
    ev.xmotion.type = MotionNotify;
    ev.xmotion.display = dpy;
    ev.xmotion.window = xid;
    ev.xmotion.root = DefaultRootWindow(dpy);
    ev.xmotion.time = CurrentTime;
    ev.xmotion.same_screen = True;
    ev.xmotion.x = x;
    ev.xmotion.y = y;
    XSendEvent(dpy, xid, False, PointerMotionMask, &ev);
}

void sendFocus(Display* dpy, ::Window xid, bool in) {
    XEvent ev{};
    ev.xfocus.type = in ? FocusIn : FocusOut;
    ev.xfocus.display = dpy;
    ev.xfocus.window = xid;
    ev.xfocus.mode = NotifyNormal;
    ev.xfocus.detail = NotifyNonlinear;
    XSendEvent(dpy, xid, False, FocusChangeMask, &ev);
}

void sendConfigure(Display* dpy, ::Window xid, int width, int height) {
    XEvent ev{};
    ev.xconfigure.type = ConfigureNotify;
    ev.xconfigure.display = dpy;
    ev.xconfigure.event = xid;
    ev.xconfigure.window = xid;
    ev.xconfigure.width = width;
    ev.xconfigure.height = height;
    XSendEvent(dpy, xid, False, StructureNotifyMask, &ev);
}

void sendDeleteWindow(Display* dpy, ::Window xid) {
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.display = dpy;
    ev.xclient.window = xid;
    ev.xclient.message_type = XInternAtom(dpy, "WM_PROTOCOLS", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = static_cast<long>(XInternAtom(dpy, "WM_DELETE_WINDOW", False));
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, xid, False, NoEventMask, &ev);
}

#if defined(FUSE_TEST_HAS_XTEST_XI2)

struct Delta {
    int x;
    int y;
};

// Mixed-sign, mixed-magnitude device deltas (no zero components, so any gain != 1 shows).
constexpr Delta kInjected[] = {{5, 4},  {-3, 6},  {7, -2}, {2, -9}, {-11, -1},
                               {1, 1},  {-6, -7}, {13, 3}, {-1, 12}, {4, -5}};
constexpr int kInjectedCount = static_cast<int>(sizeof(kInjected) / sizeof(kInjected[0]));

int findXtestPointer(Display* dpy) {
    int count = 0;
    XIDeviceInfo* devices = XIQueryDevice(dpy, XIAllDevices, &count);
    int id = -1;
    for (int i = 0; devices != nullptr && i < count; ++i) {
        if (devices[i].use == XISlavePointer && devices[i].name != nullptr &&
            std::strstr(devices[i].name, "XTEST") != nullptr) {
            id = devices[i].deviceid;
            break;
        }
    }
    if (devices != nullptr) {
        XIFreeDeviceInfo(devices);
    }
    return id;
}

// Scales the device's relative motion by (gainX, gainY) server-side (X server applies the CTM
// to relative valuators after emitting the raw event, before moving the cursor).
bool setPointerGain(Display* dpy, int deviceId, float gainX, float gainY) {
    const Atom ctm = XInternAtom(dpy, "Coordinate Transformation Matrix", True);
    const Atom floatAtom = XInternAtom(dpy, "FLOAT", True);
    if (ctm == 0 || floatAtom == 0) {
        return false;
    }
    // XIChangeProperty takes format-32 data as packed 32-bit items (not longs).
    float matrix[9] = {gainX, 0.0f, 0.0f, 0.0f, gainY, 0.0f, 0.0f, 0.0f, 1.0f};
    XIChangeProperty(dpy, deviceId, ctm, floatAtom, 32, PropModeReplace,
                     reinterpret_cast<unsigned char*>(matrix), 9);
    XSync(dpy, False);
    return true;
}

// Emulated acceleration curve: gain grows with the move's magnitude (velocity per event).
// gain >= 2.25 guarantees every integer cursor move differs from its device delta.
float accelGain(int d) {
    const int magnitude = d < 0 ? -d : d;
    return 2.0f + 0.25f * static_cast<float>(magnitude);
}

void rootPointer(Display* dpy, int& x, int& y) {
    ::Window root = 0;
    ::Window child = 0;
    int wx = 0;
    int wy = 0;
    unsigned int mask = 0;
    XQueryPointer(dpy, DefaultRootWindow(dpy), &root, &child, &x, &y, &wx, &wy, &mask);
}

struct InjectResult {
    fuse::u32 rawEvents = 0;
    fuse::u32 rawMismatches = 0;
    fuse::u32 mouseMoves = 0;
    fuse::u32 cursorEqualsInjected = 0;
    long rawSumX = 0;
    long rawSumY = 0;
    long cursorSumX = 0;
    long cursorSumY = 0;
};

// Injects every kInjected delta (optionally accelerated), pumping after each one.
InjectResult injectAndPump(Display* dpy, EventPump& pump, fuse::platform::InputState& input,
                           const fuse::platform::Window& window, int xtestId, bool accelerate,
                           bool verbose) {
    InjectResult r;
    const int winW = static_cast<int>(window.width());
    const int winH = static_cast<int>(window.height());
    // Keep the cursor inside the window (and far from screen edges) so no motion is clamped.
    XWarpPointer(dpy, None, xidOf(window), 0, 0, 0, 0, winW / 2, winH / 2);
    (void)pumpNative(dpy, pump);

    for (int i = 0; i < kInjectedCount; ++i) {
        const Delta d = kInjected[i];
        if (accelerate) {
            setPointerGain(dpy, xtestId, accelGain(d.x), accelGain(d.y));
        }
        int beforeX = 0;
        int beforeY = 0;
        rootPointer(dpy, beforeX, beforeY);
        XTestFakeRelativeMotionEvent(dpy, d.x, d.y, CurrentTime);
        XSync(dpy, False);
        int afterX = 0;
        int afterY = 0;
        rootPointer(dpy, afterX, afterY);
        const int cursorX = afterX - beforeX;
        const int cursorY = afterY - beforeY;
        r.cursorSumX += cursorX;
        r.cursorSumY += cursorY;
        if (cursorX == d.x && cursorY == d.y) {
            ++r.cursorEqualsInjected;
        }

        std::vector<PlatformEvent> events = pumpNative(dpy, pump);
        fuse::u32 rawThisEvent = 0;
        for (const PlatformEvent& e : events) {
            input.apply(e);
            if (e.type == PlatformEventType::MouseMove && e.window == &window) {
                ++r.mouseMoves;
            }
            if (e.type != PlatformEventType::RawMouseDelta) {
                continue;
            }
            ++rawThisEvent;
            ++r.rawEvents;
            r.rawSumX += e.mouseX;
            r.rawSumY += e.mouseY;
            if (e.window != &window || e.mouseX != d.x || e.mouseY != d.y) {
                ++r.rawMismatches;
            }
            if (verbose) {
                std::printf("    inject (%3d,%3d)  cursor moved (%4d,%4d)  raw (%3d,%3d)\n", d.x, d.y,
                            cursorX, cursorY, e.mouseX, e.mouseY);
            }
        }
        if (verbose && rawThisEvent == 0u) {
            std::printf("    inject (%3d,%3d)  cursor moved (%4d,%4d)  raw -\n", d.x, d.y, cursorX,
                        cursorY);
        }
        if (rawThisEvent > 1u) {
            ++r.rawMismatches; // one XTest motion must yield exactly one raw delta
        }
    }
    if (accelerate) {
        setPointerGain(dpy, xtestId, 1.0f, 1.0f);
    }
    return r;
}

int runRawMouseFallbackChild() {
    // Child mode (FUSE_X11_NO_XI2=1): XInput2 is forced off, so capture must fall back to the
    // MotionNotify cursor path without raw events and without breaking anything.
    auto* dpy = static_cast<Display*>(fuse::platform::nativeDisplayHandle());
    if (dpy == nullptr) {
        return kSkip;
    }
    expectTrue(!fuse::platform::rawMouseInputAvailable(),
               "fallback: FUSE_X11_NO_XI2=1 -> rawMouseInputAvailable() is false");
    fuse::platform::WindowDesc desc{};
    desc.title = "FUSE B1 X11 raw fallback";
    desc.width = 640;
    desc.height = 480;
    desc.createNative = true;
    fuse::platform::Window window(desc);
    EventPump pump;
    (void)pumpNative(dpy, pump);
    window.setInputCapture(fuse::platform::InputCaptureMode::Captured);
    fuse::platform::InputState input;
    input.beginFrame();
    const int xtestId = findXtestPointer(dpy);
    const InjectResult r = injectAndPump(dpy, pump, input, window, xtestId, false, false);
    expectTrue(r.rawEvents == 0u, "fallback: no RawMouseDelta without XInput2");
    expectTrue(r.mouseMoves > 0u, "fallback: captured window still receives MouseMove");
    expectTrue(!input.rawMouseActive(), "fallback: InputState stays on the cursor-delta path");
    long injectedX = 0;
    long injectedY = 0;
    for (const Delta& d : kInjected) {
        injectedX += d.x;
        injectedY += d.y;
    }
    expectTrue(input.mouseDeltaX() == injectedX - kInjected[0].x &&
                   input.mouseDeltaY() == injectedY - kInjected[0].y,
               "fallback: cursor-delta path accumulates the (unaccelerated) cursor moves");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

void runRawMouseGates(Display* dpy, fuse::platform::Window& window, EventPump& pump,
                      const char* selfExe) {
    std::printf("  -- raw mouse vs OS cursor acceleration (XInput2 XI_RawMotion + XTest) --\n");
    int xtestEvent = 0;
    int xtestError = 0;
    int xtestMajor = 0;
    int xtestMinor = 0;
    if (XTestQueryExtension(dpy, &xtestEvent, &xtestError, &xtestMajor, &xtestMinor) == False) {
        std::printf("  SKIP raw mouse section: server has no XTEST extension\n");
        return;
    }
    int xiOpcode = 0;
    int xiEvent = 0;
    int xiError = 0;
    const bool serverHasXi = XQueryExtension(dpy, "XInputExtension", &xiOpcode, &xiEvent, &xiError) != False;
#if defined(FUSE_PLATFORM_X11_XI2)
    expectTrue(serverHasXi, "X server exposes XInputExtension");
    expectTrue(fuse::platform::rawMouseInputAvailable(),
               "rawMouseInputAvailable(): XInput2 raw motion usable on this display");
#else
    std::printf("  SKIP raw mouse section: fuse_core built without libXi (FUSE_PLATFORM_X11_XI2)\n");
    (void)serverHasXi;
    return;
#endif
    const int xtestId = findXtestPointer(dpy);
    expectTrue(xtestId > 0, "found the 'Virtual core XTEST pointer' slave device");
    if (xtestId <= 0) {
        return;
    }

    // OS acceleration settings: classic X pointer acceleration, as strong as it goes.
    int oldNum = 0;
    int oldDen = 0;
    int oldThreshold = 0;
    XGetPointerControl(dpy, &oldNum, &oldDen, &oldThreshold);
    XChangePointerControl(dpy, True, True, 10, 1, 1);
    int num = 0;
    int den = 0;
    int threshold = 0;
    XGetPointerControl(dpy, &num, &den, &threshold);
    expectTrue(num == 10 && den == 1 && threshold == 1,
               "XChangePointerControl: acceleration 10/1, threshold 1 applied");

    window.setFocused(true, nullptr);
    fuse::platform::InputState input;

    // 1) Uncaptured: raw motion is not selected, so nothing raw reaches the engine.
    input.beginFrame();
    InjectResult r = injectAndPump(dpy, pump, input, window, xtestId, true, false);
    expectTrue(r.rawEvents == 0u, "released capture: no RawMouseDelta events");
    expectTrue(r.mouseMoves > 0u, "released capture: XTest motion still yields MouseMove");

    // 2) Captured + accelerated cursor: raw delta == injected device delta, exactly.
    window.setInputCapture(fuse::platform::InputCaptureMode::Captured);
    input.beginFrame();
    r = injectAndPump(dpy, pump, input, window, xtestId, true, true);
    long injectedX = 0;
    long injectedY = 0;
    for (const Delta& d : kInjected) {
        injectedX += d.x;
        injectedY += d.y;
    }
    std::printf("    sum: injected (%ld,%ld)  cursor (%ld,%ld)  raw (%ld,%ld)  InputState (%d,%d)\n",
                injectedX, injectedY, r.cursorSumX, r.cursorSumY, r.rawSumX, r.rawSumY,
                input.mouseDeltaX(), input.mouseDeltaY());
    expectTrue(r.cursorEqualsInjected == 0u,
               "every cursor move is accelerated (differs from the injected device delta)");
    expectTrue(r.cursorSumX != injectedX && r.cursorSumY != injectedY,
               "accumulated cursor displacement differs from the injected sum");
    expectTrue(r.rawEvents == static_cast<fuse::u32>(kInjectedCount),
               "captured: exactly one RawMouseDelta per injected XTest motion");
    expectTrue(r.rawMismatches == 0u,
               "captured: each RawMouseDelta equals its injected device delta and targets the window");
    expectTrue(r.rawSumX == injectedX && r.rawSumY == injectedY,
               "captured: summed raw delta equals the summed injected device deltas");
    expectTrue(r.mouseMoves > 0u, "captured: accelerated MouseMove events were applied alongside");
    expectTrue(input.rawMouseActive(), "InputState switched to raw mouse input");
    expectTrue(input.mouseDeltaX() == injectedX && input.mouseDeltaY() == injectedY,
               "InputState mouse delta equals the injected sum (acceleration never leaks in)");

    // 3) Captured but unfocused: global raw motion is not delivered (Win32 foreground parity).
    window.setFocused(false, nullptr);
    input.beginFrame();
    r = injectAndPump(dpy, pump, input, window, xtestId, true, false);
    expectTrue(r.rawEvents == 0u, "captured but unfocused: no RawMouseDelta");
    window.setFocused(true, nullptr);

    // 4) Released again: XI_RawMotion is deselected.
    window.setInputCapture(fuse::platform::InputCaptureMode::Released);
    input.beginFrame();
    r = injectAndPump(dpy, pump, input, window, xtestId, true, false);
    expectTrue(r.rawEvents == 0u, "capture released: RawMouseDelta stops");

    XChangePointerControl(dpy, True, True, oldNum, oldDen, oldThreshold);
    XSync(dpy, False);

    // 5) Clean fallback without XInput2: re-exec this binary with FUSE_X11_NO_XI2=1.
    if (selfExe != nullptr) {
        std::fflush(stdout);
        const pid_t child = fork();
        if (child == 0) {
            setenv("FUSE_X11_NO_XI2", "1", 1);
            char* const args[] = {const_cast<char*>(selfExe), const_cast<char*>("--raw-fallback-child"),
                                  nullptr};
            execv("/proc/self/exe", args);
            _exit(127);
        }
        int status = 0;
        const bool waited = child > 0 && waitpid(child, &status, 0) == child;
        expectTrue(waited && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                   "XInput2 disabled (FUSE_X11_NO_XI2=1 child): capture falls back cleanly");
    }
}

#endif // FUSE_TEST_HAS_XTEST_XI2

int runGates(const char* selfExe) {
    expectTrue(fuse::platform::activeWindowWsiKind() == fuse::platform::WindowWsiKind::X11,
               "X11 window backend is the compiled-in WSI");
    if (!fuse::platform::windowWsiAvailable()) {
        std::printf("SKIP: no X display reachable (DISPLAY unset / server down)\n");
        return kSkip;
    }
    auto* dpy = static_cast<Display*>(fuse::platform::nativeDisplayHandle());
    expectTrue(dpy != nullptr, "native display handle is a live Display*");
    std::printf("  display: %s (%s)\n", DisplayString(dpy), fuse::platform::windowWsiBackendName());

    // Headless path stays native-handle-less even with a display available.
    {
        fuse::platform::Window headless;
        expectTrue(headless.nativeHandle().value == nullptr,
                   "default Window (no createNative) keeps a null native handle");
        expectTrue(!headless.vulkanSurfaceWire().presentable, "default Window is not presentable");
    }

    fuse::platform::WindowDesc desc{};
    desc.title = "FUSE B1 X11 Gate";
    desc.width = 640;
    desc.height = 360;
    desc.createNative = true;
    fuse::platform::Window window(desc);
    expectTrue(window.isValid(), "native Window is valid");
    expectTrue(window.nativeHandle().value != nullptr, "createNative opens an X11 window");
    expectTrue(window.vulkanSurfaceWire().presentable, "native Window is presentable");
    const ::Window xid = xidOf(window);
    if (xid == 0) {
        return EXIT_FAILURE;
    }

    XWindowAttributes attrs{};
    XSync(dpy, False);
    expectTrue(XGetWindowAttributes(dpy, xid, &attrs) != 0, "X server knows the window");
    expectTrue(attrs.map_state == IsViewable, "window is mapped (IsViewable)");
    expectTrue(attrs.width == 640 && attrs.height == 360, "window geometry is 640x360");

    // --- title read-back -------------------------------------------------------------------
    expectTrue(fetchName(dpy, xid) == "FUSE B1 X11 Gate", "XFetchName returns the WindowDesc title");
    expectTrue(netWmName(dpy, xid) == "FUSE B1 X11 Gate", "_NET_WM_NAME returns the WindowDesc title");
    const char* renamed = "FUSE \xE2\x80\x94 Gate \xC3\xBC"; // "FUSE — Gate ü" (UTF-8)
    window.setTitle(renamed);
    XSync(dpy, False);
    expectTrue(netWmName(dpy, xid) == renamed, "setTitle updates _NET_WM_NAME (UTF-8)");
    expectTrue(fetchName(dpy, xid) == renamed, "setTitle updates WM_NAME");
    expectTrue(std::strcmp(window.description().title, renamed) == 0, "Window stores the new title");

    EventPump pump;
    (void)pumpNative(dpy, pump); // drain map/expose/initial configure

    // --- keys ----------------------------------------------------------------------------
    fuse::platform::InputState input;
    input.beginFrame();
    sendKey(dpy, xid, XK_a, true);
    sendKey(dpy, xid, XK_Escape, true);
    std::vector<PlatformEvent> events = pumpNative(dpy, pump);
    expectTrue(countType(events, PlatformEventType::KeyDown, &window) == 2u,
               "XSendEvent KeyPress x2 -> two KeyDown events for this window");
    const PlatformEvent* keyDown = findType(events, PlatformEventType::KeyDown);
    expectTrue(keyDown != nullptr && keyDown->keyCode == static_cast<fuse::u32>('A'),
               "KeyPress XK_a maps to keyCode 'A'");
    expectTrue(events.size() >= 2u && events[1].keyCode == 0x1Bu, "KeyPress XK_Escape maps to 0x1B");
    for (const PlatformEvent& e : events) {
        input.apply(e);
    }
    expectTrue(input.keyPressed(fuse::platform::Key::A), "InputState reports key_pressed(A)");
    expectTrue(input.keyPressed(fuse::platform::Key::Escape), "InputState reports key_pressed(Escape)");

    sendKey(dpy, xid, XK_a, false);
    events = pumpNative(dpy, pump);
    expectTrue(countType(events, PlatformEventType::KeyUp, &window) == 1u &&
                   events[0].keyCode == static_cast<fuse::u32>('A'),
               "XSendEvent KeyRelease -> KeyUp 'A'");

    // --- mouse ---------------------------------------------------------------------------
    sendMotion(dpy, xid, 30, 40);
    sendButton(dpy, xid, Button3, true, 10, 20);
    sendButton(dpy, xid, Button3, false, 10, 20);
    sendButton(dpy, xid, Button4, true, 10, 20);
    events = pumpNative(dpy, pump);
    expectTrue(events.size() == 4u, "motion + button press/release + wheel -> 4 events");
    if (events.size() == 4u) {
        expectTrue(events[0].type == PlatformEventType::MouseMove && events[0].mouseX == 30 &&
                       events[0].mouseY == 40,
                   "MotionNotify -> MouseMove(30,40)");
        expectTrue(events[1].type == PlatformEventType::MouseButtonDown &&
                       events[1].mouseButton == 2u && events[1].mouseX == 10 &&
                       events[1].mouseY == 20,
                   "ButtonPress(Button3) -> MouseButtonDown right @ (10,20)");
        expectTrue(events[2].type == PlatformEventType::MouseButtonUp && events[2].mouseButton == 2u,
                   "ButtonRelease(Button3) -> MouseButtonUp right");
        expectTrue(events[3].type == PlatformEventType::MouseMove && events[3].mouseY == 120,
                   "Button4 -> wheel delta +120 (Win32 WM_MOUSEWHEEL mapping)");
    }

    // --- focus ---------------------------------------------------------------------------
    sendFocus(dpy, xid, false);
    sendFocus(dpy, xid, true);
    events = pumpNative(dpy, pump);
    expectTrue(countType(events, PlatformEventType::WindowFocusLost, &window) == 1u,
               "FocusOut -> WindowFocusLost");
    expectTrue(countType(events, PlatformEventType::WindowFocusGained, &window) == 1u,
               "FocusIn -> WindowFocusGained");
    expectTrue(window.isFocused(), "window is focused after FocusIn");

    // --- resize --------------------------------------------------------------------------
    sendConfigure(dpy, xid, 800, 600);
    events = pumpNative(dpy, pump);
    const PlatformEvent* resized = findType(events, PlatformEventType::WindowResized);
    expectTrue(resized != nullptr && resized->window == &window && resized->width == 800u &&
                   resized->height == 600u,
               "synthetic ConfigureNotify -> WindowResized 800x600");
    expectTrue(window.width() == 800u && window.height() == 600u, "Window extent tracks ConfigureNotify");

    XResizeWindow(dpy, xid, 500, 400);
    events = pumpNative(dpy, pump);
    resized = findType(events, PlatformEventType::WindowResized);
    expectTrue(countType(events, PlatformEventType::WindowResized, &window) == 1u && resized != nullptr &&
                   resized->width == 500u && resized->height == 400u,
               "server-side XResizeWindow -> exactly one WindowResized 500x400");

    window.resize(1024, 768, &pump);
    events = pumpNative(dpy, pump);
    expectTrue(countType(events, PlatformEventType::WindowResized, &window) == 1u,
               "Window::resize dispatches exactly one WindowResized (echo is deduplicated)");
    XSync(dpy, False);
    XGetWindowAttributes(dpy, xid, &attrs);
    expectTrue(attrs.width == 1024 && attrs.height == 768, "Window::resize resizes the real X window");

    // --- close ---------------------------------------------------------------------------
    sendDeleteWindow(dpy, xid);
    events = pumpNative(dpy, pump);
    expectTrue(countType(events, PlatformEventType::WindowCloseRequested, &window) == 1u,
               "WM_DELETE_WINDOW ClientMessage -> WindowCloseRequested");
    expectTrue(window.closeRequest() == fuse::platform::WindowCloseRequest::Requested,
               "Window records the close request");

    // Capture gate: with requireCaptureForInput, key/mouse are dropped until captured.
    pump.setRequireCaptureForInput(true);
    sendKey(dpy, xid, XK_b, true);
    events = pumpNative(dpy, pump);
    expectTrue(countType(events, PlatformEventType::KeyDown, &window) == 0u,
               "requireCaptureForInput drops OS keys for an uncaptured window");
    pump.setRequireCaptureForInput(false);

#if defined(FUSE_TEST_HAS_XTEST_XI2)
    window.clearCloseRequest();
    runRawMouseGates(dpy, window, pump, selfExe);
#else
    (void)selfExe;
    std::printf("  SKIP raw mouse section: test built without libXtst/libXi\n");
#endif

    return EXIT_SUCCESS;
}

#endif // FUSE_PLATFORM_WINDOW_X11

} // namespace

int main(int argc, char** argv) {
#if !defined(FUSE_PLATFORM_WINDOW_X11)
    (void)argc;
    (void)argv;
    std::printf("SKIP: X11 window backend not compiled in (FUSE_PLATFORM_WINDOW_X11 off / GLFW active)\n");
    return kSkip;
#else
#if defined(FUSE_TEST_HAS_XTEST_XI2)
    if (argc > 1 && std::strcmp(argv[1], "--raw-fallback-child") == 0) {
        return runRawMouseFallbackChild();
    }
#endif
    const int rc = runGates(argc > 0 ? argv[0] : nullptr);
    if (rc == kSkip) {
        return kSkip;
    }
    if (g_failures == 0 && rc == EXIT_SUCCESS) {
        std::printf("fuse_core_b1_x11_window_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_core_b1_x11_window_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
#endif
}
