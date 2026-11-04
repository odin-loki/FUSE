// fuse_app X11 backend: the libX11 / libXrandr entry points and Xlib structs used, loaded at runtime (dlopen), so
// neither X11 headers nor the libraries are needed to build. Struct layouts follow the LP64 Xlib ABI. Shared with the
// X11 gates (they act as window manager / XSETTINGS manager on their own connection).
#pragma once

#if !defined(_WIN32)
#include <dlfcn.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace fuse::app::x11 {
using Display = struct _XDisplay;
using XID = unsigned long;
using Window = XID;
using Atom = unsigned long;
using KeySym = XID;
using Time = unsigned long;

struct AnyEv { int type; unsigned long serial; int send_event; Display* display; Window window; };
struct InputEv {
    int type; unsigned long serial; int send_event; Display* display; Window window, root, subwindow;
    Time time; int x, y, x_root, y_root; unsigned state; unsigned detail; // keycode / button
    int same_screen;
};
struct ConfigureEv { int type; unsigned long serial; int send_event; Display* display; Window event, window; int x, y, width, height; };
struct ClientEv { int type; unsigned long serial; int send_event; Display* display; Window window; Atom message_type; int format; long l[5]; };
struct PropertyEv { int type; unsigned long serial; int send_event; Display* display; Window window; Atom atom; Time time; int state; };
struct DestroyEv { int type; unsigned long serial; int send_event; Display* display; Window event, window; };
struct SelectionRequestEv {
    int type; unsigned long serial; int send_event; Display* display; Window owner, requestor; Atom selection, target, property; Time time;
};
struct SelectionEv { int type; unsigned long serial; int send_event; Display* display; Window requestor; Atom selection, target, property; Time time; };
struct SelectionClearEv { int type; unsigned long serial; int send_event; Display* display; Window window; Atom selection; Time time; };
// XEvent is a union padded to 24 longs.
union Event {
    int type; AnyEv any; InputEv input; ConfigureEv configure; ClientEv client; PropertyEv property; DestroyEv destroy;
    SelectionRequestEv selectionRequest; SelectionEv selection; SelectionClearEv selectionClear; long pad[24];
};
struct Image;
struct ImageFuncs { Image* (*create)(); int (*destroy)(Image*); };
struct Image {
    int width, height, xoffset, format; char* data; int byte_order, bitmap_unit, bitmap_bit_order, bitmap_pad, depth,
        bytes_per_line, bits_per_pixel; unsigned long red_mask, green_mask, blue_mask; char* obdata; ImageFuncs f;
};
// XSetWindowAttributes
struct SetWindowAttributes {
    unsigned long background_pixmap, background_pixel, border_pixmap, border_pixel;
    int bit_gravity, win_gravity, backing_store;
    unsigned long backing_planes, backing_pixel;
    int save_under;
    long event_mask, do_not_propagate_mask;
    int override_redirect;
    unsigned long colormap, cursor;
};
enum {
    KeyPress = 2, KeyRelease = 3, ButtonPress = 4, ButtonRelease = 5, MotionNotify = 6, FocusIn = 9, FocusOut = 10, Expose = 12,
    DestroyNotify = 17, UnmapNotify = 18, MapNotify = 19, ConfigureNotify = 22, PropertyNotify = 28, SelectionClear = 29,
    SelectionRequest = 30, SelectionNotify = 31, ClientMessage = 33, MappingNotify = 34
};
constexpr int kPropertyNewValue = 0, kPropertyDelete = 1;
// event masks
constexpr long kKeyPressMask = 1, kKeyReleaseMask = 2, kButtonPressMask = 4, kButtonReleaseMask = 8, kPointerMotionMask = 1 << 6,
               kExposureMask = 1 << 15, kStructureNotifyMask = 1 << 17, kSubstructureNotifyMask = 1 << 19,
               kSubstructureRedirectMask = 1 << 20, kFocusChangeMask = 1 << 21, kPropertyChangeMask = 1 << 22;
