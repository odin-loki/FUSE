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
//   * the headless path (default Window, no createNative) keeps a null native handle.
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

int runGates() {
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

    return EXIT_SUCCESS;
}

#endif // FUSE_PLATFORM_WINDOW_X11

} // namespace

int main() {
#if !defined(FUSE_PLATFORM_WINDOW_X11)
    std::printf("SKIP: X11 window backend not compiled in (FUSE_PLATFORM_WINDOW_X11 off / GLFW active)\n");
    return kSkip;
#else
    const int rc = runGates();
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
