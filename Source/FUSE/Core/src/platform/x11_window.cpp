// Native X11 (Xlib) window backend — selected when GLFW is unavailable on Linux/BSD.
// See x11_window.hpp for why Xlib stays confined to this translation unit.

#include "x11_window.hpp"

#if defined(FUSE_PLATFORM_WINDOW_X11)

#include <fuse/platform/lifecycle.hpp>

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#if defined(FUSE_PLATFORM_X11_XI2)
#include <X11/extensions/XInput2.h>
#endif

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>

#if defined(FUSE_PLATFORM_X11_VULKAN)
#ifndef VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_XLIB_KHR 1
#endif
#include <vulkan/vulkan.h>
#endif

namespace fuse::platform::x11 {

namespace {

Display* g_display = nullptr;
bool g_displayTried = false;
Atom g_wmProtocols = 0;
Atom g_wmDeleteWindow = 0;
Atom g_netWmName = 0;
Atom g_utf8String = 0;

// --- UI1 text services state ---
XIM g_im = nullptr;
XIMStyle g_imStyle = 0;
bool g_imTried = false;

constexpr int kMaxPreedit = 128; // characters
struct WindowIc {
    ::Window xid = 0;
    XIC ic = nullptr;
    u32 preedit[kMaxPreedit] = {};
    i32 preeditLength = 0;
    i32 caret = 0;
};
constexpr int kMaxIcWindows = 16;
WindowIc g_ics[kMaxIcWindows];

constexpr int kPendingEvents = 16;
Event g_pending[kPendingEvents];
int g_pendingHead = 0;
int g_pendingCount = 0;

void queueEvent(const Event& e) {
    if (g_pendingCount == kPendingEvents) { // drop the oldest: pre-edit updates supersede each other
        g_pendingHead = (g_pendingHead + 1) % kPendingEvents;
        --g_pendingCount;
    }
    g_pending[(g_pendingHead + g_pendingCount) % kPendingEvents] = e;
    ++g_pendingCount;
}

::Window g_clipWindow = 0;
Atom g_atomClipboard = 0, g_atomTargets = 0, g_atomText = 0, g_atomIncr = 0, g_atomClipProp = 0;
std::vector<char> g_clipText;
bool g_clipOwned = false;

void createInputContext(::Window xid);
void destroyInputContext(::Window xid);
bool handleSelectionEvent(XEvent& xe);

constexpr long kEventMask = StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                            ButtonReleaseMask | PointerMotionMask | FocusChangeMask |
                            ExposureMask;

// Xlib's default error handler calls exit(); a stale/foreign XID must not kill the engine.
int nonFatalErrorHandler(Display* /*display*/, XErrorEvent* /*event*/) {
    return 0;
}

::Window toXid(void* window) {
    return static_cast<::Window>(reinterpret_cast<std::uintptr_t>(window));
}

void* fromXid(::Window window) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(window));
}

bool hasDisplayEnv() {
    const char* display = std::getenv("DISPLAY");
    return display != nullptr && display[0] != '\0';
}

} // namespace

bool ensureDisplay() {
    if (g_displayTried) {
        return g_display != nullptr;
    }
    g_displayTried = true;

    if (!hasDisplayEnv()) {
        return false;
    }

    // Mesa's X11 WSI drives the same connection from its present thread.
    XInitThreads();
    g_display = XOpenDisplay(nullptr);
    if (g_display == nullptr) {
        return false;
    }

    XSetErrorHandler(nonFatalErrorHandler);
    Bool detectable = False;
    XkbSetDetectableAutoRepeat(g_display, True, &detectable);

    g_wmProtocols = XInternAtom(g_display, "WM_PROTOCOLS", False);
    g_wmDeleteWindow = XInternAtom(g_display, "WM_DELETE_WINDOW", False);
    g_netWmName = XInternAtom(g_display, "_NET_WM_NAME", False);
    g_utf8String = XInternAtom(g_display, "UTF8_STRING", False);
    g_atomClipboard = XInternAtom(g_display, "CLIPBOARD", False);
    g_atomTargets = XInternAtom(g_display, "TARGETS", False);
    g_atomText = XInternAtom(g_display, "TEXT", False);
    g_atomIncr = XInternAtom(g_display, "INCR", False);
    g_atomClipProp = XInternAtom(g_display, "FUSE_CLIPBOARD", False);
    return true;
}

void* display() {
    return ensureDisplay() ? g_display : nullptr;
}

void setTitle(void* window, const char* title) {
    if (!ensureDisplay() || window == nullptr || title == nullptr) {
        return;
    }
    const ::Window xid = toXid(window);
    XStoreName(g_display, xid, title);
    XChangeProperty(g_display, xid, g_netWmName, g_utf8String, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(title),
                    static_cast<int>(std::strlen(title)));
    XFlush(g_display);
}

void* createWindow(u32 width, u32 height, const char* title) {
    if (!ensureDisplay()) {
        return nullptr;
    }

    const int screen = DefaultScreen(g_display);
    const ::Window root = RootWindow(g_display, screen);

    XSetWindowAttributes attributes{};
    attributes.background_pixel = BlackPixel(g_display, screen);
    attributes.event_mask = kEventMask;
    const ::Window xid =
        XCreateWindow(g_display, root, 0, 0, width > 0 ? width : 1u, height > 0 ? height : 1u, 0,
                      CopyFromParent, InputOutput, CopyFromParent, CWBackPixel | CWEventMask,
                      &attributes);
    if (xid == 0) {
        return nullptr;
    }

    Atom protocols[] = {g_wmDeleteWindow};
    XSetWMProtocols(g_display, xid, protocols, 1);

    XClassHint classHint{};
    char resName[] = "fuse";
    char resClass[] = "FUSE";
    classHint.res_name = resName;
    classHint.res_class = resClass;
    XSetClassHint(g_display, xid, &classHint);

    setTitle(fromXid(xid), title != nullptr ? title : "FUSE");
    createInputContext(xid);
    XMapWindow(g_display, xid);
    XSync(g_display, False);
    return fromXid(xid);
}