constexpr unsigned long kCWBackingStore = 1ul << 6, kCWOverrideRedirect = 1ul << 9, kCWSaveUnder = 1ul << 10;
constexpr Atom kXA_ATOM = 4, kXA_CARDINAL = 6, kXA_STRING = 31, kXA_WINDOW = 33;
constexpr int kPropModeReplace = 0;

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
    char* (*GetAtomName)(Display*, Atom) = nullptr;
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
    int (*DeleteProperty)(Display*, Window, Atom) = nullptr;
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
    XID (*CreateFontCursor)(Display*, unsigned) = nullptr;
    int (*DefineCursor)(Display*, Window, XID) = nullptr;
    char* (*ResourceManagerString)(Display*) = nullptr;
    Window (*GetSelectionOwner)(Display*, Atom) = nullptr;
    int (*SetSelectionOwner)(Display*, Atom, Window, Time) = nullptr;
    int (*SetInputFocus)(Display*, Window, int, Time) = nullptr;
    int (*ConvertSelection)(Display*, Atom, Atom, Atom, Window, Time) = nullptr;
    long (*MaxRequestSize)(Display*) = nullptr;
    int (*RefreshKeyboardMapping)(void* mappingEvent) = nullptr;
    int (*LookupKeysym)(InputEv*, int) = nullptr;
    // XIM (input methods): optional, see loadXim()
    char* (*SetLocaleModifiers)(const char*) = nullptr;
    int (*SupportsLocale)() = nullptr;
    void* (*OpenIM)(Display*, void*, char*, char*) = nullptr;
    int (*CloseIM)(void*) = nullptr;
    char* (*GetIMValues)(void*, ...) = nullptr;
    void* (*CreateIC)(void*, ...) = nullptr;
    void (*DestroyIC)(void*) = nullptr;
    char* (*SetICValues)(void*, ...) = nullptr;
    char* (*GetICValues)(void*, ...) = nullptr;
    void (*SetICFocus)(void*) = nullptr;
    void (*UnsetICFocus)(void*) = nullptr;
    void* (*VaCreateNestedList)(int, ...) = nullptr;
    int (*FilterEvent)(Event*, Window) = nullptr;
    int (*Utf8LookupString)(void*, InputEv*, char*, int, KeySym*, int*) = nullptr;
    int (*FreeCursor)(Display*, XID) = nullptr;
    int (*WarpPointer)(Display*, Window, Window, int, int, unsigned, unsigned, int, int) = nullptr;

    bool load() {
        if (so) return true;
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
        get(InternAtom, "XInternAtom"); get(GetAtomName, "XGetAtomName"); get(SetWMProtocols, "XSetWMProtocols");
        get(Pending, "XPending"); get(NextEvent, "XNextEvent"); get(ConnectionNumber, "XConnectionNumber"); get(Flush, "XFlush");
        get(Sync, "XSync"); get(LookupString, "XLookupString"); get(GetImage, "XGetImage"); get(SetErrorHandler, "XSetErrorHandler");
        get(ChangeProperty, "XChangeProperty"); get(DeleteProperty, "XDeleteProperty"); get(GetWindowProperty, "XGetWindowProperty");
        get(Free, "XFree"); get(ResizeWindow, "XResizeWindow"); get(CreateImage, "XCreateImage"); get(PutImage, "XPutImage");
        get(DefaultGC, "XDefaultGC"); get(DefaultVisual, "XDefaultVisual"); get(DefaultDepth, "XDefaultDepth");
        get(MoveResizeWindow, "XMoveResizeWindow"); get(TranslateCoordinates, "XTranslateCoordinates");
        get(ChangeWindowAttributes, "XChangeWindowAttributes"); get(DisplayWidth, "XDisplayWidth");
        get(DisplayHeight, "XDisplayHeight"); get(SendEvent, "XSendEvent"); get(GrabPointer, "XGrabPointer");
        get(UngrabPointer, "XUngrabPointer"); get(CreateFontCursor, "XCreateFontCursor"); get(DefineCursor, "XDefineCursor");
        get(ResourceManagerString, "XResourceManagerString"); get(GetSelectionOwner, "XGetSelectionOwner");
        get(SetSelectionOwner, "XSetSelectionOwner"); get(SetInputFocus, "XSetInputFocus");
        get(ConvertSelection, "XConvertSelection"); get(MaxRequestSize, "XMaxRequestSize");
        get(RefreshKeyboardMapping, "XRefreshKeyboardMapping"); get(LookupKeysym, "XLookupKeysym");
        get(FreeCursor, "XFreeCursor"); get(WarpPointer, "XWarpPointer");
        return all;
    }
    /// The XIM entry points (input methods); false when libX11 lacks any of them.
    bool loadXim() {
        if (!so) return false;
        bool all = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(so, name));
            all = all && fn;
        };
        get(SetLocaleModifiers, "XSetLocaleModifiers"); get(SupportsLocale, "XSupportsLocale"); get(OpenIM, "XOpenIM");
        get(CloseIM, "XCloseIM"); get(GetIMValues, "XGetIMValues"); get(CreateIC, "XCreateIC"); get(DestroyIC, "XDestroyIC");
        get(SetICValues, "XSetICValues"); get(GetICValues, "XGetICValues"); get(SetICFocus, "XSetICFocus"); get(UnsetICFocus, "XUnsetICFocus");
        get(VaCreateNestedList, "XVaCreateNestedList"); get(FilterEvent, "XFilterEvent");
        get(Utf8LookupString, "Xutf8LookupString");
        return all;
    }

    /// A whole property (format 8 / 32; format-32 items are C longs). False when missing.
    bool property(Display* d, Window w, Atom prop, Atom type, std::vector<unsigned char>& out, int* formatOut = nullptr,
                  Atom* typeOut = nullptr) const {
        Atom t = 0;
        int format = 0;
        unsigned long n = 0, after = 0;
        unsigned char* p = nullptr;
        if (GetWindowProperty(d, w, prop, 0, 1L << 24, 0, type, &t, &format, &n, &after, &p) != 0 || !p || !t) {
            if (p) Free(p);
            return false;
        }
        const std::size_t item = format == 32 ? sizeof(long) : format == 16 ? sizeof(short) : 1;
        out.assign(p, p + n * item);
        Free(p);
        if (formatOut) *formatOut = format;
        if (typeOut) *typeOut = t;
        return true;
    }
    /// ClientMessage (format 32) to `target` with SubstructureRedirect | SubstructureNotify (EWMH requests to the root).
    bool clientMessage(Display* d, Window target, Window about, Atom type, long l0, long l1 = 0, long l2 = 0, long l3 = 0,
                       long l4 = 0) const {
        Event ev{};
        ev.client.type = ClientMessage;
        ev.client.display = d;
        ev.client.window = about;
        ev.client.message_type = type;
        ev.client.format = 32;
        ev.client.l[0] = l0; ev.client.l[1] = l1; ev.client.l[2] = l2; ev.client.l[3] = l3; ev.client.l[4] = l4;
        return SendEvent(d, target, 0, kSubstructureRedirectMask | kSubstructureNotifyMask, &ev) != 0;
    }
};

