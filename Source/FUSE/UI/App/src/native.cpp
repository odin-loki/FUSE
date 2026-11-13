// fuse_app native windows: X11 (libX11 loaded at runtime, so neither X11 headers nor libX11 are needed to build) and
// Win32, plus the presenter selection (Vulkan swapchain -> GDI on Win32 / XPutImage on X11 -> headless). See native.hpp.
// The platform services (cursors, clipboard, window management, theme, displays, dialogs) are in native_services.inl.
#include "native_internal.hpp"
#include "fuse/app/chrome.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <atomic>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#include "native_wayland_platform.hpp" // the Wayland backend (chosen at run time, see NativeOptions::backend)
#include "wayland_internal.hpp"
#endif

namespace fuse::app {
using namespace fuse::ui;
using detail::SurfaceSource;

using detail::appendUtf8;
using detail::asciiKey;

#if !defined(_WIN32)
// ---- X11 (runtime-loaded) ----------------------------------------------------------------------------------------
namespace x11 {
using Display = struct _XDisplay;
using XID = unsigned long;
using Window = XID;
using Atom = unsigned long;
using KeySym = XID;
// Event layouts (LP64 Xlib ABI); XEvent is a union padded to 24 longs.
struct AnyEv { int type; unsigned long serial; int send_event; Display* display; Window window; };
struct InputEv {
    int type; unsigned long serial; int send_event; Display* display; Window window, root, subwindow;
    unsigned long time; int x, y, x_root, y_root; unsigned state; unsigned detail; // keycode / button
    int same_screen;
};
struct ConfigureEv { int type; unsigned long serial; int send_event; Display* display; Window event, window; int x, y, width, height; };
struct ClientEv { int type; unsigned long serial; int send_event; Display* display; Window window; Atom message_type; int format; long l[5]; };
struct SelectionRequestEv {
    int type; unsigned long serial; int send_event; Display* display; Window owner, requestor; Atom selection, target, property;
    unsigned long time;
};
struct SelectionEv {
    int type; unsigned long serial; int send_event; Display* display; Window requestor; Atom selection, target, property;
    unsigned long time;
};
struct SelectionClearEv { int type; unsigned long serial; int send_event; Display* display; Window window; Atom selection; unsigned long time; };
struct PropertyEv { int type; unsigned long serial; int send_event; Display* display; Window window; Atom atom; unsigned long time; int state; };
struct FocusEv { int type; unsigned long serial; int send_event; Display* display; Window window; int mode, detail; };
union Event {
    int type; AnyEv any; InputEv input; ConfigureEv configure; ClientEv client; SelectionRequestEv selectionRequest;
    SelectionEv selection; SelectionClearEv selectionClear; PropertyEv property; FocusEv focus; long pad[24];
};
struct Color { unsigned long pixel; unsigned short red, green, blue; char flags, pad; }; // XColor
struct RRMonitor { Atom name; int primary, automatic, noutput, x, y, width, height, mwidth, mheight; XID* outputs; }; // XRRMonitorInfo
struct Image;
struct ImageFuncs { Image* (*create)(); int (*destroy)(Image*); };
struct Image {
    int width, height, xoffset, format; char* data; int byte_order, bitmap_unit, bitmap_bit_order, bitmap_pad, depth,
        bytes_per_line, bits_per_pixel; unsigned long red_mask, green_mask, blue_mask; char* obdata; ImageFuncs f;
};
enum { KeyPress = 2, KeyRelease = 3, ButtonPress = 4, ButtonRelease = 5, MotionNotify = 6, FocusIn = 9, FocusOut = 10, Expose = 12,
       UnmapNotify = 18, MapNotify = 19, ConfigureNotify = 22, PropertyNotify = 28, SelectionClear = 29, SelectionRequest = 30,
       SelectionNotify = 31, ClientMessage = 33 };
// keys, buttons, motion, exposure, structure, focus change, property change (_NET_WM_STATE)
constexpr long kMask = 1 | 2 | 4 | 8 | (1 << 6) | (1 << 15) | (1 << 17) | (1 << 21) | (1 << 22);

struct Lib {
    void* so = nullptr;
    Display* (*OpenDisplay)(const char*) = nullptr;
    int (*CloseDisplay)(Display*) = nullptr;
    int (*DefaultScreen)(Display*) = nullptr;
    Window (*RootWindow)(Display*, int) = nullptr;
    Window (*CreateSimpleWindow)(Display*, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long) = nullptr;
    int (*DestroyWindow)(Display*, Window) = nullptr;
    int (*StoreName)(Display*, Window, const char*) = nullptr;
    int (*SelectInput)(Display*, Window, long) = nullptr;
    int (*MapWindow)(Display*, Window) = nullptr;
    Atom (*InternAtom)(Display*, const char*, int) = nullptr;
    int (*SetWMProtocols)(Display*, Window, Atom*, int) = nullptr;
    int (*Pending)(Display*) = nullptr;
    int (*NextEvent)(Display*, Event*) = nullptr;
    int (*ConnectionNumber)(Display*) = nullptr;
    int (*Flush)(Display*) = nullptr;
    int (*Sync)(Display*, int) = nullptr;
    int (*LookupString)(InputEv*, char*, int, KeySym*, void*) = nullptr;
    Image* (*GetImage)(Display*, XID, int, int, unsigned, unsigned, unsigned long, int) = nullptr;
    void* (*SetErrorHandler)(int (*)(Display*, void*)) = nullptr;
    int (*ChangeProperty)(Display*, Window, Atom, Atom, int, int, const unsigned char*, int) = nullptr;
    int (*GetWindowProperty)(Display*, Window, Atom, long, long, int, Atom, Atom*, int*, unsigned long*, unsigned long*,
                             unsigned char**) = nullptr;
    int (*Free)(void*) = nullptr;
    int (*ResizeWindow)(Display*, Window, unsigned, unsigned) = nullptr;
    Image* (*CreateImage)(Display*, void*, unsigned, int, int, char*, unsigned, unsigned, int, int) = nullptr;
    int (*PutImage)(Display*, XID, void*, Image*, int, int, int, int, unsigned, unsigned) = nullptr;
    void* (*DefaultGC)(Display*, int) = nullptr;
    void* (*DefaultVisual)(Display*, int) = nullptr;
    int (*DefaultDepth)(Display*, int) = nullptr;
    int (*MoveResizeWindow)(Display*, Window, int, int, unsigned, unsigned) = nullptr;
    int (*TranslateCoordinates)(Display*, Window, Window, int, int, int*, int*, Window*) = nullptr;
    int (*ChangeWindowAttributes)(Display*, Window, unsigned long, void*) = nullptr;
    int (*DisplayWidth)(Display*, int) = nullptr;
    int (*DisplayHeight)(Display*, int) = nullptr;
    int (*SendEvent)(Display*, Window, int, long, Event*) = nullptr;
    int (*GrabPointer)(Display*, Window, int, unsigned, int, int, Window, XID, unsigned long) = nullptr;
    int (*UngrabPointer)(Display*, unsigned long) = nullptr;
    // Optional (platform services): a missing symbol or library turns the feature off, never the backend.
    XID (*CreateFontCursor)(Display*, unsigned) = nullptr;
    int (*DefineCursor)(Display*, Window, XID) = nullptr;
    XID (*CreateBitmapFromData)(Display*, XID, const char*, unsigned, unsigned) = nullptr;
    XID (*CreatePixmapCursor)(Display*, XID, XID, Color*, Color*, unsigned, unsigned) = nullptr;
    int (*FreePixmap)(Display*, XID) = nullptr;
    int (*SetSelectionOwner)(Display*, Atom, Window, unsigned long) = nullptr;
    Window (*GetSelectionOwner)(Display*, Atom) = nullptr;
    int (*ConvertSelection)(Display*, Atom, Atom, Atom, Window, unsigned long) = nullptr;
    int (*CheckTypedWindowEvent)(Display*, Window, int, Event*) = nullptr;
    int (*DeleteProperty)(Display*, Window, Atom) = nullptr;
    char* (*ResourceManagerString)(Display*) = nullptr;
    int (*UnmapWindow)(Display*, Window) = nullptr;
    int (*MoveWindow)(Display*, Window, int, int) = nullptr;
    int (*RaiseWindow)(Display*, Window) = nullptr;
    int (*SetInputFocus)(Display*, Window, int, unsigned long) = nullptr;
    char* (*GetAtomName)(Display*, Atom) = nullptr;
    void* xcursor = nullptr; ///< libXcursor: themed cursors by CSS name
    XID (*CursorLoad)(Display*, const char*) = nullptr;
    int (*CursorSetDefaultSize)(Display*, int) = nullptr;
    int (*CursorGetDefaultSize)(Display*) = nullptr;
    int (*CursorSetTheme)(Display*, const char*) = nullptr;
    void* xrandr = nullptr;  ///< libXrandr: monitors and the refresh rate
    RRMonitor* (*RRGetMonitors)(Display*, Window, int, int*) = nullptr;
    void (*RRFreeMonitors)(RRMonitor*) = nullptr;
    void* (*RRGetScreenInfo)(Display*, Window) = nullptr;
    short (*RRConfigCurrentRate)(void*) = nullptr;
    void (*RRFreeScreenConfigInfo)(void*) = nullptr;