void destroyWindow(void* window) {
    if (g_display == nullptr || window == nullptr) {
        return;
    }
    destroyInputContext(toXid(window));
    XDestroyWindow(g_display, toXid(window));
    XSync(g_display, False);
}

void resizeWindow(void* window, u32 width, u32 height) {
    if (g_display == nullptr || window == nullptr || width == 0u || height == 0u) {
        return;
    }
    XResizeWindow(g_display, toXid(window), width, height);
    XSync(g_display, False);
}

void selectInput(void* window) {
    if (!ensureDisplay() || window == nullptr) {
        return;
    }
    XSelectInput(g_display, toXid(window), kEventMask);
    createInputContext(toXid(window));
    XFlush(g_display);
}

void flush() {
    if (g_display != nullptr) {
        XFlush(g_display);
    }
}

u32 keysymToVirtualKey(unsigned long keysym) {
    if (keysym >= XK_a && keysym <= XK_z) {
        return static_cast<u32>('A' + (keysym - XK_a));
    }
    if (keysym >= XK_A && keysym <= XK_Z) {
        return static_cast<u32>('A' + (keysym - XK_A));
    }
    if (keysym >= XK_0 && keysym <= XK_9) {
        return static_cast<u32>('0' + (keysym - XK_0));
    }
    if (keysym >= XK_F1 && keysym <= XK_F12) {
        return static_cast<u32>(0x70u + (keysym - XK_F1));
    }

    switch (keysym) {
    case XK_space:
        return 0x20u;
    case XK_Return:
    case XK_KP_Enter:
        return 0x0Du;
    case XK_Escape:
        return 0x1Bu;
    case XK_Tab:
        return 0x09u;
    case XK_BackSpace:
        return 0x08u;
    case XK_Delete:
        return 0x2Eu;
    case XK_Insert:
        return 0x2Du;
    case XK_Left:
        return 0x25u;
    case XK_Up:
        return 0x26u;
    case XK_Right:
        return 0x27u;
    case XK_Down:
        return 0x28u;
    case XK_Prior:
        return 0x21u;
    case XK_Next:
        return 0x22u;
    case XK_Home:
        return 0x24u;
    case XK_End:
        return 0x23u;
    case XK_Shift_L:
        return 0xA0u;
    case XK_Shift_R:
        return 0xA1u;
    case XK_Control_L:
        return 0xA2u;
    case XK_Control_R:
        return 0xA3u;
    case XK_Alt_L:
        return 0xA4u;
    case XK_Alt_R:
        return 0xA5u;
    case XK_Super_L:
        return 0x5Bu;
    case XK_Super_R:
        return 0x5Cu;
    default:
        return 0u;
    }
}

namespace {

enum class X11Mapped { Unknown, Yes, No };

// FocusIn/FocusOut do not carry map state. A FocusOut only backgrounds when the
// window is actually unmapped (minimize / withdraw); alt-tab leaves it mapped.
X11Mapped x11WindowMapped(::Window xid) {
    if (g_display == nullptr || xid == 0) {
        return X11Mapped::Unknown;
    }
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(g_display, xid, &attributes) == 0) {
        return X11Mapped::Unknown;
    }
    return attributes.map_state == IsUnmapped ? X11Mapped::No : X11Mapped::Yes;
}

WindowIc* icFor(::Window xid) {
    for (WindowIc& w : g_ics) {
        if (w.xid == xid && w.ic != nullptr) {
            return &w;
        }
    }
    return nullptr;
}

// Committed UTF-8 for a KeyPress: Xutf8LookupString through the window's input context, else XLookupString
// (Latin-1 keysyms). Control characters are dropped (they stay key events). Returns true when text was produced.
bool lookupText(XKeyEvent& key, char* out, usize capacity) {
    out[0] = '\0';
    if (WindowIc* w = icFor(key.window)) {
        KeySym sym = 0;
        Status status = 0;
        const int n = Xutf8LookupString(w->ic, &key, out, static_cast<int>(capacity - 1u), &sym, &status);
        if ((status == XLookupChars || status == XLookupBoth) && n > 0) {
            out[n] = '\0';
            const unsigned char c = static_cast<unsigned char>(out[0]);
            if (n == 1 && (c < 0x20u || c == 0x7Fu)) {
                out[0] = '\0';
                return false;
            }
            return true;
        }
        if (status != XLookupKeySym && status != XLookupBoth && status != XBufferOverflow) {
            return false; // XLookupNone: the IM consumed it
        }
        out[0] = '\0';
        if (status == XBufferOverflow) {
            return false;
        }
    }
    char latin1[8] = {};
    KeySym typed = 0;
    const int n = XLookupString(&key, latin1, sizeof(latin1), &typed, nullptr);
    const unsigned char c = n == 1 ? static_cast<unsigned char>(latin1[0]) : 0u;
    if (c < 0x20u || c == 0x7Fu) {
        return false;
    }
    if (c < 0x80u) {
        out[0] = static_cast<char>(c);
        out[1] = '\0';
    } else {
        out[0] = static_cast<char>(0xC0u | (c >> 6));
        out[1] = static_cast<char>(0x80u | (c & 0x3Fu));
        out[2] = '\0';
    }
    return true;
}

