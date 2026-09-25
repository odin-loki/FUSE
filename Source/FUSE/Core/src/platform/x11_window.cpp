// Native X11 (Xlib) window backend — selected when GLFW is unavailable on Linux/BSD.
// See x11_window.hpp for why Xlib stays confined to this translation unit.

#include "x11_window.hpp"

#if defined(FUSE_PLATFORM_WINDOW_X11)

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
    XMapWindow(g_display, xid);
    XSync(g_display, False);
    return fromXid(xid);
}

void destroyWindow(void* window) {
    if (g_display == nullptr || window == nullptr) {
        return;
    }
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
        return true;
    case KeyPress:
    case KeyRelease: {
        XKeyEvent keyEvent = xe.xkey;
        const KeySym keysym = XLookupKeysym(&keyEvent, 0);
        out.keyCode = keysymToVirtualKey(static_cast<unsigned long>(keysym));
        if (out.keyCode == 0u) {
            return false;
        }
        out.kind = xe.type == KeyPress ? EventKind::KeyDown : EventKind::KeyUp;
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
            if (!down) {
                return false;
            }
            out.kind = EventKind::MouseWheel;
            out.y = xe.xbutton.button == Button4 ? 120 : -120;
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
    while (XPending(g_display) > 0) {
        XEvent xe;
        XNextEvent(g_display, &xe);
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