    bool load() {
        so = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!so) so = dlopen("libX11.so", RTLD_NOW | RTLD_LOCAL);
        if (!so) return false;
        bool all = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(so, name));
            all = all && fn;
        };
        get(OpenDisplay, "XOpenDisplay"); get(CloseDisplay, "XCloseDisplay"); get(DefaultScreen, "XDefaultScreen");
        get(RootWindow, "XRootWindow"); get(CreateSimpleWindow, "XCreateSimpleWindow"); get(DestroyWindow, "XDestroyWindow");
        get(StoreName, "XStoreName"); get(SelectInput, "XSelectInput"); get(MapWindow, "XMapWindow");
        get(InternAtom, "XInternAtom"); get(SetWMProtocols, "XSetWMProtocols"); get(Pending, "XPending");
        get(NextEvent, "XNextEvent"); get(ConnectionNumber, "XConnectionNumber"); get(Flush, "XFlush"); get(Sync, "XSync");
        get(LookupString, "XLookupString"); get(GetImage, "XGetImage"); get(SetErrorHandler, "XSetErrorHandler");
        get(ChangeProperty, "XChangeProperty"); get(GetWindowProperty, "XGetWindowProperty"); get(Free, "XFree"); get(ResizeWindow, "XResizeWindow");
        get(CreateImage, "XCreateImage"); get(PutImage, "XPutImage"); get(DefaultGC, "XDefaultGC");
        get(DefaultVisual, "XDefaultVisual"); get(DefaultDepth, "XDefaultDepth"); get(MoveResizeWindow, "XMoveResizeWindow");
        get(TranslateCoordinates, "XTranslateCoordinates"); get(ChangeWindowAttributes, "XChangeWindowAttributes");
        get(DisplayWidth, "XDisplayWidth"); get(DisplayHeight, "XDisplayHeight"); get(SendEvent, "XSendEvent");
        get(GrabPointer, "XGrabPointer"); get(UngrabPointer, "XUngrabPointer");
        const bool required = all;
        get(CreateFontCursor, "XCreateFontCursor"); get(DefineCursor, "XDefineCursor");
        get(CreateBitmapFromData, "XCreateBitmapFromData"); get(CreatePixmapCursor, "XCreatePixmapCursor"); get(FreePixmap, "XFreePixmap");
        get(SetSelectionOwner, "XSetSelectionOwner"); get(GetSelectionOwner, "XGetSelectionOwner");
        get(ConvertSelection, "XConvertSelection"); get(CheckTypedWindowEvent, "XCheckTypedWindowEvent");
        get(DeleteProperty, "XDeleteProperty"); get(ResourceManagerString, "XResourceManagerString");
        get(UnmapWindow, "XUnmapWindow"); get(MoveWindow, "XMoveWindow"); get(RaiseWindow, "XRaiseWindow");
        get(SetInputFocus, "XSetInputFocus"); get(GetAtomName, "XGetAtomName");
        auto lib = [&](void*& h, const char* name) {
            h = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            return h != nullptr;
        };
        auto from = [&](void* h, auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(h, name)); };
        if (lib(xcursor, "libXcursor.so.1")) {
            from(xcursor, CursorLoad, "XcursorLibraryLoadCursor"); from(xcursor, CursorSetDefaultSize, "XcursorSetDefaultSize");
            from(xcursor, CursorGetDefaultSize, "XcursorGetDefaultSize"); from(xcursor, CursorSetTheme, "XcursorSetTheme");
        }
        if (lib(xrandr, "libXrandr.so.2")) {
            from(xrandr, RRGetMonitors, "XRRGetMonitors"); from(xrandr, RRFreeMonitors, "XRRFreeMonitors");
            from(xrandr, RRGetScreenInfo, "XRRGetScreenInfo"); from(xrandr, RRConfigCurrentRate, "XRRConfigCurrentRate");
            from(xrandr, RRFreeScreenConfigInfo, "XRRFreeScreenConfigInfo");
        }
        return required;
    }
};
// XSetWindowAttributes (LP64 Xlib ABI)
struct SetWindowAttributes {
    unsigned long background_pixmap, background_pixel, border_pixmap, border_pixel;
    int bit_gravity, win_gravity, backing_store;
    unsigned long backing_planes, backing_pixel;
    int save_under;
    long event_mask, do_not_propagate_mask;
    int override_redirect;
    unsigned long colormap, cursor;
};
constexpr unsigned long kCWOverrideRedirect = 1ul << 9, kCWSaveUnder = 1ul << 10;
constexpr long kPopupMask = (1 << 2) | (1 << 3) | (1 << 6) | (1 << 15); // buttons, motion, exposure
int ignoreErrors(Display*, void*) { return 0; } // a capture of an unmapped window must not abort the process

Key mapKey(KeySym s, bool& ascii) {
    ascii = false;
    switch (s) {
    case 0xff09: case 0xfe20: return Key::Tab;
    case 0xff0d: case 0xff8d: return Key::Enter;
    case 0xff1b: return Key::Escape;
    case 0xff08: return Key::Backspace;
    case 0xffff: return Key::Delete;
    case 0xff51: return Key::Left;
    case 0xff52: return Key::Up;
    case 0xff53: return Key::Right;
    case 0xff54: return Key::Down;
    case 0xff50: return Key::Home;
    case 0xff57: return Key::End;
    case 0xff55: return Key::PageUp;
    case 0xff56: return Key::PageDown;
    case 0xffbf: return Key::F2;
    case 0xffc2: return Key::F5;
    case 0xffc7: return Key::F10;
    case 0xffbe: return Key::F1;
    case 0xffc0: return Key::F3;
    case 0xffc1: return Key::F4;
    case 0xffc3: return Key::F6;
    case 0xffc4: return Key::F7;
    case 0xffc5: return Key::F8;
    case 0xffc6: return Key::F9;
    case 0xffc8: return Key::F11;
    case 0xffc9: return Key::F12;
    default: break;
    }
    Key k = Key::Other;
    ascii = s < 0x80 && asciiKey(k, u32(s));
    return k;
}
u8 mods(unsigned state) {
    return u8(((state & 1) ? kModShift : 0) | ((state & 4) ? kModCtrl : 0) | ((state & 8) ? kModAlt : 0) |
              ((state & 64) ? kModSuper : 0));
}
} // namespace x11
#endif

// ---- shared ----------------------------------------------------------------------------------------------------------
struct NativePlatform::Impl {
    struct Rec {
        Impl* impl = nullptr;
        AppWindow* win = nullptr;
        detail::VkWin* vk = nullptr;
        PresenterKind kind = PresenterKind::Headless;
        unsigned long long handle = 0; ///< X11 Window / HWND
        bool mapped = false;
        bool focused = false;
        f32 scale = 1.f;                 ///< UI units -> window pixels (HiDPI; see NativeOptions::scale)
        Cursor cursor = Cursor::Arrow;   ///< shape last applied to the OS window
        bool cursorApplied = false;
        bool hideAsked = false;          ///< the app hid the window (an unmap is not a minimise)
        std::vector<u32> staged;         ///< X11 software presenter: the frame in the server's pixel order
        struct Popup {
            NodeHandle node;               ///< tool windows: the "os-window" node shown
            unsigned long long handle = 0;
            Rect screen;   ///< physical screen rect
            Rect rect;     ///< context rect (input offset)
            u64 revision = ~0ull;
            Image img;
            std::vector<u32> staged; ///< OS pixel order
        };
        std::vector<Popup> popups;
        std::vector<Popup> tools;        ///< OS tool windows (NativeOptions::osWindows), back to front
        int originX = 0, originY = 0;    ///< client origin on screen (px), as of the last present / configure
        /// Custom chrome without a window manager (X11): the backend moves / resizes while the button is held.
        struct ChromeDrag {
            bool active = false;
            HitArea area = HitArea::Client;
            int rootX = 0, rootY = 0;    ///< pointer at the press (screen px)
            int x = 0, y = 0, w = 0, h = 0; ///< window rect at the press (screen px)
        } chromeDrag;
        unsigned long captionClickTime = 0; ///< X11: server time of the last caption press (double click = maximise)
        Vec2 pointer{-1.f, -1.f};        ///< last pointer position (UI units): resize cursors over Custom chrome edges
        void* dropTarget = nullptr;      ///< Win32: the registered IDropTarget
#if defined(_WIN32)
        std::vector<u32> bgra; ///< GDI fallback staging
        wchar_t highSurrogate = 0;
        bool fullscreen = false;         ///< borderless on the whole monitor; `restore*` = what to go back to
        LONG restoreStyle = 0;
        WINDOWPLACEMENT restorePlacement{};
#endif
    };

    Application& app;
    NativeOptions opts;
    std::vector<Rec*> recs;
    detail::VkCtx* vk = nullptr;
    bool vkTried = false;
    std::string error;
    EventSource previous{};
#if defined(_WIN32)
    HINSTANCE hinst = nullptr;
    HCURSOR cursors[kCursorCount] = {};
    f32 windowScale(HWND hwnd) const; ///< NativeOptions::scale, else the window's monitor DPI / 96
    void initServices();              ///< DPI awareness + the system theme
    void leaveFullscreen(Rec& r);
    SystemTheme queryTheme() const;
    bool oleReady = false; ///< OleInitialize succeeded: drop targets and DoDragDrop are available
    bool dragOut = false;  ///< inside DoDragDrop: a drag of the UI is over other applications
    void registerDrop(Rec& r);
    void revokeDrop(Rec& r);
    void dragOutCheck(Rec& r);
#else
    std::unique_ptr<detail::WaylandPlatform> wl; ///< set: Wayland is the window system, everything X11 below is unused
    x11::Lib x;
    x11::Display* dpy = nullptr;
    x11::Atom wmProtocols = 0, wmDelete = 0;
    x11::Window helper = 0; ///< unmapped: owns the CLIPBOARD selection and receives conversions
    x11::Atom aClipboard = 0, aUtf8 = 0, aTargets = 0, aTextPlain = 0, aIncr = 0, aClipProp = 0, aNetWmState = 0, aFullscreen = 0, aMaxV = 0,
              aMaxH = 0, aHidden = 0, aActive = 0, aChangeState = 0, aNetWmName = 0, aWmCheck = 0, aWorkArea = 0;
    /// A selection this platform owns: CLIPBOARD, PRIMARY (formats in services.hpp ClipboardItem).
    struct Selection {
        std::vector<ClipboardItem> items;
        bool owner = false;
    } clip, primary;
    /// An outgoing INCR transfer: the rest is sent chunk by chunk as the requestor deletes the property.
    struct IncrOut {
        x11::Window requestor = 0;
        x11::Atom property = 0, target = 0;
        std::string bytes;
        std::size_t offset = 0;
    };
    std::vector<IncrOut> incrOut;
    x11::Atom aPrimary = 1 /*XA_PRIMARY*/, aImagePng = 0, aTextBare = 0, aMotifHints = 0, aMoveResize = 0;
    // XDND v5
    struct Dnd {
        x11::Atom aware = 0, enter = 0, position = 0, status = 0, leave = 0, drop = 0, finished = 0, selection = 0, actionCopy = 0,
                  actionMove = 0, typeList = 0, uriList = 0, prop = 0;
        // target side: a drag from another client over one of our windows
        x11::Window source = 0;
        x11::Window window = 0;         ///< our window it is over
        x11::Atom type = 0;             ///< the offered type we take (text/uri-list wins over text)
        Vec2 pos{};                     ///< UI units
        bool dropping = false;          ///< XdndDrop seen, the selection conversion is pending
        // source side: a drag of ours over another client
        Rec* rec = nullptr;             ///< the window the drag started in (null: no outgoing drag)
        x11::Window target = 0;         ///< the XdndAware window under the pointer
        bool accepted = false, dropped = false;
        double deadline = 0.0;          ///< app clock: give up waiting for XdndFinished
        std::vector<std::pair<x11::Atom, std::string>> types; ///< what we serve as XdndSelection
    } dnd;
    void initPlatformAtoms();
    // XSETTINGS (native_platform.inl): DPI, theme and cursor theme from the desktop's settings manager, followed live
    x11::Window xsOwner = 0;    ///< the _XSETTINGS_S<screen> selection owner seen at start (0: no settings manager)
    x11::Atom aXsProp = 0;
    std::string resourceDpi;    ///< RESOURCE_MANAGER at start (Xft.dpi: the fallback below XSETTINGS)
    void readXSettings(bool initial);
    // selections (native_platform.inl)
    Selection* selectionOf(x11::Atom a) { return a == aClipboard ? &clip : a == aPrimary ? &primary : nullptr; }
    bool ownSelection(x11::Atom sel, Selection& s, std::vector<ClipboardItem> items);
    std::vector<x11::Atom> selectionTargets(const Selection& s);
    bool selectionBytes(Selection& s, x11::Atom target, std::string& out);
    void sendProperty(x11::Window requestor, x11::Atom property, x11::Atom target, std::string bytes);
    bool continueIncr(const x11::PropertyEv& e);
    bool convert(x11::Atom selection, x11::Atom target, std::string& out, x11::Atom* type = nullptr, int* format = nullptr);
    std::string atomName(x11::Atom a) const;
    // XDND (native_platform.inl)
    void dndSend(x11::Window to, x11::Atom type, long l1, long l2, long l3, long l4, x11::Window self);
    bool dndClient(Rec& r, const x11::ClientEv& c);
    void dndSelectionNotify(const x11::SelectionEv& e);
    x11::Window dndTargetAt(int rx, int ry);
    void dndOutMotion(Rec& r, const x11::InputEv& in, bool outside);
    bool dndOutRelease(Rec& r, const x11::InputEv& in);
    void dndOutEnd(DropAction result);
    // custom chrome (native_platform.inl)
    bool chromePress(Rec& r, const x11::InputEv& in, Vec2 pos);
    void chromeDragMove(Rec& r, const x11::InputEv& in);
    unsigned long cursors[kCursorCount] = {};
    f32 detectedScale = 1.f; ///< Xft.dpi / env (dpi::x11Scale)
    bool themeQueried = false;
    unsigned long cursorFor(Cursor c);
    void serveSelection(const x11::SelectionRequestEv& rq);
    bool hasWm() const;
    WindowMode readMode(const Rec& r) const;
    bool windowLongs(x11::Window win, x11::Atom prop, x11::Atom type, std::vector<unsigned long>& out) const;
#endif
    void applyCursor(Rec& r);
    static bool windowRequest(void* user, AppWindow& w, WindowRequest what);
    static bool clipSet(void* user, std::string_view utf8);
    static bool clipGet(void* user, std::string& out);
    bool clipboardUsable() const;