bool translate(const XEvent& xe, Event& out) {
    out = Event{};
    out.window = fromXid(xe.xany.window);

    switch (xe.type) {
    case ClientMessage:
        if (static_cast<Atom>(xe.xclient.message_type) == g_wmProtocols &&
            static_cast<Atom>(xe.xclient.data.l[0]) == g_wmDeleteWindow) {
            out.kind = EventKind::Close;
            return true;
        }
        return false;
    case ConfigureNotify:
        out.window = fromXid(xe.xconfigure.window);
        if (xe.xconfigure.width <= 0 || xe.xconfigure.height <= 0) {
            return false;
        }
        out.kind = EventKind::Resize;
        out.width = static_cast<u32>(xe.xconfigure.width);
        out.height = static_cast<u32>(xe.xconfigure.height);
        return true;
    case FocusIn:
    case FocusOut:
        if (xe.xfocus.detail == NotifyPointer) {
            return false;
        }
        out.kind = xe.type == FocusIn ? EventKind::FocusGained : EventKind::FocusLost;
        if (xe.type == FocusIn && x11WindowMapped(xe.xfocus.window) == X11Mapped::Yes) {
            notifyAppVisibility(AppVisibility::Foreground);
        } else if (xe.type == FocusOut && x11WindowMapped(xe.xfocus.window) == X11Mapped::No) {
            notifyAppVisibility(AppVisibility::Background);
        }
        return true;
    case KeyPress:
    case KeyRelease: {
        XKeyEvent keyEvent = xe.xkey;
        const KeySym keysym = XLookupKeysym(&keyEvent, 0);
        out.keyCode = keysymToVirtualKey(static_cast<unsigned long>(keysym));
        if (out.keyCode == 0u) {
            // XIM commits arrive as KeyPress with keycode 0 (or keys FUSE has no VK for): text only.
            if (xe.type == KeyPress && lookupText(keyEvent, out.text, sizeof(out.text))) {
                out.kind = EventKind::Text;
                return true;
            }
            return false;
        }
        out.kind = xe.type == KeyPress ? EventKind::KeyDown : EventKind::KeyUp;
        if (xe.type == KeyPress) {
            lookupText(keyEvent, out.text, sizeof(out.text));
        }
        return true;
    }
    case MotionNotify:
        out.kind = EventKind::MouseMove;
        out.x = xe.xmotion.x;
        out.y = xe.xmotion.y;
        return true;
    case ButtonPress:
    case ButtonRelease: {
        const bool down = xe.type == ButtonPress;
        out.x = xe.xbutton.x;
        out.y = xe.xbutton.y;
        switch (xe.xbutton.button) {
        case Button1:
            out.button = 1u;
            break;
        case Button2:
            out.button = 3u;
            break;
        case Button3:
            out.button = 2u;
            break;
        case Button4:
        case Button5:
        case 6:
        case 7:
            if (!down) {
                return false;
            }
            out.kind = EventKind::MouseWheel;
            switch (xe.xbutton.button) {
            case Button4: out.wheelY = 120; break;
            case Button5: out.wheelY = -120; break;
            case 6: out.wheelX = -120; break;
            default: out.wheelX = 120; break;
            }
            return true;
        case 8:
            out.button = 4u;
            break;
        case 9:
            out.button = 5u;
            break;
        default:
            return false;
        }
        out.kind = down ? EventKind::MouseButtonDown : EventKind::MouseButtonUp;
        return true;
    }
    case MapNotify:
        notifyAppVisibility(AppVisibility::Foreground);
        return false;
    case UnmapNotify:
        notifyAppVisibility(AppVisibility::Background);
        return false;
    default:
        return false;
    }
}

} // namespace

// --- XInput2 raw motion ---------------------------------------------------------------------
//
// XI_RawMotion is selected on the root window for XIAllMasterDevices while some FUSE window
// holds input capture. `raw_values` are the source device's valuators *before* the server's
// pointer acceleration (XChangePointerControl / "Device Accel *" properties) and before the
// device's Coordinate Transformation Matrix, so they are the physical device counts. Axes 0/1
// are X/Y. Relative-mode axes are used as-is; absolute-mode sources (tablets, touchscreens)
// are converted to deltas by differencing successive samples. Sub-unit fractions (high-res
// mice) are carried in a remainder so the emitted integer deltas sum to the device total.