// libXrandr (RandR 1.5 monitors + 1.2 CRTC modes for the refresh rate)
struct MonitorInfo { Atom name; int primary, automatic, noutput; int x, y, width, height, mwidth, mheight; XID* outputs; };
struct ModeInfo {
    XID id; unsigned width, height; unsigned long dotClock; unsigned hSyncStart, hSyncEnd, hTotal, hSkew, vSyncStart, vSyncEnd,
        vTotal; char* name; unsigned nameLength; unsigned long modeFlags;
};
struct ScreenResources { Time timestamp, configTimestamp; int ncrtc; XID* crtcs; int noutput; XID* outputs; int nmode; ModeInfo* modes; };
struct OutputInfo {
    Time timestamp; XID crtc; char* name; int nameLen; unsigned long mm_width, mm_height; unsigned short connection, subpixel;
    int ncrtc; XID* crtcs; int nclone; XID* clones; int nmode, npreferred; XID* modes;
};
struct CrtcInfo {
    Time timestamp; int x, y; unsigned width, height; XID mode; unsigned short rotation; int noutput; XID* outputs;
    unsigned short rotations; int npossible; XID* possible;
};
static_assert(sizeof(void*) != 8 || (sizeof(MonitorInfo) == 56 && sizeof(ModeInfo) == 80 && sizeof(ScreenResources) == 64 &&
                                     sizeof(OutputInfo) == 96 && sizeof(CrtcInfo) == 64 && sizeof(Event) == 192));