    Impl(Application& a, const NativeOptions& o) : app(a), opts(o) {}

    Rec* find(unsigned long long h) {
        for (Rec* r : recs)
            if (r->handle == h) return r;
        return nullptr;
    }
    const Rec* find(const AppWindow& w) const {
        for (const Rec* r : recs)
            if (r->win == &w) return r;
        return nullptr;
    }
    /// Window pixels -> UI units.
    static Vec2 toUi(const Rec& r, f32 px, f32 py) { return {px / r.scale, py / r.scale}; }
    detail::VkCtx* vkContext() {
        if (!vkTried && opts.vulkan) {
            vkTried = true;
#if defined(_WIN32)
            const auto kind = SurfaceSource::Win32;
#else
            const auto kind = SurfaceSource::Xlib;
#endif
            vk = opts.adoptDevice.instance && opts.adoptDevice.device ? detail::vkCtxAdopt(kind, opts.adoptDevice, error)
                                                                      : detail::vkCtxCreate(kind, error);
        }
        return vk;
    }

    static bool present(void* user, AppWindow& w, Rect damage);
    static void destroy(void* user);
    static bool pump(void* user, Application& app, f32 wait);
    void destroyOsWindow(Rec& r);
    bool createOsWindow(Rec& r);
    bool captureOs(const Rec& r, Image& out);
    // OS-window popups
    void syncPopups(Rec& r);
    /// An outside click (grabbed pointer outside every window of the app, or the app deactivated): close every popup.
    static void closeAllPopups(Rec& r) {
        UiContext& ui = r.win->ui();
        std::vector<NodeHandle> nodes;
        for (const auto& ps : ui.popupSurfaces()) nodes.push_back(ps.node);
        for (NodeHandle n : nodes)
            if (ui.alive(n)) fuse::ui::widgets::closePopup(ui, n);
        r.win->requestRedraw();
    }
    void grabPointer(Rec& r, bool on);
    bool screenInfo(const Rec& r, int& ox, int& oy, Rect& work) const; ///< client origin + work area (screen px)
    bool createPopupOs(Rec& r, Rec::Popup& p);
    void movePopupOs(Rec::Popup& p);
    void destroyPopupOs(Rec::Popup& p);
    void blitPopupOs(Rec::Popup& p);
    bool capturePopupOs(const Rec::Popup& p, Image& out);
    bool postClickOs(unsigned long long h, Vec2 at);
    // OS tool windows (native_platform.inl)
    void syncTools(Rec& r);
    bool createToolOs(Rec& r, Rec::Popup& p);
    void raiseToolOs(Rec::Popup& p);
    Rec* findTool(unsigned long long h, std::size_t& idx) {
        for (Rec* r : recs)
            for (std::size_t i = 0; i < r->tools.size(); ++i)
                if (r->tools[i].handle == h) { idx = i; return r; }
        return nullptr;
    }
    bool postInputOs(unsigned long long h, Vec2 at, int kind); ///< 0 move, 1 press, 2 release, 3 close
#if !defined(_WIN32)
    bool captureX(unsigned long win, unsigned w, unsigned h, Image& out);
#endif
    Rec* findPopup(unsigned long long h, std::size_t& idx) {
        for (Rec* r : recs)
            for (std::size_t i = 0; i < r->popups.size(); ++i)
                if (r->popups[i].handle == h) { idx = i; return r; }
        return nullptr;
    }
};

// The Wayland seam: with the Wayland backend open (Linux / BSD) the public call goes to it.
#if defined(_WIN32)
#define FUSE_APP_WL(call)
#define FUSE_APP_WL_NONE(value)
#else
#define FUSE_APP_WL(call) \
    if (m->wl) return m->wl->call
/// For what the Wayland backend does not have: the "unsupported" answer, before any X11 state is looked at.
#define FUSE_APP_WL_NONE(value) \
    if (m->wl) return value
#endif

void NativePlatform::Impl::syncPopups(Rec& r) {
    if (!opts.osPopups || !r.handle) return;
    UiContext& ui = r.win->ui();
    int ox = 0, oy = 0;
    Rect work;
    const f32 s = r.scale; // UI units = physical px / scale
    if (screenInfo(r, ox, oy, work)) ui.setOsPopups(true, {(work.x - f32(ox)) / s, (work.y - f32(oy)) / s, work.w / s, work.h / s});
    const auto& surf = ui.popupSurfaces();
    const bool had = !r.popups.empty();
    while (r.popups.size() > surf.size()) {
        destroyPopupOs(r.popups.back());
        r.popups.pop_back();
    }
    if (had && r.popups.empty()) grabPointer(r, false);
    for (std::size_t i = 0; i < surf.size(); ++i) {
        if (i == r.popups.size()) r.popups.emplace_back();
        Rec::Popup& p = r.popups[i];
        const auto& ps = surf[i];
        const Rect scr{std::round(f32(ox) + ps.rect.x * s), std::round(f32(oy) + ps.rect.y * s), std::max(1.f, std::ceil(ps.rect.w * s)),
                       std::max(1.f, std::ceil(ps.rect.h * s))};
        const bool moved = scr.x != p.screen.x || scr.y != p.screen.y || scr.w != p.screen.w || scr.h != p.screen.h;
        p.rect = ps.rect;
        p.screen = scr;
        if (!p.handle) {
            if (!createPopupOs(r, p)) { r.popups.pop_back(); return; }
            if (i == 0) grabPointer(r, true); // Qt::Popup: clicks anywhere else on screen close the menu
        } else if (moved) {
            movePopupOs(p);
        }
        if (moved || p.revision != ps.revision) {
            p.revision = ps.revision;
            u32 clear = 0xFF000000u; // the panel's own fill shows in the corners its radius leaves
            for (const DrawCmd& c : ps.draw.cmds())
                if (c.op != DrawOp::Shadow && c.fill.a > 0.9f) { clear = packPremultiplied(c.fill) | 0xFF000000u; break; }
            p.img.resize(int(scr.w), int(scr.h), clear);
            CpuRasterizer rz = r.win->rasterizer(); // the window's images + the current glyph atlas
            rz.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
            rz.render(ps.draw, p.img); // already device pixels, popup-local (UiContext::PopupSurface)
            blitPopupOs(p);
        }
    }
}