namespace {

#if defined(FUSE_PLATFORM_X11_XI2)

int g_xiOpcode = -1;
bool g_xiProbed = false;
bool g_xiAvailable = false;
bool g_rawSelected = false;
double g_rawRemainderX = 0.0;
double g_rawRemainderY = 0.0;

struct RawSourceInfo {
    int sourceId = -1;
    bool absoluteX = false;
    bool absoluteY = false;
    bool haveLast = false;
    double lastX = 0.0;
    double lastY = 0.0;
};

constexpr int kMaxRawSources = 16;
RawSourceInfo g_rawSources[kMaxRawSources];
int g_rawSourceCount = 0;
int g_rawSourceNext = 0;

bool probeXi2() {
    if (g_xiProbed) {
        return g_xiAvailable;
    }
    g_xiProbed = true;
    const char* disable = std::getenv("FUSE_X11_NO_XI2");
    if (disable != nullptr && disable[0] != '\0' && disable[0] != '0') {
        return false;
    }
    int event = 0;
    int error = 0;
    if (XQueryExtension(g_display, "XInputExtension", &g_xiOpcode, &event, &error) == False) {
        return false;
    }
    int major = 2;
    int minor = 2;
    if (XIQueryVersion(g_display, &major, &minor) != Success) {
        // Another component may have announced a lower client version; 2.0 is enough.
        major = 2;
        minor = 0;
        if (XIQueryVersion(g_display, &major, &minor) != Success) {
            return false;
        }
    }
    g_xiAvailable = major >= 2;
    return g_xiAvailable;
}

void selectRawMasks(bool enabled) {
    unsigned char rawMask[XIMaskLen(XI_LASTEVENT)] = {};
    unsigned char hierarchyMask[XIMaskLen(XI_LASTEVENT)] = {};
    if (enabled) {
        XISetMask(rawMask, XI_RawMotion);
        XISetMask(hierarchyMask, XI_HierarchyChanged);
        XISetMask(hierarchyMask, XI_DeviceChanged);
    }
    XIEventMask masks[2];
    masks[0].deviceid = XIAllMasterDevices;
    masks[0].mask_len = sizeof(rawMask);
    masks[0].mask = rawMask;
    masks[1].deviceid = XIAllDevices;
    masks[1].mask_len = sizeof(hierarchyMask);
    masks[1].mask = hierarchyMask;
    XISelectEvents(g_display, DefaultRootWindow(g_display), masks, 2);
    XFlush(g_display);
}

void resetRawSources() {
    g_rawSourceCount = 0;
    g_rawSourceNext = 0;
}

RawSourceInfo* rawSource(int sourceId) {
    for (int i = 0; i < g_rawSourceCount; ++i) {
        if (g_rawSources[i].sourceId == sourceId) {
            return &g_rawSources[i];
        }
    }

    RawSourceInfo info{};
    info.sourceId = sourceId;
    int count = 0;
    XIDeviceInfo* devices = XIQueryDevice(g_display, sourceId, &count);
    if (devices != nullptr) {
        for (int c = 0; c < devices[0].num_classes; ++c) {
            const XIAnyClassInfo* any = devices[0].classes[c];
            if (any == nullptr || any->type != XIValuatorClass) {
                continue;
            }
            const auto* valuator = reinterpret_cast<const XIValuatorClassInfo*>(any);
            if (valuator->number == 0) {
                info.absoluteX = valuator->mode == XIModeAbsolute;
            } else if (valuator->number == 1) {
                info.absoluteY = valuator->mode == XIModeAbsolute;
            }
        }
        XIFreeDeviceInfo(devices);
    }

    int slot = g_rawSourceCount;
    if (g_rawSourceCount < kMaxRawSources) {
        ++g_rawSourceCount;
    } else {
        slot = g_rawSourceNext;
        g_rawSourceNext = (g_rawSourceNext + 1) % kMaxRawSources;
    }
    g_rawSources[slot] = info;
    return &g_rawSources[slot];
}

// Returns true (and fills `out`) when the cookie is an XI_RawMotion with a non-zero delta.
bool translateXi2(XGenericEventCookie& cookie, Event& out) {
    if (cookie.extension != g_xiOpcode) {
        return false;
    }
    if (cookie.evtype == XI_HierarchyChanged || cookie.evtype == XI_DeviceChanged) {
        resetRawSources();
        return false;
    }
    if (cookie.evtype != XI_RawMotion || !g_rawSelected) {
        return false;
    }
    if (XGetEventData(g_display, &cookie) == False) {
        return false;
    }

    const auto* raw = static_cast<const XIRawEvent*>(cookie.data);
    bool haveX = false;
    bool haveY = false;
    double rawX = 0.0;
    double rawY = 0.0;
    // raw_values are packed in ascending valuator order, so X/Y (0/1) come first when set.
    const int bitCount = raw->valuators.mask_len * 8;
    int index = 0;
    for (int bit = 0; bit < 2 && bit < bitCount; ++bit) {
        if (!XIMaskIsSet(raw->valuators.mask, bit)) {
            continue;
        }
        if (bit == 0) {
            rawX = raw->raw_values[index];
            haveX = true;
        } else {
            rawY = raw->raw_values[index];
            haveY = true;
        }
        ++index;
    }
    const int sourceId = raw->sourceid;
    XFreeEventData(g_display, &cookie);

    if (!haveX && !haveY) {
        return false;
    }

    RawSourceInfo* source = rawSource(sourceId);
    double dx = rawX;
    double dy = rawY;
    if (source->absoluteX || source->absoluteY) {
        const double absX = haveX ? rawX : source->lastX;
        const double absY = haveY ? rawY : source->lastY;
        if (source->absoluteX) {
            dx = source->haveLast && haveX ? absX - source->lastX : 0.0;
        }
        if (source->absoluteY) {
            dy = source->haveLast && haveY ? absY - source->lastY : 0.0;
        }
        source->lastX = absX;
        source->lastY = absY;
        source->haveLast = true;
    }
    if (!haveX) {
        dx = 0.0;
    }
    if (!haveY) {
        dy = 0.0;
    }

    g_rawRemainderX += dx;
    g_rawRemainderY += dy;
    const double wholeX = std::trunc(g_rawRemainderX);
    const double wholeY = std::trunc(g_rawRemainderY);
    g_rawRemainderX -= wholeX;
    g_rawRemainderY -= wholeY;
    if (wholeX == 0.0 && wholeY == 0.0) {
        return false;
    }

    out = Event{};
    out.kind = EventKind::RawMouseDelta;
    out.x = static_cast<i32>(wholeX);
    out.y = static_cast<i32>(wholeY);
    return true;
}

#endif // FUSE_PLATFORM_X11_XI2

} // namespace