struct Randr {
    void* so = nullptr;
    int (*QueryExtension)(Display*, int*, int*) = nullptr;
    MonitorInfo* (*GetMonitors)(Display*, Window, int, int*) = nullptr;
    void (*FreeMonitors)(MonitorInfo*) = nullptr;
    ScreenResources* (*GetScreenResourcesCurrent)(Display*, Window) = nullptr;
    void (*FreeScreenResources)(ScreenResources*) = nullptr;
    OutputInfo* (*GetOutputInfo)(Display*, ScreenResources*, XID) = nullptr;
    void (*FreeOutputInfo)(OutputInfo*) = nullptr;
    CrtcInfo* (*GetCrtcInfo)(Display*, ScreenResources*, XID) = nullptr;
    void (*FreeCrtcInfo)(CrtcInfo*) = nullptr;
    // gates: user-defined monitors
    MonitorInfo* (*AllocateMonitor)(Display*, int) = nullptr;
    void (*SetMonitor)(Display*, Window, MonitorInfo*) = nullptr;
    void (*DeleteMonitor)(Display*, Window, Atom) = nullptr;

    bool load() {
        if (so) return true;
        so = dlopen("libXrandr.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!so) return false;
        bool all = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(so, name));
            all = all && fn;
        };
        get(QueryExtension, "XRRQueryExtension"); get(GetMonitors, "XRRGetMonitors"); get(FreeMonitors, "XRRFreeMonitors");
        get(GetScreenResourcesCurrent, "XRRGetScreenResourcesCurrent"); get(FreeScreenResources, "XRRFreeScreenResources");
        get(GetOutputInfo, "XRRGetOutputInfo"); get(FreeOutputInfo, "XRRFreeOutputInfo"); get(GetCrtcInfo, "XRRGetCrtcInfo");
        get(FreeCrtcInfo, "XRRFreeCrtcInfo"); get(AllocateMonitor, "XRRAllocateMonitor"); get(SetMonitor, "XRRSetMonitor");
        get(DeleteMonitor, "XRRDeleteMonitor");
        if (!all) {
            dlclose(so);
            so = nullptr;
        }
        return all;
    }
};

// libXcursor: themed cursors at a chosen size (XSETTINGS Gtk/CursorThemeName / Gtk/CursorThemeSize)
struct Xcursor {
    void* so = nullptr;
    XID (*LibraryLoadCursor)(Display*, const char*) = nullptr;
    int (*SetDefaultSize)(Display*, int) = nullptr;
    int (*GetDefaultSize)(Display*) = nullptr;
    int (*SetTheme)(Display*, const char*) = nullptr;