bool NativePlatform::Impl::present(void* user, AppWindow& w, Rect damage) {
    Rec& r = *static_cast<Rec*>(user);
    switch (r.kind) {
    case PresenterKind::Vulkan: {
        const bool ok = detail::vkWinPresent(r.vk, w, damage);
        r.impl->syncPopups(r);
        r.impl->syncTools(r);
        return ok;
    }
#if defined(_WIN32)
    case PresenterKind::Software: {
        const Image& img = w.rasterize(damage);
        r.bgra.resize(img.pixels.size());
        for (std::size_t i = 0; i < img.pixels.size(); ++i) {
            const u32 p = img.pixels[i];
            r.bgra[i] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = img.width;
        bi.bmiHeader.biHeight = -img.height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HWND hwnd = reinterpret_cast<HWND>(r.handle);
        HDC dc = GetDC(hwnd);
        SetDIBitsToDevice(dc, 0, 0, DWORD(img.width), DWORD(img.height), 0, 0, 0, UINT(img.height), r.bgra.data(), &bi,
                          DIB_RGB_COLORS);
        ReleaseDC(hwnd, dc);
        r.impl->syncPopups(r);
        r.impl->syncTools(r);
        return true;
    }
#else
    case PresenterKind::Software: { // no Vulkan: the X server shows the CPU raster (XPutImage, TrueColor 24 / 32)
        const Image& img = w.rasterize(damage);
        Impl& m = *r.impl;
        auto& x = m.x;
        const int scr = x.DefaultScreen(m.dpy);
        r.staged.resize(img.pixels.size());
        for (std::size_t i = 0; i < img.pixels.size(); ++i) { // 0xAABBGGRR -> 0x00RRGGBB
            const u32 q = img.pixels[i];
            r.staged[i] = ((q & 0xFFu) << 16) | (q & 0xFF00u) | ((q >> 16) & 0xFFu);
        }
        if (x11::Image* xi = x.CreateImage(m.dpy, x.DefaultVisual(m.dpy, scr), unsigned(x.DefaultDepth(m.dpy, scr)), 2 /*ZPixmap*/, 0,
                                           reinterpret_cast<char*>(r.staged.data()), unsigned(img.width), unsigned(img.height), 32, 0)) {
            x.PutImage(m.dpy, x11::Window(r.handle), x.DefaultGC(m.dpy, scr), xi, 0, 0, 0, 0, unsigned(img.width), unsigned(img.height));
            xi->data = nullptr; // ours (XDestroyImage would free it)
            xi->f.destroy(xi);
            x.Flush(m.dpy);
        }
        r.impl->syncPopups(r);
        r.impl->syncTools(r);
        return true;
    }
#endif
    default: w.rasterize(damage); r.impl->syncPopups(r); r.impl->syncTools(r); return true;
    }
}

void NativePlatform::Impl::destroy(void* user) {
    Rec* r = static_cast<Rec*>(user);
    Impl& m = *r->impl;
    detail::vkWinDestroy(r->vk);
    r->vk = nullptr;
    for (Rec::Popup& p : r->popups) m.destroyPopupOs(p);
    r->popups.clear();
    for (Rec::Popup& p : r->tools) m.destroyPopupOs(p);
    r->tools.clear();
#if !defined(_WIN32)
    if (m.dnd.rec == r) m.dnd.rec = nullptr; // an outgoing drag of this window ends with it
#endif
    if (r->win) r->win->ui().setOsWindows(false);
    if (r->win) r->win->ui().setOsPopups(false);
    if (r->win) {
        r->win->ui().setClipboard({}); // back to the context-local buffer
        r->win->setWindowBackend({});
    }
    m.destroyOsWindow(*r);
    std::erase(m.recs, r);
    delete r;
}

#if defined(_WIN32)
// ---- Win32 -----------------------------------------------------------------------------------------------------------
namespace {
constexpr const wchar_t* kClass = L"FuseAppWindow";
Key mapVk(WPARAM vk, bool& ascii) {
    ascii = false;
    switch (vk) {
    case VK_TAB: return Key::Tab;
    case VK_RETURN: return Key::Enter;
    case VK_ESCAPE: return Key::Escape;
    case VK_SPACE: return Key::Space;
    case VK_BACK: return Key::Backspace;
    case VK_DELETE: return Key::Delete;
    case VK_LEFT: return Key::Left;
    case VK_RIGHT: return Key::Right;
    case VK_UP: return Key::Up;
    case VK_DOWN: return Key::Down;
    case VK_HOME: return Key::Home;
    case VK_END: return Key::End;
    case VK_PRIOR: return Key::PageUp;
    case VK_NEXT: return Key::PageDown;
    case VK_F2: return Key::F2;
    case VK_F5: return Key::F5;
    case VK_F10: return Key::F10;
    case VK_F1: return Key::F1;
    case VK_F3: return Key::F3;
    case VK_F4: return Key::F4;
    case VK_F6: return Key::F6;
    case VK_F7: return Key::F7;
    case VK_F8: return Key::F8;
    case VK_F9: return Key::F9;
    case VK_F11: return Key::F11;
    case VK_F12: return Key::F12;
    case VK_OEM_4: return Key::BracketLeft;
    case VK_OEM_6: return Key::BracketRight;
    default: break;
    }
    Key k = Key::Other;
    ascii = vk < 0x80 && vk != 0x5B && vk != 0x5D && asciiKey(k, u32(vk)); // 0x5B / 0x5D are VK_LWIN / VK_APPS, not [ ]
    return k;
}
u8 winMods() {
    return u8(((GetKeyState(VK_SHIFT) & 0x8000) ? kModShift : 0) | ((GetKeyState(VK_CONTROL) & 0x8000) ? kModCtrl : 0) |
              ((GetKeyState(VK_MENU) & 0x8000) ? kModAlt : 0) |
              (((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) ? kModSuper : 0));
}
Vec2 lparamPos(LPARAM lp) { return {f32(short(LOWORD(lp))), f32(short(HIWORD(lp)))}; }
/// Client pixels -> UI units.
Vec2 uiPos(const NativePlatform::Impl::Rec& r, LPARAM lp) {
    const Vec2 p = lparamPos(lp);
    return {p.x / r.scale, p.y / r.scale};
}
// native_platform_win32.inl: OS tool windows and client-side chrome
constexpr const wchar_t* kToolClass = L"FuseAppToolWindow";
LRESULT CALLBACK toolProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
bool chromeMessage(HWND hwnd, NativePlatform::Impl::Rec& r, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
DWORD chromeStyle(WindowFrame f);
void chromeAdjust(const AppWindow& w, RECT& rc);

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* r = reinterpret_cast<NativePlatform::Impl::Rec*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!r || !r->win) return DefWindowProcW(hwnd, msg, wp, lp);
    AppWindow& w = *r->win;
    UiContext& ui = w.ui();
    if (LRESULT chrome = 0; chromeMessage(hwnd, *r, msg, wp, lp, chrome)) return chrome; // Borderless / Custom frames
    switch (msg) {
    case WM_CLOSE: w.tryClose(); return 0; // the close guard may veto
    case WM_ACTIVATEAPP: // another application took the pointer / focus: popups close (Qt::Popup)
        if (!wp && !r->popups.empty()) NativePlatform::Impl::closeAllPopups(*r);
        break;
    case WM_ACTIVATE: w.notifyActive(LOWORD(wp) != WA_INACTIVE); break;
    case WM_MOVE: w.notifyMoved(uiPos(*r, lp)); break; // client origin on screen
    case WM_SHOWWINDOW: w.notifyVisible(wp != 0); break;
    case WM_GETMINMAXINFO: { // AppWindow::setMinimumSize / setMaximumSize (client area, UI units)
        if (r->fullscreen) break;
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        const Size mn = w.minimumSize(), mx = w.maximumSize();
        RECT fr{0, 0, 0, 0};
        if (w.chrome().frame == WindowFrame::System) // a Borderless / Custom window is all client area
            AdjustWindowRectEx(&fr, DWORD(GetWindowLongW(hwnd, GWL_STYLE)), FALSE, DWORD(GetWindowLongW(hwnd, GWL_EXSTYLE)));
        const LONG fw = fr.right - fr.left, fh = fr.bottom - fr.top;
        if (mn.w > 0.f) mm->ptMinTrackSize.x = LONG(std::lround(mn.w * r->scale)) + fw;
        if (mn.h > 0.f) mm->ptMinTrackSize.y = LONG(std::lround(mn.h * r->scale)) + fh;
        if (mx.w > 0.f) mm->ptMaxTrackSize.x = LONG(std::lround(mx.w * r->scale)) + fw;
        if (mx.h > 0.f) mm->ptMaxTrackSize.y = LONG(std::lround(mx.h * r->scale)) + fh;
        return 0;
    }
    case WM_DPICHANGED: { // moved to a monitor with another DPI: take the suggested rect, re-render at the new scale
        if (r->impl->opts.scale <= 0.f) r->scale = dpi::resolveScale(0.f, dpi::scaleFromDpi(f32(HIWORD(wp))));
        const RECT* sr = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, sr->left, sr->top, sr->right - sr->left, sr->bottom - sr->top, SWP_NOZORDER | SWP_NOACTIVATE);
        RECT c;
        if (GetClientRect(hwnd, &c) && c.right > 0 && c.bottom > 0) w.resizePixels(u32(c.right), u32(c.bottom), r->scale);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { // the UI's cursor over the client area; the frame keeps the system's
            r->impl->applyCursor(*r);
            return TRUE;
        }
        break;
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED: // light / dark preference or accent changed
        r->impl->app.setSystemTheme(r->impl->queryTheme());
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        r->mapped = true;
        w.requestRedraw();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (LOWORD(lp) && HIWORD(lp)) w.resizePixels(LOWORD(lp), HIWORD(lp), r->scale);
        w.notifyMode(wp == SIZE_MINIMIZED ? WindowMode::Minimized : r->fullscreen ? WindowMode::Fullscreen
                     : wp == SIZE_MAXIMIZED ? WindowMode::Maximized : WindowMode::Normal);
        return 0;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        ui.setModifiers(u8(((wp & MK_SHIFT) ? kModShift : 0) | ((wp & MK_CONTROL) ? kModCtrl : 0) | ((GetKeyState(VK_MENU) < 0) ? kModAlt : 0)));
        break;
    default: break;
    }
    switch (msg) {
    case WM_MOUSEMOVE:
        ui.pointerMove(uiPos(*r, lp));
        r->impl->applyCursor(*r);
        r->impl->dragOutCheck(*r); // a UI drag that left the app's windows goes on as an OLE drag
        return 0;
    case WM_LBUTTONDOWN: SetCapture(hwnd); ui.pointerDown(uiPos(*r, lp), MouseButton::Left); return 0;
    case WM_RBUTTONDOWN: ui.pointerDown(uiPos(*r, lp), MouseButton::Right); return 0;
    case WM_MBUTTONDOWN: ui.pointerDown(uiPos(*r, lp), MouseButton::Middle); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); ui.pointerUp(uiPos(*r, lp)); return 0;
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: ui.pointerUp(uiPos(*r, lp)); return 0;
    case WM_MOUSEWHEEL: {
        POINT p{short(LOWORD(lp)), short(HIWORD(lp))};
        ScreenToClient(hwnd, &p);
        ui.wheel({f32(p.x) / r->scale, f32(p.y) / r->scale}, {0.f, f32(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA});
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        bool ascii = false;
        const Key k = mapVk(wp, ascii);
        const u8 md = winMods();
        if (k != Key::Other) { // printable keys too: viewport WASD / 1-4; text arrives separately
            if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
                if (!ui.keyDown(k, md)) w.menuShortcut(k, md); // unconsumed: a menu shortcut (AppWindow::setMenu)
            } else {
                ui.keyUp(k, md);
            }
        }
        break;
    }
    case WM_CHAR: {
        const wchar_t c = wchar_t(wp);
        if (c >= 0xD800 && c < 0xDC00) { r->highSurrogate = c; return 0; }
        u32 cp = c;
        if (c >= 0xDC00 && c < 0xE000 && r->highSurrogate) cp = 0x10000u + ((u32(r->highSurrogate) - 0xD800u) << 10) + (c - 0xDC00u);
        r->highSurrogate = 0;
        if (cp < 0x20 || cp == 0x7F || (winMods() & kModCtrl)) return 0;
        std::string s;
        appendUtf8(s, cp);
        ui.textInput(s);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
constexpr const wchar_t* kPopupClass = L"FuseAppPopup";
LRESULT CALLBACK popupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* r = reinterpret_cast<NativePlatform::Impl::Rec*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE; // the main window keeps keyboard focus (Qt popup behaviour)
    if (!r || !r->win) return DefWindowProcW(hwnd, msg, wp, lp);
    std::size_t idx = 0;
    if (!r->impl->findPopup(reinterpret_cast<unsigned long long>(hwnd), idx)) return DefWindowProcW(hwnd, msg, wp, lp);
    NativePlatform::Impl::Rec::Popup& p = r->popups[idx];
    UiContext& ui = r->win->ui();
    const f32 s = r->scale;
    const Vec2 lp2 = lparamPos(lp);
    const Vec2 pos{p.rect.x + lp2.x / s, p.rect.y + lp2.y / s};
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        r->impl->blitPopupOs(p);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: ui.pointerMove(pos); r->win->requestRedraw(); return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        if (lp2.x < 0.f || lp2.y < 0.f || lp2.x >= p.screen.w || lp2.y >= p.screen.h) { // captured click elsewhere
            NativePlatform::Impl::closeAllPopups(*r);
            return 0;
        }
        if (msg == WM_LBUTTONDOWN) ui.pointerDown(pos, MouseButton::Left);
        else ui.pointerDown(pos, MouseButton::Right);
        r->win->requestRedraw();
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: ui.pointerUp(pos); r->win->requestRedraw(); return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

bool NativePlatform::Impl::screenInfo(const Rec& r, int& ox, int& oy, Rect& work) const {
    HWND hwnd = reinterpret_cast<HWND>(r.handle);
    POINT o{0, 0};
    if (!ClientToScreen(hwnd, &o)) return false;
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    ox = o.x;
    oy = o.y;
    work = {f32(mi.rcWork.left), f32(mi.rcWork.top), f32(mi.rcWork.right - mi.rcWork.left), f32(mi.rcWork.bottom - mi.rcWork.top)};
    return true;
}
void NativePlatform::Impl::grabPointer(Rec&, bool) {} // Win32: WM_ACTIVATEAPP closes popups instead of a grab
bool NativePlatform::Impl::createPopupOs(Rec& r, Rec::Popup& p) {
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kPopupClass, L"", WS_POPUP, int(p.screen.x),
                                int(p.screen.y), int(p.screen.w), int(p.screen.h), reinterpret_cast<HWND>(r.handle), nullptr, hinst, nullptr);
    if (!hwnd) return false;
    p.handle = reinterpret_cast<unsigned long long>(hwnd);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&r));
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return true;
}
void NativePlatform::Impl::movePopupOs(Rec::Popup& p) {
    SetWindowPos(reinterpret_cast<HWND>(p.handle), nullptr, int(p.screen.x), int(p.screen.y), int(p.screen.w), int(p.screen.h),
                 SWP_NOACTIVATE | SWP_NOZORDER);
}
void NativePlatform::Impl::destroyPopupOs(Rec::Popup& p) {
    if (!p.handle) return;
    HWND hwnd = reinterpret_cast<HWND>(p.handle);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    p.handle = 0;
}
void NativePlatform::Impl::blitPopupOs(Rec::Popup& p) {
    if (!p.handle || p.img.pixels.empty()) return;
    p.staged.resize(p.img.pixels.size());
    for (std::size_t i = 0; i < p.img.pixels.size(); ++i) {
        const u32 q = p.img.pixels[i];
        p.staged[i] = (q & 0xFF00FF00u) | ((q & 0xFFu) << 16) | ((q >> 16) & 0xFFu);
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = p.img.width;
    bi.bmiHeader.biHeight = -p.img.height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HWND hwnd = reinterpret_cast<HWND>(p.handle);
    HDC dc = GetDC(hwnd);
    SetDIBitsToDevice(dc, 0, 0, DWORD(p.img.width), DWORD(p.img.height), 0, 0, 0, UINT(p.img.height), p.staged.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(hwnd, dc);
}
bool NativePlatform::Impl::postClickOs(unsigned long long h, Vec2 at) {
    HWND hwnd = reinterpret_cast<HWND>(h);
    const LPARAM lp = MAKELPARAM(int(at.x), int(at.y));
    return PostMessageW(hwnd, WM_MOUSEMOVE, 0, lp) && PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp) &&
           PostMessageW(hwnd, WM_LBUTTONUP, 0, lp);
}

bool NativePlatform::Impl::createOsWindow(Rec& r) {
    const Size sz = r.win->size();
    const DWORD style = chromeStyle(r.win->chrome().frame);
    RECT rc{0, 0, LONG(sz.w), LONG(sz.h)};
    chromeAdjust(*r.win, rc);
    const std::string& t = r.win->title();
    std::wstring wt(std::size_t(MultiByteToWideChar(CP_UTF8, 0, t.data(), int(t.size()), nullptr, 0)), L'\0');
    if (!wt.empty()) MultiByteToWideChar(CP_UTF8, 0, t.data(), int(t.size()), wt.data(), int(wt.size()));
    HWND hwnd = CreateWindowExW(0, kClass, wt.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hinst, nullptr);
    if (!hwnd) {
        error = "CreateWindowExW failed";
        return false;
    }
    r.handle = reinterpret_cast<unsigned long long>(hwnd);
    // HiDPI: the client area is UI size x the scale of the monitor the window landed on (NativeOptions::scale)
    r.scale = windowScale(hwnd);
    const Size px = dpi::toPixels(sz, r.scale);
    RECT want{0, 0, LONG(px.w), LONG(px.h)};
    chromeAdjust(*r.win, want);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&r));
    // SWP_FRAMECHANGED: a Custom frame's first WM_NCCALCSIZE ran before the window knew its AppWindow
    SetWindowPos(hwnd, nullptr, 0, 0, want.right - want.left, want.bottom - want.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RECT client;
    if (GetClientRect(hwnd, &client) && client.right > 0 && client.bottom > 0) r.win->resizePixels(u32(client.right), u32(client.bottom), r.scale);
    if (r.win->positionRequested()) windowRequest(&r, *r.win, WindowRequest::Position); // a saved position
    registerDrop(r);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return true;
}
void NativePlatform::Impl::destroyOsWindow(Rec& r) {
    if (!r.handle) return;
    HWND hwnd = reinterpret_cast<HWND>(r.handle);
    revokeDrop(r);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    r.handle = 0;
}
bool NativePlatform::Impl::pump(void* user, Application&, f32 wait) {
    (void)user;
    MSG msg;
    if (wait > 0.f && !PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE)) {
        const DWORD ms = std::isinf(wait) ? INFINITE : DWORD(std::ceil(double(wait) * 1000.0));
        MsgWaitForMultipleObjects(0, nullptr, FALSE, ms, QS_ALLINPUT);
    }
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}
static bool captureHwnd(HWND hwnd, Image& out) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    HDC wdc = GetDC(hwnd);
    HDC mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    const bool okBlt = BitBlt(mdc, 0, 0, w, h, wdc, 0, 0, SRCCOPY) != 0;
    GdiFlush();
    if (okBlt && bits) {
        out.resize(w, h);
        const u32* src = static_cast<const u32*>(bits);
        for (std::size_t i = 0; i < out.pixels.size(); ++i) {
            const u32 p = src[i];
            out.pixels[i] = 0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
        }
    }
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(hwnd, wdc);
    return okBlt;
}
bool NativePlatform::Impl::captureOs(const Rec& r, Image& out) {
    // GDI cannot read a Vulkan-presented client area (DWM flip model on Windows; a separate X drawable under Wine), so
    // a Vulkan window reports the frame it presented; the GDI fallback is read from the window DC.
    if (r.vk) return detail::vkWinReadback(r.vk, out);
    return captureHwnd(reinterpret_cast<HWND>(r.handle), out);
}
bool NativePlatform::Impl::capturePopupOs(const Rec::Popup& p, Image& out) {
    return p.handle && captureHwnd(reinterpret_cast<HWND>(p.handle), out);
}