bool rawMotionAvailable() {
#if defined(FUSE_PLATFORM_X11_XI2)
    return ensureDisplay() && probeXi2();
#else
    return false;
#endif
}

bool setRawMotionEnabled(bool enabled) {
#if defined(FUSE_PLATFORM_X11_XI2)
    // Never opens the display on its own: raw motion only matters once a native window exists.
    if (g_rawSelected == enabled) {
        return g_rawSelected;
    }
    if (g_display == nullptr || !probeXi2()) {
        return false;
    }
    selectRawMasks(enabled);
    g_rawSelected = enabled;
    g_rawRemainderX = 0.0;
    g_rawRemainderY = 0.0;
    resetRawSources();
    return g_rawSelected;
#else
    (void)enabled;
    return false;
#endif
}

bool pollEvent(Event& out) {
    if (g_display == nullptr) {
        return false;
    }
    for (;;) {
        if (g_pendingCount > 0) {
            out = g_pending[g_pendingHead];
            g_pendingHead = (g_pendingHead + 1) % kPendingEvents;
            --g_pendingCount;
            return true;
        }
        if (XPending(g_display) <= 0) {
            return false;
        }
        XEvent xe;
        XNextEvent(g_display, &xe);
        if (handleSelectionEvent(xe)) {
            continue;
        }
        if (g_im != nullptr && XFilterEvent(&xe, 0) == True) {
            continue; // consumed by the input method (dead keys, compose, pre-edit); callbacks may have queued events
        }
#if defined(FUSE_PLATFORM_X11_XI2)
        if (xe.type == GenericEvent) {
            if (g_xiAvailable && translateXi2(xe.xcookie, out)) {
                return true;
            }
            continue;
        }
#endif
        if (translate(xe, out)) {
            return true;
        }
    }
    return false;
}

// --- UI1 text services: XIM input contexts + pre-edit callbacks, CLIPBOARD selection -------------

namespace {

usize encodeUtf8(u32 cp, char* out) {
    if (cp < 0x80u) { out[0] = static_cast<char>(cp); return 1; }
    if (cp < 0x800u) { out[0] = static_cast<char>(0xC0u | (cp >> 6)); out[1] = static_cast<char>(0x80u | (cp & 0x3Fu)); return 2; }
    if (cp < 0x10000u) {
        out[0] = static_cast<char>(0xE0u | (cp >> 12)); out[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = static_cast<char>(0x80u | (cp & 0x3Fu)); return 3;
    }
    out[0] = static_cast<char>(0xF0u | (cp >> 18)); out[1] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)); out[3] = static_cast<char>(0x80u | (cp & 0x3Fu)); return 4;
}

// Decodes UTF-8 (pre-edit text in a UTF-8 locale) into code points; returns the count written.
i32 decodeUtf8(const char* s, u32* out, i32 capacity) {
    i32 n = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    while (*p != 0u && n < capacity) {
        u32 cp = *p;
        int extra = cp >= 0xF0u ? 3 : cp >= 0xE0u ? 2 : cp >= 0xC0u ? 1 : 0;
        cp &= extra == 3 ? 0x07u : extra == 2 ? 0x0Fu : extra == 1 ? 0x1Fu : 0x7Fu;
        ++p;
        for (; extra > 0 && (*p & 0xC0u) == 0x80u; --extra, ++p) {
            cp = (cp << 6) | (*p & 0x3Fu);
        }
        out[n++] = cp;
    }
    return n;
}

void queueComposition(WindowIc& w) {
    Event e{};
    e.kind = EventKind::Composition;
    e.window = fromXid(w.xid);
    usize bytes = 0;
    for (i32 i = 0; i < w.preeditLength; ++i) {
        char tmp[4];
        const usize n = encodeUtf8(w.preedit[i], tmp);
        if (bytes + n >= sizeof(e.text)) {
            break;
        }
        if (i == w.caret) {
            e.caret = static_cast<i32>(bytes);
        }
        std::memcpy(e.text + bytes, tmp, n);
        bytes += n;
    }
    if (w.caret >= w.preeditLength) {
        e.caret = static_cast<i32>(bytes);
    }
    e.text[bytes] = '\0';
    queueEvent(e);
}

// Replaces `length` chars at `first` with `chars` (count `count`), then sets the caret.
void applyPreedit(WindowIc& w, i32 first, i32 length, const u32* chars, i32 count, i32 caret) {
    first = first < 0 ? 0 : (first > w.preeditLength ? w.preeditLength : first);
    length = length < 0 ? 0 : (first + length > w.preeditLength ? w.preeditLength - first : length);
    i32 tail = w.preeditLength - first - length;
    if (first + count + tail > kMaxPreedit) {
        count = kMaxPreedit - first - tail;
        if (count < 0) { tail += count; count = 0; }
    }
    std::memmove(w.preedit + first + count, w.preedit + first + length, static_cast<usize>(tail) * sizeof(u32));
    if (count > 0) {
        std::memcpy(w.preedit + first, chars, static_cast<usize>(count) * sizeof(u32));
    }
    w.preeditLength = first + count + tail;
    w.caret = caret < 0 ? 0 : (caret > w.preeditLength ? w.preeditLength : caret);
    queueComposition(w);
}