    bool load() {
        if (so) return true;
        so = dlopen("libXcursor.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!so) return false;
        LibraryLoadCursor = reinterpret_cast<decltype(LibraryLoadCursor)>(dlsym(so, "XcursorLibraryLoadCursor"));
        SetDefaultSize = reinterpret_cast<decltype(SetDefaultSize)>(dlsym(so, "XcursorSetDefaultSize"));
        GetDefaultSize = reinterpret_cast<decltype(GetDefaultSize)>(dlsym(so, "XcursorGetDefaultSize"));
        SetTheme = reinterpret_cast<decltype(SetTheme)>(dlsym(so, "XcursorSetTheme"));
        if (!LibraryLoadCursor || !SetDefaultSize || !GetDefaultSize || !SetTheme) {
            dlclose(so);
            so = nullptr;
            return false;
        }
        return true;
    }
};

// XSETTINGS (freedesktop spec): the _XSETTINGS_SETTINGS property of the _XSETTINGS_S<screen> selection owner.
struct XSetting {
    std::string name;
    int type = 0; ///< 0 integer, 1 string, 2 colour
    int integer = 0;
    std::string string;
};
inline std::vector<XSetting> parseXSettings(const unsigned char* p, std::size_t n) {
    std::vector<XSetting> out;
    if (n < 12) return out;
    const bool msb = p[0] == 1;
    auto u16 = [&](std::size_t at) { return msb ? unsigned(p[at] << 8 | p[at + 1]) : unsigned(p[at] | p[at + 1] << 8); };
    auto u32 = [&](std::size_t at) {
        return msb ? std::uint32_t(p[at]) << 24 | std::uint32_t(p[at + 1]) << 16 | std::uint32_t(p[at + 2]) << 8 | p[at + 3]
                   : std::uint32_t(p[at]) | std::uint32_t(p[at + 1]) << 8 | std::uint32_t(p[at + 2]) << 16 | std::uint32_t(p[at + 3]) << 24;
    };
    auto pad4 = [](std::size_t v) { return (v + 3) & ~std::size_t(3); };
    const std::uint32_t count = u32(8);
    std::size_t at = 12;
    for (std::uint32_t i = 0; i < count && at + 4 <= n; ++i) {
        XSetting s;
        s.type = p[at];
        const std::size_t nameLen = u16(at + 2);
        at += 4;
        if (at + pad4(nameLen) + 4 > n) break;
        s.name.assign(reinterpret_cast<const char*>(p + at), nameLen);
        at += pad4(nameLen) + 4; // name, last-change serial
        if (s.type == 0) {
            if (at + 4 > n) break;
            s.integer = int(u32(at));
            at += 4;
        } else if (s.type == 1) {
            if (at + 4 > n) break;
            const std::size_t len = u32(at);
            at += 4;
            if (at + pad4(len) > n) break;
            s.string.assign(reinterpret_cast<const char*>(p + at), len);
            at += pad4(len);
        } else if (s.type == 2) {
            at += 8;
        } else {
            break;
        }
        out.push_back(std::move(s));
    }
    return out;
}
/// Serialises settings (LSB first): what an XSETTINGS manager writes (gates use it).
inline std::vector<unsigned char> buildXSettings(const std::vector<XSetting>& in, std::uint32_t serial) {
    std::vector<unsigned char> b;
    auto put32 = [&](std::uint32_t v) { for (int k = 0; k < 4; ++k) b.push_back((unsigned char)(v >> (8 * k))); };
    auto pad = [&] { while (b.size() % 4) b.push_back(0); };
    b = {0, 0, 0, 0};
    put32(serial);
    put32(std::uint32_t(in.size()));
    for (const XSetting& s : in) {
        b.push_back((unsigned char)s.type);
        b.push_back(0);
        b.push_back((unsigned char)(s.name.size() & 0xFF));
        b.push_back((unsigned char)(s.name.size() >> 8));
        b.insert(b.end(), s.name.begin(), s.name.end());
        pad();
        put32(serial);
        if (s.type == 0) put32(std::uint32_t(s.integer));
        else {
            put32(std::uint32_t(s.string.size()));
            b.insert(b.end(), s.string.begin(), s.string.end());
            pad();
        }
    }
    return b;
}
} // namespace fuse::app::x11
#endif