NativePlatform::NativePlatform(Application& app, const NativeOptions& o) : m(std::make_unique<Impl>(app, o)) {
    m->hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &wndProc;
    wc.hInstance = m->hinst;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc); // fails harmlessly when a second NativePlatform registers again
    WNDCLASSEXW pc = wc;
    pc.style = CS_SAVEBITS | CS_DROPSHADOW;
    pc.lpfnWndProc = &popupProc;
    pc.lpszClassName = kPopupClass;
    RegisterClassExW(&pc);
    WNDCLASSEXW tc = wc;
    tc.lpfnWndProc = &toolProc;
    tc.lpszClassName = kToolClass;
    RegisterClassExW(&tc);
    m->oleReady = o.dragAndDrop && SUCCEEDED(OleInitialize(nullptr)); // drop targets + DoDragDrop need OLE on this thread
    m->initServices();
    app.setEventSource({m.get(), &Impl::pump});
}
bool NativePlatform::available() const { return true; }
void* NativePlatform::nativeDisplay() const { return m->hinst; }

#else
// ---- X11 platform ------------------------------------------------------------------------------------------------
bool NativePlatform::Impl::createOsWindow(Rec& r) {
    r.scale = dpi::resolveScale(opts.scale, detectedScale);
    const Size sz = dpi::toPixels(r.win->size(), r.scale); // the X window is physical pixels
    const int scr = x.DefaultScreen(dpy);
    const x11::Window w = x.CreateSimpleWindow(dpy, x.RootWindow(dpy, scr), 0, 0, unsigned(sz.w), unsigned(sz.h), 0, 0, 0);
    if (!w) {
        error = "XCreateSimpleWindow failed";
        return false;
    }
    x.StoreName(dpy, w, r.win->title().c_str());
    x.SelectInput(dpy, w, x11::kMask);
    x.SetWMProtocols(dpy, w, &wmDelete, 1);
    if (x.DefineCursor)
        if (const unsigned long arrow = cursorFor(Cursor::Arrow)) x.DefineCursor(dpy, w, arrow); // a bare X root shows an X
    if (r.win->chrome().frame != WindowFrame::System) {
        // custom chrome: _MOTIF_WM_HINTS { flags = decorations, functions, decorations = none, input mode, status }
        const long hints[5] = {2, 0, 0, 0, 0};
        x.ChangeProperty(dpy, w, aMotifHints, aMotifHints, 32, 0, reinterpret_cast<const unsigned char*>(hints), 5);
    }
    if (opts.dragAndDrop) { // XDND v5 drop target
        const long version = 5;
        x.ChangeProperty(dpy, w, dnd.aware, 4 /*XA_ATOM*/, 32, 0, reinterpret_cast<const unsigned char*>(&version), 1);
    }
    if (r.win->positionRequested() && x.MoveWindow) { // placed before it is mapped (a saved position)
        const Vec2 at = r.win->requestedPosition();
        x.MoveWindow(dpy, w, int(std::lround(at.x * r.scale)), int(std::lround(at.y * r.scale)));
    }
    x.MapWindow(dpy, w);
    x.Flush(dpy);
    r.handle = w;
    r.win->resizePixels(u32(sz.w), u32(sz.h), r.scale);
    return true;
}
void NativePlatform::Impl::destroyOsWindow(Rec& r) {
    if (!r.handle || !dpy) return;
    x.DestroyWindow(dpy, x11::Window(r.handle));
    x.Flush(dpy);
    r.handle = 0;
}
bool NativePlatform::Impl::pump(void* user, Application& app, f32 wait) {
    Impl& m = *static_cast<Impl*>(user);
    if (!m.dpy) return m.previous.pump ? m.previous.pump(m.previous.user, app, wait) : true;
    auto& x = m.x;
    if (m.dnd.rec && m.dnd.dropped) wait = std::min(wait, 0.05f); // a drop of ours is waiting for XdndFinished
    if (wait > 0.f && !x.Pending(m.dpy)) {
        pollfd p{x.ConnectionNumber(m.dpy), POLLIN, 0};
        const int ms = std::isinf(wait) ? -1 : int(std::min(std::ceil(double(wait) * 1000.0), 2.0e9));
        ::poll(&p, 1, ms);
    }
    while (x.Pending(m.dpy)) {
        x11::Event ev{};
        x.NextEvent(m.dpy, &ev);
        if (ev.type == x11::SelectionRequest) { m.serveSelection(ev.selectionRequest); continue; }
        if (ev.type == x11::SelectionClear) { // another client took the selection
            if (Selection* sel = m.selectionOf(ev.selectionClear.selection)) *sel = {};
            continue;
        }
        if (ev.type == x11::SelectionNotify) { // the data of an XDND drop; else a late answer to a conversion we gave up on
            if (ev.selection.selection == m.dnd.selection) m.dndSelectionNotify(ev.selection);
            continue;
        }
        if (ev.type == x11::PropertyNotify && m.xsOwner && ev.property.window == m.xsOwner) { // the desktop settings changed
            if (ev.property.atom == m.aXsProp) m.readXSettings(false);
            continue;
        }
        if (ev.type == x11::PropertyNotify && m.continueIncr(ev.property)) continue;
        bool fromTool = false;
        std::size_t ti = 0;
        if (Rec* tr = m.findTool(ev.any.window, ti)) { // a tool window: its input is the owner's, in context coordinates
            if (ev.type == x11::Expose) { m.blitPopupOs(tr->tools[ti]); continue; }
            if (ev.type != x11::MotionNotify && ev.type != x11::ButtonPress && ev.type != x11::ButtonRelease && ev.type != x11::KeyPress &&
                ev.type != x11::KeyRelease)
                continue;
            if (ev.type == x11::ButtonPress) m.raiseToolOs(tr->tools[ti]);
            // through the screen position: the window may move under the pointer while its title is dragged
            ev.input.x = ev.input.x_root - tr->originX;
            ev.input.y = ev.input.y_root - tr->originY;
            ev.input.window = x11::Window(tr->handle);
            fromTool = true;
        }
        std::size_t pi = 0;
        if (Rec* pr = m.findPopup(ev.any.window, pi)) { // a popup window: input in context coordinates of its owner
            Rec::Popup& p = pr->popups[pi];
            UiContext& ui = pr->win->ui();
            const f32 s = pr->scale;
            const Vec2 pos{p.rect.x + f32(ev.input.x) / s, p.rect.y + f32(ev.input.y) / s};
            if (ev.type == x11::Expose) m.blitPopupOs(p);
            else if (ev.type == x11::MotionNotify) { ui.setModifiers(x11::mods(ev.input.state)); ui.pointerMove(pos); }
            else if (ev.type == x11::ButtonPress && (ev.input.x < 0 || ev.input.y < 0 || f32(ev.input.x) >= p.screen.w ||
                                                     f32(ev.input.y) >= p.screen.h)) {
                closeAllPopups(*pr); // grabbed click outside every window of the app
                continue;
            } else if (ev.type == x11::ButtonPress) {
                ui.setModifiers(x11::mods(ev.input.state));
                if (ev.input.detail == 4 || ev.input.detail == 5) ui.wheel(pos, {0.f, ev.input.detail == 4 ? 1.f : -1.f});
                else ui.pointerDown(pos, ev.input.detail == 3 ? MouseButton::Right : MouseButton::Left);
            } else if (ev.type == x11::ButtonRelease && ev.input.detail <= 3) ui.pointerUp(pos);
            pr->win->requestRedraw();
            continue;
        }
        Rec* r = m.find(ev.any.window);
        if (ev.type == x11::ConfigureNotify) r = m.find(ev.configure.window);
        if (!r || !r->win) continue;
        AppWindow& w = *r->win;
        UiContext& ui = w.ui();
        const x11::InputEv& in = ev.input;
        const Vec2 pos = toUi(*r, f32(in.x), f32(in.y));
        switch (ev.type) {
        case x11::ClientMessage:
            if (ev.client.message_type == m.wmProtocols && x11::Atom(ev.client.l[0]) == m.wmDelete) w.tryClose();
            else m.dndClient(*r, ev.client); // XDND: a drag over this window, or the answers to one of ours
            break;
        case x11::MapNotify:
            r->mapped = true;
            r->hideAsked = false;
            w.notifyVisible(true);
            w.notifyMode(m.readMode(*r));
            w.requestRedraw();
            break;
        case x11::UnmapNotify:
            r->mapped = false;
            if (r->hideAsked) w.notifyVisible(false); // else: iconified by the window manager (_NET_WM_STATE says so)
            break;
        case x11::PropertyNotify:
            if (ev.property.atom == m.aNetWmState) w.notifyMode(m.readMode(*r));
            break;
        case x11::FocusIn:
        case x11::FocusOut:
            if (ev.focus.mode == 1 || ev.focus.mode == 2) break; // NotifyGrab / NotifyUngrab: a menu's grab, not a focus change
            r->focused = ev.type == x11::FocusIn;
            w.notifyActive(r->focused);
            break;
        case x11::Expose: r->mapped = true; w.requestRedraw(); break;
        case x11::ConfigureNotify: {
            const Size s = w.pixelSize();
            if (ev.configure.width > 0 && ev.configure.height > 0 &&
                (u32(ev.configure.width) != u32(s.w) || u32(ev.configure.height) != u32(s.h)))
                w.resizePixels(u32(ev.configure.width), u32(ev.configure.height), r->scale);
            int ox = 0, oy = 0;
            Rect work;
            if (m.screenInfo(*r, ox, oy, work)) {
                r->originX = ox;
                r->originY = oy;
                w.notifyMoved(toUi(*r, f32(ox), f32(oy)));
                if (!r->tools.empty()) w.requestRedraw(); // the tool windows follow the main window
            }
            break;
        }
        case x11::MotionNotify: {
            if (r->chromeDrag.active) { m.chromeDragMove(*r, in); break; }
            ui.setModifiers(x11::mods(in.state));
            r->pointer = pos;
            ui.pointerMove(pos);
            // a drag of ours that left the app's windows continues over other applications (XDND source)
            const Size vp = ui.viewport();
            const bool outside = pos.x < 0.f || pos.y < 0.f || pos.x >= vp.w || pos.y >= vp.h;
            if (m.opts.dragAndDrop && (m.dnd.rec == r || (outside && ui.dragging()))) m.dndOutMotion(*r, in, outside);
            break;
        }
        case x11::ButtonPress:
            ui.setModifiers(x11::mods(in.state));
            if (in.detail == 4 || in.detail == 5) ui.wheel(pos, {0.f, in.detail == 4 ? 1.f : -1.f});
            else if (in.detail == 6 || in.detail == 7) ui.wheel(pos, {in.detail == 6 ? -1.f : 1.f, 0.f});
            else if (in.detail == 1 && !fromTool && m.chromePress(*r, in, pos)) break; // caption / edge of a Custom frame
            else ui.pointerDown(pos, in.detail == 3 ? MouseButton::Right : in.detail == 2 ? MouseButton::Middle : MouseButton::Left);
            break;
        case x11::ButtonRelease:
            ui.setModifiers(x11::mods(in.state));
            if (in.detail == 1 && r->chromeDrag.active) { r->chromeDrag.active = false; break; }
            if (in.detail == 1 && m.dnd.rec == r && m.dndOutRelease(*r, in)) break; // dropped on another application
            if (in.detail <= 3) ui.pointerUp(pos);
            break;
        case x11::KeyPress:
        case x11::KeyRelease: {
            char buf[32];
            x11::KeySym sym = 0;
            x11::InputEv copy = in;
            const int n = x.LookupString(&copy, buf, int(sizeof buf), &sym, nullptr);
            bool ascii = false;
            const Key k = x11::mapKey(sym, ascii);
            const u8 md = x11::mods(in.state);
            if (k != Key::Other) { // printable keys too: viewport WASD / 1-4; text arrives separately
                if (ev.type == x11::KeyPress) {
                    if (!ui.keyDown(k, md)) w.menuShortcut(k, md); // unconsumed: a menu shortcut (AppWindow::setMenu)
                } else {
                    ui.keyUp(k, md);
                }
            }
            if (ev.type == x11::KeyPress && n > 0 && !(md & (kModCtrl | kModAlt))) {
                std::string s; // XLookupString yields Latin-1
                for (int i = 0; i < n; ++i)
                    if (u8(buf[i]) >= 0x20 && u8(buf[i]) != 0x7F) appendUtf8(s, u8(buf[i]));
                if (!s.empty()) ui.textInput(s);
            }
            break;
        }
        default: break;
        }
    }
    if (m.dnd.rec && m.dnd.dropped && app.now() > m.dnd.deadline) m.dndOutEnd(DropAction::None); // the target never answered
    for (Rec* r : m.recs)
        if (r->handle && r->win) m.applyCursor(*r); // what the UI wants after this batch of input
    return true;
}
bool NativePlatform::Impl::screenInfo(const Rec& r, int& ox, int& oy, Rect& work) const {
    const int scr = x.DefaultScreen(dpy);
    x11::Window child = 0;
    if (!x.TranslateCoordinates(dpy, x11::Window(r.handle), x.RootWindow(dpy, scr), 0, 0, &ox, &oy, &child)) return false;
    work = {0.f, 0.f, f32(x.DisplayWidth(dpy, scr)), f32(x.DisplayHeight(dpy, scr))};
    return true;
}
bool NativePlatform::Impl::createPopupOs(Rec& r, Rec::Popup& p) {
    (void)r;
    const int scr = x.DefaultScreen(dpy);
    const x11::Window w = x.CreateSimpleWindow(dpy, x.RootWindow(dpy, scr), int(p.screen.x), int(p.screen.y), unsigned(p.screen.w),
                                               unsigned(p.screen.h), 0, 0, 0);
    if (!w) return false;
    x11::SetWindowAttributes a{};
    a.override_redirect = 1; // no WM frame / focus: a menu window (Qt::Popup)
    a.save_under = 1;
    x.ChangeWindowAttributes(dpy, w, x11::kCWOverrideRedirect | x11::kCWSaveUnder, &a);
    const x11::Atom type = x.InternAtom(dpy, "_NET_WM_WINDOW_TYPE", 0), menu = x.InternAtom(dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", 0);
    x.ChangeProperty(dpy, w, type, 4 /*XA_ATOM*/, 32, 0, reinterpret_cast<const unsigned char*>(&menu), 1);
    x.SelectInput(dpy, w, x11::kPopupMask);
    if (x.DefineCursor)
        if (const unsigned long arrow = cursorFor(Cursor::Arrow)) x.DefineCursor(dpy, w, arrow);
    x.MapWindow(dpy, w);
    p.handle = w;
    return true;
}
void NativePlatform::Impl::grabPointer(Rec& r, bool on) {
    if (!on) { x.UngrabPointer(dpy, 0); x.Flush(dpy); return; }
    if (r.popups.empty() || !r.popups[0].handle) return;
    x.Sync(dpy, 0); // the override-redirect window must be viewable before it can grab
    // owner_events: the app's own windows (main + popups) keep their events; everything else reports to the popup
    x.GrabPointer(dpy, x11::Window(r.popups[0].handle), 1, (1u << 2) | (1u << 3) | (1u << 6), 1, 1, 0, 0, 0);
    x.Flush(dpy);
}
void NativePlatform::Impl::movePopupOs(Rec::Popup& p) {
    x.MoveResizeWindow(dpy, x11::Window(p.handle), int(p.screen.x), int(p.screen.y), unsigned(p.screen.w), unsigned(p.screen.h));
}
void NativePlatform::Impl::destroyPopupOs(Rec::Popup& p) {
    if (!p.handle || !dpy) return;
    x.DestroyWindow(dpy, x11::Window(p.handle));
    x.Flush(dpy);
    p.handle = 0;
}
void NativePlatform::Impl::blitPopupOs(Rec::Popup& p) {
    if (!p.handle || p.img.pixels.empty()) return;
    const int scr = x.DefaultScreen(dpy);
    if (x.DefaultDepth(dpy, scr) < 24) return;
    p.staged.resize(p.img.pixels.size());
    for (std::size_t i = 0; i < p.img.pixels.size(); ++i) { // 0xAABBGGRR -> TrueColor 0x00RRGGBB
        const u32 q = p.img.pixels[i];
        p.staged[i] = ((q & 0xFFu) << 16) | (q & 0xFF00u) | ((q >> 16) & 0xFFu);
    }
    x11::Image* img = x.CreateImage(dpy, x.DefaultVisual(dpy, scr), unsigned(x.DefaultDepth(dpy, scr)), 2 /*ZPixmap*/, 0,
                                    reinterpret_cast<char*>(p.staged.data()), unsigned(p.img.width), unsigned(p.img.height), 32, 0);
    if (!img) return;
    x.PutImage(dpy, x11::Window(p.handle), x.DefaultGC(dpy, scr), img, 0, 0, 0, 0, unsigned(p.img.width), unsigned(p.img.height));
    img->data = nullptr; // ours (XDestroyImage would free it)
    img->f.destroy(img);
    x.Flush(dpy);
}
bool NativePlatform::Impl::postClickOs(unsigned long long h, Vec2 at) {
    return postInputOs(h, at, 1) && postInputOs(h, at, 2); // press + release, with the screen position filled in
}
bool NativePlatform::Impl::captureOs(const Rec& r, Image& out) {
    const Size sz = r.win->pixelSize();
    if (captureX(x11::Window(r.handle), unsigned(sz.w), unsigned(sz.h), out)) return true;
    // window not fully on screen (GetImage: BadMatch) or not mapped: a Vulkan window reports the frame it presented
    return r.vk && detail::vkWinReadback(r.vk, out);
}
bool NativePlatform::Impl::capturePopupOs(const Rec::Popup& p, Image& out) {
    return p.handle && captureX(x11::Window(p.handle), unsigned(p.screen.w), unsigned(p.screen.h), out);
}
bool NativePlatform::Impl::captureX(unsigned long win, unsigned w, unsigned h, Image& out) {
    x.Sync(dpy, 0);
    x11::Image* img = x.GetImage(dpy, win, 0, 0, w, h, ~0ul, 2 /*ZPixmap*/);
    if (!img) return false;
    bool good = img->bits_per_pixel == 32;
    if (good) {
        out.resize(img->width, img->height);
        for (int y = 0; y < img->height; ++y) {
            const u32* row = reinterpret_cast<const u32*>(img->data + std::ptrdiff_t(y) * img->bytes_per_line);
            for (int i = 0; i < img->width; ++i) {
                const u32 p = row[i]; // 0x00RRGGBB (TrueColor 24/32)
                out.pixels[std::size_t(y) * std::size_t(img->width) + std::size_t(i)] =
                    0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
            }
        }
    }
    img->f.destroy(img);
    return good;
}

NativePlatform::NativePlatform(Application& app, const NativeOptions& o) : m(std::make_unique<Impl>(app, o)) {
    const char* disp = std::getenv("DISPLAY");
    {
        const char* be = std::getenv("FUSE_UI_BACKEND");
        const char* wd = std::getenv("WAYLAND_DISPLAY");
        const std::string_view env = be ? be : "";
        if (wl::chooseBackend(o.backend, env, wd ? wd : "") == NativeBackend::Wayland) {
            m->wl = detail::WaylandPlatform::open(app, o, m->error);
            if (m->wl || wl::backendForced(o.backend, env)) return; // forced: no fallback to X11
            m->error.clear(); // no compositor / no libwayland-client: X11 (XWayland) when there is a DISPLAY
        }
    }
    if (disp && *disp && m->x.load()) {
        m->dpy = m->x.OpenDisplay(nullptr);
        if (m->dpy) {
            m->x.SetErrorHandler(&x11::ignoreErrors);
            m->wmProtocols = m->x.InternAtom(m->dpy, "WM_PROTOCOLS", 0);
            m->wmDelete = m->x.InternAtom(m->dpy, "WM_DELETE_WINDOW", 0);
            auto atom = [&](const char* name) { return m->x.InternAtom(m->dpy, name, 0); };
            m->aClipboard = atom("CLIPBOARD"); m->aUtf8 = atom("UTF8_STRING"); m->aTargets = atom("TARGETS");
            m->aTextPlain = atom("text/plain;charset=utf-8"); m->aIncr = atom("INCR"); m->aClipProp = atom("FUSE_CLIPBOARD");
            m->aNetWmState = atom("_NET_WM_STATE"); m->aFullscreen = atom("_NET_WM_STATE_FULLSCREEN");
            m->aMaxV = atom("_NET_WM_STATE_MAXIMIZED_VERT"); m->aMaxH = atom("_NET_WM_STATE_MAXIMIZED_HORZ");
            m->aHidden = atom("_NET_WM_STATE_HIDDEN"); m->aActive = atom("_NET_ACTIVE_WINDOW"); m->aChangeState = atom("WM_CHANGE_STATE");
            m->aNetWmName = atom("_NET_WM_NAME"); m->aWmCheck = atom("_NET_SUPPORTING_WM_CHECK"); m->aWorkArea = atom("_NET_WORKAREA");
            const int scr = m->x.DefaultScreen(m->dpy);
            m->helper = m->x.CreateSimpleWindow(m->dpy, m->x.RootWindow(m->dpy, scr), -10, -10, 1, 1, 0, 0, 0); // never mapped
            m->x.SelectInput(m->dpy, m->helper, 1L << 22); // PropertyChange: incoming INCR transfers
            m->initPlatformAtoms();
            const char* rm = m->x.ResourceManagerString ? m->x.ResourceManagerString(m->dpy) : nullptr;
            const char* gdk = std::getenv("GDK_SCALE");
            const char* own = std::getenv("FUSE_UI_SCALE");
            m->resourceDpi = rm ? rm : "";
            m->detectedScale = dpi::x11Scale(m->resourceDpi, gdk ? gdk : "", own ? own : "");
            if (const char* gtk = std::getenv("GTK_THEME")) // cheap hint; systemTheme() asks gsettings
                if (const ColorScheme cs = schemeFromGtkTheme(gtk); cs != ColorScheme::Unknown) app.setSystemTheme({.scheme = cs, .highContrast = highContrastFromGtkTheme(gtk)});
            m->readXSettings(true); // a settings manager's DPI / theme / cursor theme win over the above
        } else {
            m->error = "cannot open X display";
        }
    } else {
        m->error = disp && *disp ? "libX11.so.6 not found" : "no DISPLAY";
    }
    if (m->dpy) app.setEventSource({m.get(), &Impl::pump});
}
bool NativePlatform::available() const { return m->wl || m->dpy != nullptr; }
void* NativePlatform::nativeDisplay() const {
    FUSE_APP_WL(nativeDisplay());
    return m->dpy;
}
NativeBackend NativePlatform::backend() const { return m->wl ? NativeBackend::Wayland : m->dpy ? NativeBackend::X11 : NativeBackend::Auto; }
#endif

NativePlatform::~NativePlatform() {
#if !defined(_WIN32)
    if (m->wl) { // detaches its windows, closes the connection, restores the event source
        m->wl.reset();
        return;
    }
#endif
    // detach every shown window (restores its headless presenter) before the display / instance go away
    while (!m->recs.empty()) m->recs.back()->win->setPresenter({});
    detail::vkCtxDestroy(m->vk);
#if defined(_WIN32)
    m->app.setEventSource({});
    if (m->oleReady) OleUninitialize();
#else
    if (m->dpy) {
        if (m->helper) m->x.DestroyWindow(m->dpy, m->helper);
        m->x.CloseDisplay(m->dpy);
        m->app.setEventSource({});
    }
#endif
}

PresenterKind NativePlatform::show(AppWindow& w) {
    FUSE_APP_WL(show(w));
    if (const Impl::Rec* existing = m->find(w)) return existing->kind;
    if (!available()) return PresenterKind::Headless;
    auto* r = new Impl::Rec{};
    r->impl = m.get();
    r->win = &w;
    m->recs.push_back(r);
    if (!m->createOsWindow(*r)) {
        std::erase(m->recs, r);
        delete r;
        return PresenterKind::Headless;
    }
    if (detail::VkCtx* c = m->vkContext()) {
        SurfaceSource s;
#if defined(_WIN32)
        s.kind = SurfaceSource::Win32;
        s.hinstance = m->hinst;
        s.hwnd = reinterpret_cast<void*>(r->handle);
#else
        s.kind = SurfaceSource::Xlib;
        s.display = m->dpy;
        s.xwindow = static_cast<unsigned long>(r->handle);
#endif
        r->vk = detail::vkWinCreate(c, s, m->opts, m->error);
    }
#if defined(_WIN32)
    r->kind = r->vk ? PresenterKind::Vulkan : PresenterKind::Software;
#else
    // without Vulkan the X server shows the CPU raster when the visual is plain TrueColor; else the window stays empty
    r->kind = r->vk ? PresenterKind::Vulkan
              : m->x.DefaultDepth(m->dpy, m->x.DefaultScreen(m->dpy)) >= 24 ? PresenterKind::Software : PresenterKind::Headless;
#endif
    if (m->opts.osPopups) w.ui().setOsPopups(true); // screen bounds arrive with the first present
    if (m->opts.osWindows) w.ui().setOsWindows(true); // floating docks ("os-window" nodes) get their own OS windows
    if (m->clipboardUsable()) w.ui().setClipboard({&Impl::clipSet, &Impl::clipGet, m.get()});
    // a FIFO swapchain waits for the display; everything else is paced by AppWindow::setMaxFrameRate when asked
    w.setPresenter({r, &Impl::present, &Impl::destroy, r->vk != nullptr && m->opts.vsync});
    w.setWindowBackend({r, &Impl::windowRequest});
    {
        int ox = 0, oy = 0;
        Rect work;
        if (m->screenInfo(*r, ox, oy, work)) {
            r->originX = ox;
            r->originY = oy;
            w.notifyMoved(Impl::toUi(*r, f32(ox), f32(oy))); // where the OS put it
        }
    }
    // what the app asked for before the window existed
    if (w.minimumSize().w > 0.f || w.minimumSize().h > 0.f || w.maximumSize().w > 0.f || w.maximumSize().h > 0.f)
        Impl::windowRequest(r, w, WindowRequest::SizeLimits);
    if (w.requestedMode() != WindowMode::Normal) Impl::windowRequest(r, w, WindowRequest::Mode);
    if (!w.requestedVisible()) Impl::windowRequest(r, w, WindowRequest::Visible);
    return r->kind;
}

PresenterKind NativePlatform::kind(const AppWindow& w) const {
    FUSE_APP_WL(kind(w));
    const Impl::Rec* r = m->find(w);
    return r ? r->kind : PresenterKind::Headless;
}
std::string_view NativePlatform::gpuName() const {
    FUSE_APP_WL(gpuName());
    return detail::vkCtxDeviceName(m->vk);
}
bool NativePlatform::gpuDraws(const AppWindow& w) const {
    FUSE_APP_WL(gpuDraws(w));
    const Impl::Rec* r = m->find(w);
    return r && detail::vkWinGpuDraws(r->vk);
}
std::string_view NativePlatform::lastError() const {
    FUSE_APP_WL(lastError());
    return m->error;
}
bool NativePlatform::setExternalTexture(const AppWindow& w, unsigned slot, const ExternalTexture& tex) {
    FUSE_APP_WL(setExternalTexture(w, slot, tex));
    const Impl::Rec* r = m->find(w);
    return r && detail::vkWinSetExternal(r->vk, slot, tex);
}
bool NativePlatform::capture(const AppWindow& w, Image& out) {
    FUSE_APP_WL(capture(w, out));
    const Impl::Rec* r = m->find(w);
    return r && r->handle && m->captureOs(*r, out);
}
std::size_t NativePlatform::popupCount(const AppWindow& w) const {
    FUSE_APP_WL(popupCount(w));
    const Impl::Rec* r = m->find(w);
    return r ? r->popups.size() : 0;
}
unsigned long long NativePlatform::popupWindow(const AppWindow& w, std::size_t i) const {
    FUSE_APP_WL(popupWindow(w, i));
    const Impl::Rec* r = m->find(w);
    return r && i < r->popups.size() ? r->popups[i].handle : 0;
}
Rect NativePlatform::popupScreenRect(const AppWindow& w, std::size_t i) const {
    FUSE_APP_WL(popupScreenRect(w, i));
    const Impl::Rec* r = m->find(w);
    return r && i < r->popups.size() ? r->popups[i].screen : Rect{};
}
bool NativePlatform::capturePopup(const AppWindow& w, std::size_t i, Image& out) {
    FUSE_APP_WL(capturePopup(w, i, out));
    const Impl::Rec* r = m->find(w);
    return r && i < r->popups.size() && m->capturePopupOs(r->popups[i], out);
}
Vec2 NativePlatform::screenOrigin(const AppWindow& w) const {
    FUSE_APP_WL_NONE(Vec2{}); // Wayland has no global coordinates
    const Impl::Rec* r = m->find(w);
    int ox = 0, oy = 0;
    Rect work;
    if (!r || !r->handle || !m->screenInfo(*r, ox, oy, work)) return {};
    return {f32(ox), f32(oy)};
}
Rect NativePlatform::safeArea(const AppWindow& w) const {
    const Size s = w.size();
    return {0.f, 0.f, s.w, s.h};
}
float NativePlatform::scale(const AppWindow& w) const {
    FUSE_APP_WL(scale(w));
    const Impl::Rec* r = m->find(w);
    return r ? r->scale : 1.f;
}
Cursor NativePlatform::cursor(const AppWindow& w) const {
    FUSE_APP_WL(cursor(w));
    const Impl::Rec* r = m->find(w);
    return r ? r->cursor : Cursor::Arrow;
}
bool NativePlatform::setClipboardText(std::string_view utf8) {
    FUSE_APP_WL(setClipboardText(utf8));
    return m->clipboardUsable() && Impl::clipSet(m.get(), utf8);
}
bool NativePlatform::clipboardText(std::string& out) {
    FUSE_APP_WL(clipboardText(out));
    return m->clipboardUsable() && Impl::clipGet(m.get(), out);
}
bool NativePlatform::postClick(unsigned long long nativeWin, Vec2 at) {
    FUSE_APP_WL(postClick(nativeWin, at));
    return nativeWin && m->postClickOs(nativeWin, at);
}
unsigned long long NativePlatform::nativeWindow(const AppWindow& w) const {
    FUSE_APP_WL(nativeWindow(w));
    const Impl::Rec* r = m->find(w);
    return r ? r->handle : 0;
}

namespace {
// fuse::ui::Image pixels are 0xAABBGGRR (R in the low byte); OS icons want 0xAARRGGBB
u32 toArgb(u32 p) { return (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu); }
} // namespace

#if defined(_WIN32)
namespace {
HICON makeIcon(const Image& img) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = img.width;
    bi.bV5Height = -img.height;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000u;
    bi.bV5GreenMask = 0x0000FF00u;
    bi.bV5BlueMask = 0x000000FFu;
    bi.bV5AlphaMask = 0xFF000000u;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return nullptr;
    u32* dst = static_cast<u32*>(bits);
    for (std::size_t i = 0; i < img.pixels.size(); ++i) dst[i] = toArgb(img.pixels[i]);
    HBITMAP mask = CreateBitmap(img.width, img.height, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}
const Image* closest(std::span<const Image> sizes, int want) {
    const Image* best = nullptr;
    for (const Image& i : sizes)
        if (i.width > 0 && (!best || std::abs(i.width - want) < std::abs(best->width - want))) best = &i;
    return best;
}
int iconWidth(HICON icon) {
    ICONINFO ii{};
    if (!icon || !GetIconInfo(icon, &ii)) return 0;
    BITMAP bm{};
    GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return bm.bmWidth;
}
} // namespace

bool NativePlatform::setIcon(const AppWindow& w, std::span<const Image> sizes) {
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle) return false;
    HWND hwnd = reinterpret_cast<HWND>(r->handle);
    bool ok = false;
    for (const auto& [which, metric] : {std::pair{ICON_BIG, SM_CXICON}, std::pair{ICON_SMALL, SM_CXSMICON}}) {
        const Image* img = closest(sizes, GetSystemMetrics(metric));
        if (!img) continue;
        if (HICON icon = makeIcon(*img)) {
            if (HICON old = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_SETICON, WPARAM(which), LPARAM(icon)))) DestroyIcon(old);
            ok = true;
        }
    }
    return ok;
}
bool NativePlatform::resizeWindow(const AppWindow& w, unsigned width, unsigned height) {
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle) return false;
    const Size px = dpi::toPixels({f32(width), f32(height)}, r->scale);
    RECT rc{0, 0, LONG(px.w), LONG(px.h)};
    chromeAdjust(w, rc);
    return SetWindowPos(reinterpret_cast<HWND>(r->handle), nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != 0;
}
std::vector<int> NativePlatform::iconSizes(const AppWindow& w) const {
    std::vector<int> out;
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle) return out;
    HWND hwnd = reinterpret_cast<HWND>(r->handle);
    for (WPARAM which : {WPARAM(ICON_BIG), WPARAM(ICON_SMALL)})
        if (const int wpx = iconWidth(reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, which, 0)))) out.push_back(wpx);
    return out;
}
#else
bool NativePlatform::setIcon(const AppWindow& w, std::span<const Image> sizes) {
    FUSE_APP_WL(setIcon(w, sizes));
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle || !m->dpy) return false;
    std::vector<unsigned long> data; // format-32 X properties are arrays of C long
    for (const Image& img : sizes) {
        if (img.width <= 0 || img.height <= 0 || img.pixels.size() != std::size_t(img.width) * std::size_t(img.height)) continue;
        data.push_back(static_cast<unsigned long>(img.width));
        data.push_back(static_cast<unsigned long>(img.height));
        for (u32 p : img.pixels) data.push_back(toArgb(p));
    }
    if (data.empty()) return false;
    const x11::Atom netIcon = m->x.InternAtom(m->dpy, "_NET_WM_ICON", 0);
    const x11::Atom cardinal = 6; // XA_CARDINAL
    m->x.ChangeProperty(m->dpy, x11::Window(r->handle), netIcon, cardinal, 32, 0 /*PropModeReplace*/,
                        reinterpret_cast<const unsigned char*>(data.data()), int(data.size()));
    m->x.Flush(m->dpy);
    return true;
}
bool NativePlatform::resizeWindow(const AppWindow& w, unsigned width, unsigned height) {
    FUSE_APP_WL(resizeWindow(w, width, height));
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle || !m->dpy) return false;
    const Size px = dpi::toPixels({f32(width), f32(height)}, r->scale);
    m->x.ResizeWindow(m->dpy, x11::Window(r->handle), unsigned(px.w), unsigned(px.h));
    m->x.Flush(m->dpy);
    return true;
}
std::vector<int> NativePlatform::iconSizes(const AppWindow& w) const {
    FUSE_APP_WL(iconSizes(w));
    std::vector<int> out;
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle || !m->dpy) return out;
    const x11::Atom netIcon = m->x.InternAtom(m->dpy, "_NET_WM_ICON", 0);
    x11::Atom type = 0;
    int format = 0;
    unsigned long n = 0, after = 0;
    unsigned char* prop = nullptr;
    m->x.Sync(m->dpy, 0);
    if (m->x.GetWindowProperty(m->dpy, x11::Window(r->handle), netIcon, 0, 1L << 22, 0, 6, &type, &format, &n, &after, &prop) != 0 ||
        !prop)
        return out;
    const unsigned long* v = reinterpret_cast<const unsigned long*>(prop);
    for (unsigned long i = 0; i + 2 <= n;) {
        const unsigned long iw = v[i], ih = v[i + 1];
        if (!iw || !ih || i + 2 + iw * ih > n) break;
        out.push_back(int(iw));
        i += 2 + iw * ih;
    }
    m->x.Free(prop);
    return out;
}
#endif

} // namespace fuse::app

#include "native_services.inl" // opens namespace fuse::app itself
#include "native_platform.inl" // tool windows, clipboard formats, OS drag and drop, custom chrome, window-less dialogs
