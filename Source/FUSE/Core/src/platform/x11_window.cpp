// Native X11 (Xlib) window backend — selected when GLFW is unavailable on Linux/BSD.
// See x11_window.hpp for why Xlib stays confined to this translation unit.

#include "x11_window.hpp"

#if defined(FUSE_PLATFORM_WINDOW_X11)

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

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

bool pollEvent(Event& out) {
    if (g_display == nullptr) {
        return false;
    }
    while (XPending(g_display) > 0) {
        XEvent xe;
        XNextEvent(g_display, &xe);
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