WindowIc* icByClientData(XPointer data) { return reinterpret_cast<WindowIc*>(data); }

int preeditStart(XIC, XPointer data, XPointer) {
    WindowIc* w = icByClientData(data);
    w->preeditLength = w->caret = 0;
    return kMaxPreedit; // max pre-edit length we accept
}

void preeditDone(XIC, XPointer data, XPointer) {
    WindowIc* w = icByClientData(data);
    w->preeditLength = w->caret = 0;
    queueComposition(*w);
}

void preeditDraw(XIC, XPointer data, XPointer callData) {
    WindowIc* w = icByClientData(data);
    const auto* d = reinterpret_cast<XIMPreeditDrawCallbackStruct*>(callData);
    u32 chars[kMaxPreedit];
    i32 count = 0;
    if (d->text != nullptr) {
        if (d->text->encoding_is_wchar) {
            for (; count < kMaxPreedit && count < d->text->length; ++count) {
                chars[count] = static_cast<u32>(d->text->string.wide_char[count]);
            }
        } else if (d->text->string.multi_byte != nullptr) {
            count = decodeUtf8(d->text->string.multi_byte, chars, kMaxPreedit); // UTF-8 locale assumed
        }
    }
    applyPreedit(*w, d->chg_first, d->chg_length, chars, count, d->caret);
}

void preeditCaret(XIC, XPointer data, XPointer callData) {
    WindowIc* w = icByClientData(data);
    auto* d = reinterpret_cast<XIMPreeditCaretCallbackStruct*>(callData);
    switch (d->direction) {
    case XIMForwardChar: ++w->caret; break;
    case XIMBackwardChar: --w->caret; break;
    case XIMLineStart: w->caret = 0; break;
    case XIMLineEnd: w->caret = w->preeditLength; break;
    case XIMAbsolutePosition: w->caret = d->position; break;
    default: break;
    }
    w->caret = w->caret < 0 ? 0 : (w->caret > w->preeditLength ? w->preeditLength : w->caret);
    d->position = w->caret;
    queueComposition(*w);
}

void imDestroyed(XIM, XPointer, XPointer) {
    g_im = nullptr;
    for (WindowIc& w : g_ics) {
        w.ic = nullptr; // the IM owns them; contexts die with it
    }
}

bool openInputMethod() {
    if (g_imTried) {
        return g_im != nullptr;
    }
    g_imTried = true;
    if (g_display == nullptr || XSupportsLocale() == False) {
        return false;
    }
    XSetLocaleModifiers(""); // honours XMODIFIERS (@im=ibus, fcitx, ...)
    g_im = XOpenIM(g_display, nullptr, nullptr, nullptr);
    if (g_im == nullptr) {
        XSetLocaleModifiers("@im=none"); // built-in IM: compose sequences and dead keys
        g_im = XOpenIM(g_display, nullptr, nullptr, nullptr);
    }
    if (g_im == nullptr) {
        return false;
    }
    XIMCallback destroy{nullptr, imDestroyed};
    XSetIMValues(g_im, XNDestroyCallback, &destroy, nullptr);
    XIMStyles* styles = nullptr;
    XGetIMValues(g_im, XNQueryInputStyle, &styles, nullptr);
    const XIMStyle preferred[] = {XIMPreeditCallbacks | XIMStatusNothing, XIMPreeditPosition | XIMStatusNothing,
                                  XIMPreeditNothing | XIMStatusNothing, XIMPreeditNone | XIMStatusNone};
    if (styles != nullptr) {
        for (XIMStyle want : preferred) {
            for (unsigned short i = 0; i < styles->count_styles && g_imStyle == 0; ++i) {
                if (styles->supported_styles[i] == want) {
                    g_imStyle = want;
                }
            }
        }
        XFree(styles);
    }
    if (g_imStyle == 0) {
        XCloseIM(g_im);
        g_im = nullptr;
        return false;
    }
    return true;
}

void createInputContext(::Window xid) {
    if (!openInputMethod() || icFor(xid) != nullptr) {
        return;
    }
    WindowIc* slot = nullptr;
    for (WindowIc& w : g_ics) {
        if (w.xid == 0 || w.ic == nullptr) {
            slot = &w;
            break;
        }
    }
    if (slot == nullptr) {
        return; // more windows than slots: plain XLookupString text for the extra ones
    }
    *slot = WindowIc{};
    slot->xid = xid;
    XIMCallback start{reinterpret_cast<XPointer>(slot), reinterpret_cast<XIMProc>(reinterpret_cast<void (*)()>(preeditStart))};
    XIMCallback done{reinterpret_cast<XPointer>(slot), reinterpret_cast<XIMProc>(reinterpret_cast<void (*)()>(preeditDone))};
    XIMCallback draw{reinterpret_cast<XPointer>(slot), reinterpret_cast<XIMProc>(reinterpret_cast<void (*)()>(preeditDraw))};
    XIMCallback caret{reinterpret_cast<XPointer>(slot), reinterpret_cast<XIMProc>(reinterpret_cast<void (*)()>(preeditCaret))};
    XPoint spot{0, 0};
    XVaNestedList preedit = nullptr;
    if ((g_imStyle & XIMPreeditCallbacks) != 0) {
        preedit = XVaCreateNestedList(0, XNPreeditStartCallback, &start, XNPreeditDoneCallback, &done,
                                      XNPreeditDrawCallback, &draw, XNPreeditCaretCallback, &caret, nullptr);
    } else if ((g_imStyle & XIMPreeditPosition) != 0) {
        preedit = XVaCreateNestedList(0, XNSpotLocation, &spot, nullptr);
    }
    slot->ic = preedit != nullptr
                   ? XCreateIC(g_im, XNInputStyle, g_imStyle, XNClientWindow, xid, XNFocusWindow, xid,
                               XNPreeditAttributes, preedit, nullptr)
                   : XCreateIC(g_im, XNInputStyle, g_imStyle, XNClientWindow, xid, XNFocusWindow, xid, nullptr);
    if (preedit != nullptr) {
        XFree(preedit);
    }
    if (slot->ic == nullptr) {
        slot->xid = 0;
        return;
    }
    long filter = 0;
    XGetICValues(slot->ic, XNFilterEvents, &filter, nullptr);
    XWindowAttributes attributes{};
    XGetWindowAttributes(g_display, xid, &attributes);
    XSelectInput(g_display, xid, attributes.your_event_mask | filter);
    XSetICFocus(slot->ic);
}

void destroyInputContext(::Window xid) {
    for (WindowIc& w : g_ics) {
        if (w.xid == xid) {
            if (w.ic != nullptr && g_im != nullptr) {
                XDestroyIC(w.ic);
            }
            w = WindowIc{};
        }
    }
}

bool ensureClipWindow() {
    if (!ensureDisplay()) {
        return false;
    }
    if (g_clipWindow == 0) {
        XSetWindowAttributes attributes{};
        attributes.event_mask = PropertyChangeMask;
        g_clipWindow = XCreateWindow(g_display, DefaultRootWindow(g_display), -10, -10, 1, 1, 0, 0, InputOnly,
                                     CopyFromParent, CWEventMask, &attributes);
    }
    return g_clipWindow != 0;
}

void answerSelectionRequest(const XSelectionRequestEvent& req) {
    XSelectionEvent reply{};
    reply.type = SelectionNotify;
    reply.display = req.display;
    reply.requestor = req.requestor;
    reply.selection = req.selection;
    reply.target = req.target;
    reply.time = req.time;
    reply.property = 0; // refused unless filled below
    const Atom property = req.property != 0 ? req.property : req.target; // obsolete clients pass None
    const auto* bytes = reinterpret_cast<const unsigned char*>(g_clipText.data());
    const int length = static_cast<int>(g_clipText.size());
    if (g_clipOwned && req.selection == g_atomClipboard) {
        if (req.target == g_atomTargets) {
            const Atom targets[] = {g_atomTargets, g_utf8String, XA_STRING, g_atomText};
            XChangeProperty(g_display, req.requestor, property, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(targets), 4);
            reply.property = property;
        } else if (req.target == g_utf8String || req.target == g_atomText) {
            XChangeProperty(g_display, req.requestor, property, g_utf8String, 8, PropModeReplace, bytes, length);
            reply.property = property;
        } else if (req.target == XA_STRING) {
            // ICCCM STRING is Latin-1: code points above U+00FF become '?'.
            std::vector<unsigned char> latin1;
            latin1.reserve(g_clipText.size());
            u32 cps[1] = {0};
            for (usize i = 0; i < g_clipText.size();) {
                const unsigned char c = bytes[i];
                const usize n = c >= 0xF0u ? 4u : c >= 0xE0u ? 3u : c >= 0xC0u ? 2u : 1u;
                char tmp[5] = {};
                std::memcpy(tmp, bytes + i, n <= g_clipText.size() - i ? n : g_clipText.size() - i);
                decodeUtf8(tmp, cps, 1);
                latin1.push_back(cps[0] <= 0xFFu ? static_cast<unsigned char>(cps[0]) : '?');
                i += n;
            }
            XChangeProperty(g_display, req.requestor, property, XA_STRING, 8, PropModeReplace, latin1.data(),
                            static_cast<int>(latin1.size()));
            reply.property = property;
        }
    }
    XSendEvent(g_display, req.requestor, False, 0, reinterpret_cast<XEvent*>(&reply));
    XFlush(g_display);
}

bool handleSelectionEvent(XEvent& xe) {
    if (g_clipWindow == 0 || xe.xany.window != g_clipWindow) {
        return false;
    }
    if (xe.type == SelectionRequest) {
        answerSelectionRequest(xe.xselectionrequest);
    } else if (xe.type == SelectionClear && xe.xselectionclear.selection == g_atomClipboard) {
        g_clipOwned = false; // another client took the clipboard
    }
    return true; // everything on the helper window is ours
}

} // namespace

bool inputMethodAvailable() { return ensureDisplay() && openInputMethod(); }

void setTextInputFocus(void* window, bool focused) {
    if (WindowIc* w = g_display != nullptr ? icFor(toXid(window)) : nullptr) {
        if (focused) {
            XSetICFocus(w->ic);
        } else {
            XUnsetICFocus(w->ic);
            if (w->preeditLength > 0) {
                char* committed = Xutf8ResetIC(w->ic); // drop the pending pre-edit
                if (committed != nullptr) {
                    XFree(committed);
                }
                w->preeditLength = w->caret = 0;
                queueComposition(*w);
            }
        }
        XFlush(g_display);
    }
}

void setTextInputSpot(void* window, i32 x, i32 y) {
    WindowIc* w = g_display != nullptr ? icFor(toXid(window)) : nullptr;
    if (w == nullptr) {
        return;
    }
    XPoint spot{static_cast<short>(x), static_cast<short>(y)};
    XVaNestedList attributes = XVaCreateNestedList(0, XNSpotLocation, &spot, nullptr);
    XSetICValues(w->ic, XNPreeditAttributes, attributes, nullptr);
    XFree(attributes);
}

void debugPreeditDraw(void* window, i32 first, i32 length, const char* utf8, i32 caret) {
    for (WindowIc& w : g_ics) {
        if (w.xid == toXid(window)) {
            u32 chars[kMaxPreedit];
            const i32 count = utf8 != nullptr ? decodeUtf8(utf8, chars, kMaxPreedit) : 0;
            applyPreedit(w, first, length, chars, count, caret);
            return;
        }
    }
    // No IM: a scratch context still exercises the logic (tests under a bare Xvfb).
    for (WindowIc& w : g_ics) {
        if (w.xid == 0) {
            w = WindowIc{};
            w.xid = toXid(window);
            u32 chars[kMaxPreedit];
            const i32 count = utf8 != nullptr ? decodeUtf8(utf8, chars, kMaxPreedit) : 0;
            applyPreedit(w, first, length, chars, count, caret);
            return;
        }
    }
}

bool setClipboard(const char* utf8, usize length) {
    if (!ensureClipWindow()) {
        return false;
    }
    g_clipText.assign(utf8, utf8 + length);
    XSetSelectionOwner(g_display, g_atomClipboard, g_clipWindow, CurrentTime);
    g_clipOwned = XGetSelectionOwner(g_display, g_atomClipboard) == g_clipWindow;
    XFlush(g_display);
    return g_clipOwned;
}

void serviceSelections() {
    if (g_display == nullptr || g_clipWindow == 0) {
        return;
    }
    XEvent xe;
    while (XCheckWindowEvent(g_display, g_clipWindow, ~0L, &xe) == True ||
           XCheckTypedWindowEvent(g_display, g_clipWindow, SelectionRequest, &xe) == True ||
           XCheckTypedWindowEvent(g_display, g_clipWindow, SelectionClear, &xe) == True) {
        handleSelectionEvent(xe);
    }
}

bool clipboard(std::vector<char>& out, u32 timeoutMs) {
    out.clear();
    if (!ensureClipWindow()) {
        return false;
    }
    if (g_clipOwned && XGetSelectionOwner(g_display, g_atomClipboard) == g_clipWindow) {
        out = g_clipText;
        return !out.empty();
    }
    g_clipOwned = false;
    if (XGetSelectionOwner(g_display, g_atomClipboard) == 0) {
        return false;
    }
    for (const Atom target : {g_utf8String, static_cast<Atom>(XA_STRING)}) {
        XDeleteProperty(g_display, g_clipWindow, g_atomClipProp);
        XConvertSelection(g_display, g_atomClipboard, target, g_atomClipProp, g_clipWindow, CurrentTime);
        XFlush(g_display);
        XEvent xe{};
        bool got = false;
        for (u32 waited = 0; waited <= timeoutMs; ++waited) {
            if (XCheckTypedWindowEvent(g_display, g_clipWindow, SelectionNotify, &xe) == True) {
                got = true;
                break;
            }
            timespec ts{0, 1000000};
            nanosleep(&ts, nullptr);
        }
        if (!got) {
            return false; // owner did not answer in time
        }
        if (xe.xselection.property == 0) {
            continue; // target refused: try the next one
        }
        Atom type = 0;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(g_display, g_clipWindow, g_atomClipProp, 0, 0x7FFFFFFF, True, AnyPropertyType, &type,
                               &format, &count, &after, &data) != Success) {
            return false;
        }
        bool ok = false;
        if (type == g_atomIncr) {
            ok = false; // INCR transfers (very large selections) are not supported
        } else if (format == 8 && data != nullptr) {
            if (type == XA_STRING) {
                for (unsigned long i = 0; i < count; ++i) {
                    char tmp[4];
                    const usize n = encodeUtf8(data[i], tmp);
                    out.insert(out.end(), tmp, tmp + n);
                }
            } else {
                out.assign(reinterpret_cast<char*>(data), reinterpret_cast<char*>(data) + count);
            }
            ok = !out.empty();
        }
        if (data != nullptr) {
            XFree(data);
        }
        return ok;
    }
    return false;
}

void vulkanInstanceExtensions(std::vector<const char*>& out) {
#if defined(FUSE_PLATFORM_X11_VULKAN)
    out.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    out.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#else
    (void)out;
#endif
}

bool createVulkanSurface(void* vkInstance, void* window, void** outSurface) {
#if defined(FUSE_PLATFORM_X11_VULKAN)
    if (vkInstance == nullptr || window == nullptr || !ensureDisplay()) {
        return false;
    }

    VkXlibSurfaceCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    createInfo.dpy = g_display;
    createInfo.window = toXid(window);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult result =
        vkCreateXlibSurfaceKHR(static_cast<VkInstance>(vkInstance), &createInfo, nullptr, &surface);
    if (result != VK_SUCCESS || surface == VK_NULL_HANDLE) {
        return false;
    }
    if (outSurface != nullptr) {
        *outSurface = (void*)surface;
    }
    return true;
#else
    (void)vkInstance;
    (void)window;
    (void)outSurface;
    return false;
#endif
}

} // namespace fuse::platform::x11

#endif // FUSE_PLATFORM_WINDOW_X11
