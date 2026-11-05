// fuse_app Wayland backend: see wayland_internal.hpp. Everything Wayland is reached through dlopen'd entry points
// and the hand-written protocol tables below (the wire format only needs names, signatures and opcodes).
#include "wayland_internal.hpp"

#if !defined(_WIN32) && !defined(__ANDROID__)
#include "fuse/app/chrome.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <poll.h>

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

namespace fuse::app::wl {
using namespace fuse::ui;

// ---- libwayland-client ABI (wayland-util.h / wayland-client-core.h) -------------------------------------------------
struct wl_interface;
struct wl_message {
    const char* name;
    const char* signature;
    const wl_interface** types;
};
struct wl_interface {
    const char* name;
    int version;
    int method_count;
    const wl_message* methods;
    int event_count;
    const wl_message* events;
};
struct wl_array {
    std::size_t size, alloc;
    void* data;
};
struct wl_proxy;
struct wl_display;
using Fn = void (*)(void);

namespace {
struct Lib {
    void* so = nullptr;
    wl_display* (*display_connect)(const char*) = nullptr;
    void (*display_disconnect)(wl_display*) = nullptr;
    int (*display_get_fd)(wl_display*) = nullptr;
    int (*display_dispatch_pending)(wl_display*) = nullptr;
    int (*display_roundtrip)(wl_display*) = nullptr;
    int (*display_flush)(wl_display*) = nullptr;
    int (*display_prepare_read)(wl_display*) = nullptr;
    int (*display_read_events)(wl_display*) = nullptr;
    void (*display_cancel_read)(wl_display*) = nullptr;
    int (*display_get_error)(wl_display*) = nullptr;
    wl_proxy* (*proxy_marshal_constructor)(wl_proxy*, uint32_t, const wl_interface*, ...) = nullptr;
    wl_proxy* (*proxy_marshal_constructor_versioned)(wl_proxy*, uint32_t, const wl_interface*, uint32_t, ...) = nullptr;
    void (*proxy_marshal)(wl_proxy*, uint32_t, ...) = nullptr;
    int (*proxy_add_listener)(wl_proxy*, Fn*, void*) = nullptr;
    void (*proxy_destroy)(wl_proxy*) = nullptr;
    uint32_t (*proxy_get_version)(wl_proxy*) = nullptr;
    // core protocol interfaces (data symbols of libwayland-client)
    const wl_interface *registry = nullptr, *compositor = nullptr, *surface = nullptr, *shm = nullptr, *shmPool = nullptr,
                       *buffer = nullptr, *seat = nullptr, *pointer = nullptr, *keyboard = nullptr, *output = nullptr,
                       *callback = nullptr;
    bool load(std::string& err) {
        if (so) return true;
        so = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!so) {
            err = "libwayland-client.so.0 not found";
            return false;
        }
        bool all = true;
        auto get = [&](auto& f, const char* n) {
            f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(so, n));
            all = all && f;
        };
        get(display_connect, "wl_display_connect");
        get(display_disconnect, "wl_display_disconnect");
        get(display_get_fd, "wl_display_get_fd");
        get(display_dispatch_pending, "wl_display_dispatch_pending");
        get(display_roundtrip, "wl_display_roundtrip");
        get(display_flush, "wl_display_flush");
        get(display_prepare_read, "wl_display_prepare_read");
        get(display_read_events, "wl_display_read_events");
        get(display_cancel_read, "wl_display_cancel_read");
        get(display_get_error, "wl_display_get_error");
        get(proxy_marshal_constructor, "wl_proxy_marshal_constructor");
        get(proxy_marshal_constructor_versioned, "wl_proxy_marshal_constructor_versioned");
        get(proxy_marshal, "wl_proxy_marshal");
        get(proxy_add_listener, "wl_proxy_add_listener");
        get(proxy_destroy, "wl_proxy_destroy");
        get(proxy_get_version, "wl_proxy_get_version");
        auto data = [&](const wl_interface*& i, const char* n) {
            i = static_cast<const wl_interface*>(dlsym(so, n));
            all = all && i;
        };
        data(registry, "wl_registry_interface");
        data(compositor, "wl_compositor_interface");
        data(surface, "wl_surface_interface");
        data(shm, "wl_shm_interface");
        data(shmPool, "wl_shm_pool_interface");
        data(buffer, "wl_buffer_interface");
        data(seat, "wl_seat_interface");
        data(pointer, "wl_pointer_interface");
        data(keyboard, "wl_keyboard_interface");
        data(output, "wl_output_interface");
        data(callback, "wl_callback_interface");
        if (!all) err = "libwayland-client.so.0 lacks an entry point";
        return all;
    }
};
Lib L;

// ---- xkbcommon (optional: without it a US evdev table maps keys, no text) -----------------------------------------
struct xkb_context;
struct xkb_keymap;
struct xkb_state;
struct RuleNames { const char *rules, *model, *layout, *variant, *options; };
struct Xkb {
    void* so = nullptr;
    xkb_context* (*context_new)(int) = nullptr;
    void (*context_unref)(xkb_context*) = nullptr;
    xkb_keymap* (*keymap_new_from_string)(xkb_context*, const char*, int, int) = nullptr;
    xkb_keymap* (*keymap_new_from_names)(xkb_context*, const RuleNames*, int) = nullptr;
    void (*keymap_unref)(xkb_keymap*) = nullptr;
    int (*keymap_key_repeats)(xkb_keymap*, uint32_t) = nullptr;
    xkb_state* (*state_new)(xkb_keymap*) = nullptr;
    void (*state_unref)(xkb_state*) = nullptr;
    int (*state_update_mask)(xkb_state*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) = nullptr;
    int (*state_update_key)(xkb_state*, uint32_t, int) = nullptr;
    uint32_t (*state_key_get_one_sym)(xkb_state*, uint32_t) = nullptr;
    int (*state_key_get_utf8)(xkb_state*, uint32_t, char*, std::size_t) = nullptr;
    int (*state_mod_name_is_active)(xkb_state*, const char*, int) = nullptr;
    bool tried = false, ok = false;
    bool load() {
        if (tried) return ok;
        tried = true;
        so = dlopen("libxkbcommon.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!so) return false;
        bool all = true;
        auto get = [&](auto& f, const char* n) {
            f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(so, n));
            all = all && f;
        };
        get(context_new, "xkb_context_new");
        get(context_unref, "xkb_context_unref");
        get(keymap_new_from_string, "xkb_keymap_new_from_string");
        get(keymap_new_from_names, "xkb_keymap_new_from_names");
        get(keymap_unref, "xkb_keymap_unref");
        get(keymap_key_repeats, "xkb_keymap_key_repeats");
        get(state_new, "xkb_state_new");
        get(state_unref, "xkb_state_unref");
        get(state_update_mask, "xkb_state_update_mask");
        get(state_update_key, "xkb_state_update_key");
        get(state_key_get_one_sym, "xkb_state_key_get_one_sym");
        get(state_key_get_utf8, "xkb_state_key_get_utf8");
        get(state_mod_name_is_active, "xkb_state_mod_name_is_active");
        ok = all;
        return ok;
    }
};
Xkb X;
constexpr int kXkbStateModsEffective = 8;

// ---- libwayland-cursor (optional: the compositor's default cursor without it) --------------------------------------
struct wl_cursor_theme;
struct wl_cursor_image { uint32_t width, height, hotspot_x, hotspot_y, delay; };
struct wl_cursor { unsigned image_count; wl_cursor_image** images; char* name; };
struct Cur {
    void* so = nullptr;
    wl_cursor_theme* (*theme_load)(const char*, int, wl_proxy*) = nullptr;
    void (*theme_destroy)(wl_cursor_theme*) = nullptr;
    wl_cursor* (*theme_get_cursor)(wl_cursor_theme*, const char*) = nullptr;
    wl_proxy* (*image_get_buffer)(wl_cursor_image*) = nullptr;
    bool tried = false, ok = false;
    bool load() {
        if (tried) return ok;
        tried = true;
        so = dlopen("libwayland-cursor.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!so) return false;
        bool all = true;
        auto get = [&](auto& f, const char* n) {
            f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(so, n));
            all = all && f;
        };
        get(theme_load, "wl_cursor_theme_load");
        get(theme_destroy, "wl_cursor_theme_destroy");
        get(theme_get_cursor, "wl_cursor_theme_get_cursor");
        get(image_get_buffer, "wl_cursor_image_get_buffer");
        ok = all;
        return ok;
    }
};
Cur C;

// ---- xdg-shell (stable, version 1 messages) and xdg-decoration-unstable-v1 tables ----------------------------------
extern const wl_interface xdgWmBase, xdgPositioner, xdgSurface, xdgToplevel, xdgPopup, decoManager, decoToplevel;
const wl_interface* tNone[8] = {};
const wl_interface* tCreatePositioner[] = {&xdgPositioner};
const wl_interface* tGetXdgSurface[] = {&xdgSurface, nullptr /* wl_surface, set at load */};
const wl_interface* tGetToplevel[] = {&xdgToplevel};
const wl_interface* tGetPopup[] = {&xdgPopup, &xdgSurface, &xdgPositioner};
const wl_interface* tSetParent[] = {&xdgToplevel};
const wl_interface* tSeatFirst[] = {nullptr /* wl_seat */, nullptr, nullptr, nullptr};
const wl_interface* tOutput[] = {nullptr /* wl_output */};
const wl_interface* tGetDeco[] = {&decoToplevel, &xdgToplevel};

const wl_message wmBaseRequests[] = {
    {"destroy", "", tNone}, {"create_positioner", "n", tCreatePositioner}, {"get_xdg_surface", "no", tGetXdgSurface}, {"pong", "u", tNone}};
const wl_message wmBaseEvents[] = {{"ping", "u", tNone}};
const wl_message positionerRequests[] = {
    {"destroy", "", tNone}, {"set_size", "ii", tNone}, {"set_anchor_rect", "iiii", tNone}, {"set_anchor", "u", tNone},
    {"set_gravity", "u", tNone}, {"set_constraint_adjustment", "u", tNone}, {"set_offset", "ii", tNone}};
const wl_message xdgSurfaceRequests[] = {
    {"destroy", "", tNone}, {"get_toplevel", "n", tGetToplevel}, {"get_popup", "n?oo", tGetPopup},
    {"set_window_geometry", "iiii", tNone}, {"ack_configure", "u", tNone}};
const wl_message xdgSurfaceEvents[] = {{"configure", "u", tNone}};
const wl_message toplevelRequests[] = {
    {"destroy", "", tNone}, {"set_parent", "?o", tSetParent}, {"set_title", "s", tNone}, {"set_app_id", "s", tNone},
    {"show_window_menu", "ouii", tSeatFirst}, {"move", "ou", tSeatFirst}, {"resize", "ouu", tSeatFirst},
    {"set_max_size", "ii", tNone}, {"set_min_size", "ii", tNone}, {"set_maximized", "", tNone}, {"unset_maximized", "", tNone},
    {"set_fullscreen", "?o", tOutput}, {"unset_fullscreen", "", tNone}, {"set_minimized", "", tNone}};
const wl_message toplevelEvents[] = {{"configure", "iia", tNone}, {"close", "", tNone}};
const wl_message popupRequests[] = {{"destroy", "", tNone}, {"grab", "ou", tSeatFirst}};
const wl_message popupEvents[] = {{"configure", "iiii", tNone}, {"popup_done", "", tNone}};
const wl_message decoManagerRequests[] = {{"destroy", "", tNone}, {"get_toplevel_decoration", "no", tGetDeco}};
const wl_message decoToplevelRequests[] = {{"destroy", "", tNone}, {"set_mode", "u", tNone}, {"unset_mode", "", tNone}};
const wl_message decoToplevelEvents[] = {{"configure", "u", tNone}};

template <std::size_t N>
constexpr int count(const wl_message (&)[N]) { return int(N); }
const wl_interface xdgWmBase{"xdg_wm_base", 1, count(wmBaseRequests), wmBaseRequests, count(wmBaseEvents), wmBaseEvents};
const wl_interface xdgPositioner{"xdg_positioner", 1, count(positionerRequests), positionerRequests, 0, nullptr};
const wl_interface xdgSurface{"xdg_surface", 1, count(xdgSurfaceRequests), xdgSurfaceRequests, count(xdgSurfaceEvents), xdgSurfaceEvents};
const wl_interface xdgToplevel{"xdg_toplevel", 1, count(toplevelRequests), toplevelRequests, count(toplevelEvents), toplevelEvents};
const wl_interface xdgPopup{"xdg_popup", 1, count(popupRequests), popupRequests, count(popupEvents), popupEvents};
const wl_interface decoManager{"zxdg_decoration_manager_v1", 1, count(decoManagerRequests), decoManagerRequests, 0, nullptr};
const wl_interface decoToplevel{"zxdg_toplevel_decoration_v1", 1, count(decoToplevelRequests), decoToplevelRequests,
                                          count(decoToplevelEvents), decoToplevelEvents};
void fillProtocolTypes() {
    tGetXdgSurface[1] = L.surface;
    tSeatFirst[0] = L.seat;
    tOutput[0] = L.output;
}

// opcodes
enum : uint32_t {
    kDisplayGetRegistry = 1, kRegistryBind = 0, kCompositorCreateSurface = 0,
    kSurfaceDestroy = 0, kSurfaceAttach = 1, kSurfaceDamage = 2, kSurfaceCommit = 6, kSurfaceSetBufferScale = 8, kSurfaceDamageBuffer = 9,
    kShmCreatePool = 0, kPoolCreateBuffer = 0, kPoolDestroy = 1, kBufferDestroy = 0,
    kSeatGetPointer = 0, kSeatGetKeyboard = 1, kPointerSetCursor = 0, kPointerRelease = 1, kKeyboardRelease = 0,
    kWmBaseDestroy = 0, kWmBaseGetXdgSurface = 2, kWmBasePong = 3,
    kXdgSurfaceDestroy = 0, kXdgSurfaceGetToplevel = 1, kXdgSurfaceAck = 4,
    kToplevelDestroy = 0, kToplevelSetTitle = 2, kToplevelSetAppId = 3, kToplevelMove = 5, kToplevelResize = 6,
    kToplevelSetMinSize = 8, kToplevelSetMaximized = 9, kToplevelUnsetMaximized = 10, kToplevelSetMinimized = 13,
    kDecoManagerDestroy = 0, kDecoManagerGet = 1, kDecoDestroy = 0, kDecoSetMode = 1,
};
constexpr uint32_t kShmXrgb8888 = 1;
constexpr uint32_t kBtnLeft = 0x110, kBtnRight = 0x111, kBtnMiddle = 0x112;

template <class F>
Fn fn(F f) { return reinterpret_cast<Fn>(f); }
void destroyRequest(wl_proxy* p, uint32_t op) {
    if (!p) return;
    L.proxy_marshal(p, op);
    L.proxy_destroy(p);
}
f32 fixedToFloat(int32_t v) { return f32(v) / 256.f; }

// US layout evdev -> X keysym (no xkbcommon)
unsigned long evdevSym(unsigned code, bool shift) {
    static const char* rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const unsigned first[] = {2, 16, 30, 44};
    for (int r = 0; r < 4; ++r) {
        const unsigned n = unsigned(std::strlen(rows[r]));
        if (code >= first[r] && code < first[r] + n) {
            const char c = rows[r][code - first[r]];
            return shift && c >= 'a' && c <= 'z' ? unsigned(c - 32) : unsigned(c);
        }
    }
    switch (code) {
    case 1: return 0xff1b; case 14: return 0xff08; case 15: return 0xff09; case 28: return 0xff0d; case 57: return ' ';
    case 26: return '['; case 27: return ']'; case 102: return 0xff50; case 103: return 0xff52; case 104: return 0xff55;
    case 105: return 0xff51; case 106: return 0xff53; case 107: return 0xff57; case 108: return 0xff54; case 109: return 0xff56;
    case 110: return 0xff63; case 111: return 0xffff; case 56: return 0xffe9; case 100: return 0xffea;
    case 87: return 0xffc8; case 88: return 0xffc9;
    default: break;
    }
    if (code >= 59 && code <= 68) return 0xffbe + (code - 59); // F1..F10
    return 0;
}
} // namespace

// ---- backend state ----------------------------------------------------------------------------------------------------
struct Output {
    wl_proxy* proxy = nullptr;
    uint32_t name = 0;
    int x = 0, y = 0, w = 0, h = 0, refresh = 0, scale = 1;
    std::string make, model;
};
struct ShmBuf {
    wl_proxy* buffer = nullptr;
    void* data = nullptr;
    std::size_t bytes = 0;
    int w = 0, h = 0;
    bool busy = false;
};
struct Window {
    Backend* b = nullptr;
    AppWindow* win = nullptr;
    bool* focused = nullptr;
    wl_proxy *surface = nullptr, *xdgSurf = nullptr, *toplevel = nullptr, *deco = nullptr;
    bool configured = false;
    int pendingW = 0, pendingH = 0;
    bool pendingMax = false;
    unsigned long long acks = 0;
    int scale = 1, committedScale = 1;
    std::vector<Output*> entered;
    ShmBuf bufs[3];
    int last = -1;
    CursorShape cursor = CursorShape::Count;
    std::chrono::steady_clock::time_point captionPress{};
};
struct Backend {
    wl_display* dpy = nullptr;
    wl_proxy *registry = nullptr, *compositor = nullptr, *shm = nullptr, *seat = nullptr, *wmBase = nullptr, *decoMgr = nullptr;
    uint32_t compositorVersion = 1, seatName = 0;
    std::vector<Output*> outputs;
    wl_proxy *pointer = nullptr, *keyboard = nullptr;
    std::vector<Window*> windows;
    Window *ptrFocus = nullptr, *kbdFocus = nullptr;
    uint32_t ptrSerial = 0, enterSerial = 0;
    Vec2 ptrPos{};
    bool dpiAware = true;
    int envScale = 0; ///< $FUSE_UI_SCALE rounded (0: follow the outputs)
    KeysymMap keys = nullptr;
    xkb_context* xctx = nullptr;
    xkb_keymap* keymap = nullptr;
    xkb_state* xstate = nullptr;
    u8 mods = 0;
    bool shiftRaw = false; // no-xkb fallback
    int32_t repeatRate = 25, repeatDelay = 600;
    uint32_t repeatKey = 0;
    std::chrono::steady_clock::time_point repeatAt{};
    wl_proxy* cursorSurface = nullptr;
    wl_cursor_theme* theme = nullptr;
    int themeScale = 0;
    bool readPrepared = false;
};

namespace {
Window* windowOf(Backend* b, wl_proxy* surface) {
    for (Window* w : b->windows)
        if (w->surface == surface) return w;
    return nullptr;
}
int outputScale(const Window& w) {
    int s = 0;
    for (const Output* o : w.entered) s = std::max(s, o->scale);
    if (s == 0)
        for (const Output* o : w.b->outputs) s = std::max(s, o->scale); // not on an output yet: the largest known
    return std::max(1, s);
}
void applyScale(Window& w, int s) {
    s = std::clamp(s, 1, 8);
    if (s == w.scale && f32(s) == w.win->scale()) return;
    w.scale = s;
    w.win->setScale(f32(s)); // keeps the logical size; the next buffer is logical * s pixels
    w.win->requestRedraw();
}
void rescale(Window& w) {
    if (!w.b->dpiAware) return;
    applyScale(w, w.b->envScale > 0 ? w.b->envScale : outputScale(w));
}
void sendBufferScale(Window& w) {
    if (w.committedScale == w.scale || w.b->compositorVersion < 3) return;
    L.proxy_marshal(w.surface, kSurfaceSetBufferScale, int32_t(w.scale));
    w.committedScale = w.scale;
}

// ---- listeners ----
void registryGlobal(void* data, wl_proxy* reg, uint32_t name, const char* iface, uint32_t version);
void registryRemove(void* data, wl_proxy*, uint32_t name);
void outputGeometry(void* data, wl_proxy*, int32_t x, int32_t y, int32_t, int32_t, int32_t, const char* make, const char* model, int32_t) {
    auto* o = static_cast<Output*>(data);
    o->x = x;
    o->y = y;
    o->make = make ? make : "";
    o->model = model ? model : "";
}
void outputMode(void* data, wl_proxy*, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
    auto* o = static_cast<Output*>(data);
    if (flags & 1u) { // current
        o->w = w;
        o->h = h;
        o->refresh = refresh;
    }
}
Backend* gBackend = nullptr; // output events reach windows through it (one backend per process)
void outputDone(void*, wl_proxy*) {
    if (gBackend)
        for (Window* w : gBackend->windows) rescale(*w);
}
void outputScaleEv(void* data, wl_proxy*, int32_t factor) { static_cast<Output*>(data)->scale = std::max(1, int(factor)); }
void outputName(void*, wl_proxy*, const char*) {}
const Fn* outputListener() {
    static const Fn l[] = {fn(&outputGeometry), fn(&outputMode), fn(&outputDone), fn(&outputScaleEv), fn(&outputName), fn(&outputName)};
    return l;
}

void bufferRelease(void* data, wl_proxy*) { static_cast<ShmBuf*>(data)->busy = false; }
const Fn* bufferListener() {
    static const Fn l[] = {fn(&bufferRelease)};
    return l;
}
void shmFormat(void*, wl_proxy*, uint32_t) {}
void pingEv(void*, wl_proxy* base, uint32_t serial) { L.proxy_marshal(base, kWmBasePong, serial); }

// surface enter / leave: output scale
void surfaceEnter(void* data, wl_proxy*, wl_proxy* output) {
    auto* w = static_cast<Window*>(data);
    for (Output* o : w->b->outputs)
        if (o->proxy == output && std::find(w->entered.begin(), w->entered.end(), o) == w->entered.end()) w->entered.push_back(o);
    rescale(*w);
}
void surfaceLeave(void* data, wl_proxy*, wl_proxy* output) {
    auto* w = static_cast<Window*>(data);
    std::erase_if(w->entered, [&](const Output* o) { return o->proxy == output; });
    rescale(*w);
}
void surfaceInt(void*, wl_proxy*, int32_t) {}
void surfaceUint(void*, wl_proxy*, uint32_t) {}

void xdgSurfaceConfigure(void* data, wl_proxy* xs, uint32_t serial) {
    auto* w = static_cast<Window*>(data);
    L.proxy_marshal(xs, kXdgSurfaceAck, serial);
    ++w->acks;
    AppWindow& a = *w->win;
    if (w->pendingW > 0 && w->pendingH > 0) {
        const Size s = a.size();
        if (u32(w->pendingW) != u32(s.w) || u32(w->pendingH) != u32(s.h)) a.resize(u32(w->pendingW), u32(w->pendingH));
    }
    if (a.chrome().maximized != w->pendingMax) a.chrome().maximized = w->pendingMax;
    w->configured = true;
    a.requestRedraw();
}
void toplevelConfigure(void* data, wl_proxy*, int32_t width, int32_t height, wl_array* states) {
    auto* w = static_cast<Window*>(data);
    w->pendingW = width;
    w->pendingH = height;
    w->pendingMax = false;
    if (states)
        for (std::size_t i = 0; i + 4 <= states->size; i += 4) {
            uint32_t s = 0;
            std::memcpy(&s, static_cast<const char*>(states->data) + i, 4);
            if (s == 1) w->pendingMax = true; // XDG_TOPLEVEL_STATE_MAXIMIZED
        }
}
void toplevelClose(void* data, wl_proxy*) { static_cast<Window*>(data)->win->requestClose(); }
void decoConfigure(void*, wl_proxy*, uint32_t) {}

// ---- pointer ----
Vec2 localPos(int32_t sx, int32_t sy) { return {fixedToFloat(sx), fixedToFloat(sy)}; } // surface-local = UI units
void applyCursor(Backend* b, Window* w);
void pointerEnter(void* data, wl_proxy*, uint32_t serial, wl_proxy* surface, int32_t sx, int32_t sy) {
    auto* b = static_cast<Backend*>(data);
    b->ptrFocus = windowOf(b, surface);
    b->enterSerial = serial;
    b->ptrPos = localPos(sx, sy);
    if (Window* w = b->ptrFocus) {
        w->cursor = CursorShape::Count; // the compositor forgets ours on leave
        applyCursor(b, w);
        w->win->ui().setModifiers(b->mods);
        w->win->ui().pointerMove(b->ptrPos);
    }
}
void pointerLeave(void* data, wl_proxy*, uint32_t, wl_proxy*) { static_cast<Backend*>(data)->ptrFocus = nullptr; }
void pointerMotion(void* data, wl_proxy*, uint32_t, int32_t sx, int32_t sy) {
    auto* b = static_cast<Backend*>(data);
    b->ptrPos = localPos(sx, sy);
    if (Window* w = b->ptrFocus) {
        w->win->ui().setModifiers(b->mods);
        w->win->ui().pointerMove(b->ptrPos);
        applyCursor(b, w);
    }
}
uint32_t resizeEdge(HitArea a) { // xdg_toplevel.resize_edge
    switch (a) {
    case HitArea::Top: return 1;
    case HitArea::Bottom: return 2;
    case HitArea::Left: return 4;
    case HitArea::TopLeft: return 5;
    case HitArea::BottomLeft: return 6;
    case HitArea::Right: return 8;
    case HitArea::TopRight: return 9;
    case HitArea::BottomRight: return 10;
    default: return 0;
    }
}
void deliverButton(Backend* b, Window* w, uint32_t serial, uint32_t button, bool down) {
    AppWindow& a = *w->win;
    UiContext& ui = a.ui();
    ui.setModifiers(b->mods);
    const Vec2 pos = b->ptrPos;
    if (down && button == kBtnLeft && a.chrome().frame != WindowFrame::System && w->toplevel && b->seat) {
        const HitArea h = chromeHitTest(a, pos);
        if (h == HitArea::Caption) {
            const auto now = std::chrono::steady_clock::now();
            if (now - w->captionPress < std::chrono::milliseconds(400)) { // double click toggles maximise
                w->captionPress = {};
                a.command(WindowCommand::ToggleMaximize);
                return;
            }
            w->captionPress = now;
            L.proxy_marshal(w->toplevel, kToplevelMove, b->seat, serial);
            return;
        }
        if (const uint32_t e = resizeEdge(h); e && a.chrome().frame == WindowFrame::Custom) {
            L.proxy_marshal(w->toplevel, kToplevelResize, b->seat, serial, e);
            return;
        }
    }
    const MouseButton mb = button == kBtnRight ? MouseButton::Right : button == kBtnMiddle ? MouseButton::Middle : MouseButton::Left;
    if (down) ui.pointerDown(pos, mb);
    else ui.pointerUp(pos, mb);
    applyCursor(b, w);
}
void pointerButton(void* data, wl_proxy*, uint32_t serial, uint32_t, uint32_t button, uint32_t state) {
    auto* b = static_cast<Backend*>(data);
    b->ptrSerial = serial;
    if (Window* w = b->ptrFocus) deliverButton(b, w, serial, button, state == 1);
}
void pointerAxis(void* data, wl_proxy*, uint32_t, uint32_t axis, int32_t value) {
    auto* b = static_cast<Backend*>(data);
    Window* w = b->ptrFocus;
    if (!w) return;
    const f32 v = fixedToFloat(value) / 10.f; // ~10 per notch; positive = down / right
    w->win->ui().wheel(b->ptrPos, axis == 0 ? Vec2{0.f, -v} : Vec2{v, 0.f});
}
void pointerFrame(void*, wl_proxy*) {}
void pointerAxisSource(void*, wl_proxy*, uint32_t) {}
void pointerAxisStop(void*, wl_proxy*, uint32_t, uint32_t) {}
void pointerAxisDiscrete(void*, wl_proxy*, uint32_t, int32_t) {}
const Fn* pointerListener() {
    static const Fn l[] = {fn(&pointerEnter), fn(&pointerLeave), fn(&pointerMotion), fn(&pointerButton), fn(&pointerAxis),
                           fn(&pointerFrame), fn(&pointerAxisSource), fn(&pointerAxisStop), fn(&pointerAxisDiscrete)};
    return l;
}

// ---- keyboard ----
void keyboardKeymap(void* data, wl_proxy*, uint32_t format, int32_t fd, uint32_t size) {
    auto* b = static_cast<Backend*>(data);
    if (format != 1 /*xkb_v1*/ || !X.load()) {
        ::close(fd);
        return;
    }
    void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) return;
    if (!b->xctx) b->xctx = X.context_new(0);
    xkb_keymap* km = b->xctx ? X.keymap_new_from_string(b->xctx, static_cast<const char*>(map), 1, 0) : nullptr;
    ::munmap(map, size);
    if (!km) return;
    if (b->xstate) X.state_unref(b->xstate);
    if (b->keymap) X.keymap_unref(b->keymap);
    b->keymap = km;
    b->xstate = X.state_new(km);
}
void keyboardEnter(void* data, wl_proxy*, uint32_t, wl_proxy* surface, wl_array*) {
    auto* b = static_cast<Backend*>(data);
    b->kbdFocus = windowOf(b, surface);
    if (b->kbdFocus && b->kbdFocus->focused) *b->kbdFocus->focused = true;
}
void keyboardLeave(void* data, wl_proxy*, uint32_t, wl_proxy*) {
    auto* b = static_cast<Backend*>(data);
    if (b->kbdFocus && b->kbdFocus->focused) *b->kbdFocus->focused = false;
    b->kbdFocus = nullptr;
    b->repeatKey = 0;
}
void deliverKey(Backend* b, Window* w, uint32_t key, bool down, [[maybe_unused]] bool repeat) {
    UiContext& ui = w->win->ui();
    unsigned long sym = 0;
    char text[32] = {};
    if (b->xstate) {
        sym = X.state_key_get_one_sym(b->xstate, key + 8);
        if (down) X.state_key_get_utf8(b->xstate, key + 8, text, sizeof text);
    } else {
        if (key == 42 || key == 54) b->shiftRaw = down;
        sym = evdevSym(key, b->shiftRaw);
        if (down && sym >= 0x20 && sym < 0x7f) text[0] = char(sym);
    }
    bool ascii = false;
    const Key k = b->keys ? b->keys(sym, ascii) : Key::Other;
    if (k != Key::Other) {
        if (down) ui.keyDown(k, b->mods);
        else ui.keyUp(k, b->mods);
    }
    if (down && !(b->mods & (kModCtrl | kModAlt))) {
        std::string s(text);
        std::erase_if(s, [](char ch) { return u8(ch) < 0x20 || u8(ch) == 0x7F; });
        if (!s.empty()) ui.textInput(s);
    }
}
void keyboardKey(void* data, wl_proxy*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    auto* b = static_cast<Backend*>(data);
    Window* w = b->kbdFocus;
    if (!w) return;
    const bool down = state == 1;
    deliverKey(b, w, key, down, false);
    if (down && b->repeatRate > 0 && (!b->keymap || X.keymap_key_repeats(b->keymap, key + 8))) {
        b->repeatKey = key;
        b->repeatAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(b->repeatDelay);
    } else if (!down && key == b->repeatKey) {
        b->repeatKey = 0;
    }
}
void keyboardModifiers(void* data, wl_proxy*, uint32_t, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
    auto* b = static_cast<Backend*>(data);
    if (!b->xstate) return;
    X.state_update_mask(b->xstate, dep, lat, lock, 0, 0, group);
    auto on = [&](const char* n) { return X.state_mod_name_is_active(b->xstate, n, kXkbStateModsEffective) > 0; };
    b->mods = u8((on("Shift") ? kModShift : 0) | (on("Control") ? kModCtrl : 0) | (on("Mod1") ? kModAlt : 0) | (on("Mod4") ? kModSuper : 0));
}
void keyboardRepeat(void* data, wl_proxy*, int32_t rate, int32_t delay) {
    auto* b = static_cast<Backend*>(data);
    b->repeatRate = rate;
    b->repeatDelay = delay;
}
const Fn* keyboardListener() {
    static const Fn l[] = {fn(&keyboardKeymap), fn(&keyboardEnter), fn(&keyboardLeave), fn(&keyboardKey), fn(&keyboardModifiers),
                           fn(&keyboardRepeat)};
    return l;
}

// ---- seat / registry ----
void seatCaps(void* data, wl_proxy* seat, uint32_t caps) {
    auto* b = static_cast<Backend*>(data);
    if ((caps & 1u) && !b->pointer) {
        b->pointer = L.proxy_marshal_constructor(seat, kSeatGetPointer, L.pointer, nullptr);
        if (b->pointer) L.proxy_add_listener(b->pointer, const_cast<Fn*>(pointerListener()), b);
    } else if (!(caps & 1u) && b->pointer) {
        L.proxy_destroy(b->pointer);
        b->pointer = nullptr;
        b->ptrFocus = nullptr;
    }
    if ((caps & 2u) && !b->keyboard) {
        b->keyboard = L.proxy_marshal_constructor(seat, kSeatGetKeyboard, L.keyboard, nullptr);
        if (b->keyboard) L.proxy_add_listener(b->keyboard, const_cast<Fn*>(keyboardListener()), b);
    } else if (!(caps & 2u) && b->keyboard) {
        L.proxy_destroy(b->keyboard);
        b->keyboard = nullptr;
        b->kbdFocus = nullptr;
    }
}
void seatName(void*, wl_proxy*, const char*) {}
const Fn* seatListener() {
    static const Fn l[] = {fn(&seatCaps), fn(&seatName)};
    return l;
}
wl_proxy* bindGlobal(wl_proxy* reg, uint32_t name, const wl_interface* iface, uint32_t version) {
    return L.proxy_marshal_constructor_versioned(reg, kRegistryBind, iface, version, name, iface->name, version, nullptr);
}
void registryGlobal(void* data, wl_proxy* reg, uint32_t name, const char* iface, uint32_t version) {
    auto* b = static_cast<Backend*>(data);
    const std::string_view n(iface);
    if (n == "wl_compositor" && !b->compositor) {
        b->compositorVersion = std::min(version, 4u);
        b->compositor = bindGlobal(reg, name, L.compositor, b->compositorVersion);
    } else if (n == "wl_shm" && !b->shm) {
        b->shm = bindGlobal(reg, name, L.shm, 1);
        static const Fn l[] = {fn(&shmFormat)};
        L.proxy_add_listener(b->shm, const_cast<Fn*>(l), b);
    } else if (n == "xdg_wm_base" && !b->wmBase) {
        b->wmBase = bindGlobal(reg, name, &xdgWmBase, 1);
        static const Fn l[] = {fn(&pingEv)};
        L.proxy_add_listener(b->wmBase, const_cast<Fn*>(l), b);
    } else if (n == "zxdg_decoration_manager_v1" && !b->decoMgr) {
        b->decoMgr = bindGlobal(reg, name, &decoManager, 1);
    } else if (n == "wl_seat" && !b->seat) {
        b->seat = bindGlobal(reg, name, L.seat, std::min(version, 5u));
        b->seatName = name;
        L.proxy_add_listener(b->seat, const_cast<Fn*>(seatListener()), b);
    } else if (n == "wl_output") {
        auto* o = new Output{};
        o->name = name;
        o->proxy = bindGlobal(reg, name, L.output, std::min(version, 2u));
        L.proxy_add_listener(o->proxy, const_cast<Fn*>(outputListener()), o);
        b->outputs.push_back(o);
    }
}
void registryRemove(void* data, wl_proxy*, uint32_t name) {
    auto* b = static_cast<Backend*>(data);
    for (std::size_t i = 0; i < b->outputs.size(); ++i)
        if (b->outputs[i]->name == name) {
            Output* o = b->outputs[i];
            for (Window* w : b->windows) std::erase(w->entered, o);
            L.proxy_destroy(o->proxy);
            delete o;
            b->outputs.erase(b->outputs.begin() + std::ptrdiff_t(i));
            for (Window* w : b->windows) rescale(*w);
            return;
        }
}
const Fn* registryListener() {
    static const Fn l[] = {fn(&registryGlobal), fn(&registryRemove)};
    return l;
}

// ---- cursor ----
void applyCursor(Backend* b, Window* w) {
    if (!b->pointer || b->ptrFocus != w) return;
    CursorShape c = w->win->ui().cursor();
    if (c == CursorShape::Auto) c = CursorShape::Arrow;
    if (w->win->chrome().frame == WindowFrame::Custom) switch (chromeHitTest(*w->win, b->ptrPos)) {
        case HitArea::Left: case HitArea::Right: c = CursorShape::ResizeEW; break;
        case HitArea::Top: case HitArea::Bottom: c = CursorShape::ResizeNS; break;
        case HitArea::TopLeft: case HitArea::BottomRight: c = CursorShape::ResizeNWSE; break;
        case HitArea::TopRight: case HitArea::BottomLeft: c = CursorShape::ResizeNESW; break;
        default: break;
        }
    if (c == w->cursor) return;
    w->cursor = c;
    if (!C.load() || !b->shm) return;
    const int scale = w->scale;
    if (!b->theme || b->themeScale != scale) {
        if (b->theme) C.theme_destroy(b->theme);
        int size = 24;
        if (const char* e = std::getenv("XCURSOR_SIZE"); e && std::atoi(e) > 0) size = std::atoi(e);
        b->theme = C.theme_load(std::getenv("XCURSOR_THEME"), size * scale, b->shm);
        b->themeScale = scale;
    }
    if (!b->theme) return;
    static constexpr const char* kNames[][2] = {{"default", "left_ptr"}, {"default", "left_ptr"}, {"text", "xterm"},
        {"pointer", "hand2"}, {"ns-resize", "sb_v_double_arrow"}, {"ew-resize", "sb_h_double_arrow"},
        {"nwse-resize", "bottom_right_corner"}, {"nesw-resize", "bottom_left_corner"}, {"move", "fleur"},
        {"wait", "watch"}, {"not-allowed", "crossed_circle"}, {"crosshair", "cross"}};
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == std::size_t(CursorShape::Count));
    wl_cursor* cur = nullptr;
    for (int k = 0; !cur && k < 2; ++k) cur = C.theme_get_cursor(b->theme, kNames[std::size_t(c)][k]);
    if (!cur) cur = C.theme_get_cursor(b->theme, "left_ptr");
    if (!cur || !cur->image_count) return;
    wl_cursor_image* img = cur->images[0];
    wl_proxy* buf = C.image_get_buffer(img);
    if (!buf) return;
    if (!b->cursorSurface) b->cursorSurface = L.proxy_marshal_constructor(b->compositor, kCompositorCreateSurface, L.surface, nullptr);
    if (b->compositorVersion >= 3) L.proxy_marshal(b->cursorSurface, kSurfaceSetBufferScale, int32_t(scale));
    L.proxy_marshal(b->cursorSurface, kSurfaceAttach, buf, int32_t(0), int32_t(0));
    L.proxy_marshal(b->cursorSurface, kSurfaceDamage, int32_t(0), int32_t(0), int32_t(img->width), int32_t(img->height));
    L.proxy_marshal(b->cursorSurface, kSurfaceCommit);
    L.proxy_marshal(b->pointer, kPointerSetCursor, b->enterSerial, b->cursorSurface, int32_t(img->hotspot_x / unsigned(scale)),
                    int32_t(img->hotspot_y / unsigned(scale)));
}

void chromeCommand(void* user, AppWindow& a, WindowCommand c) {
    auto* w = static_cast<Window*>(user);
    switch (c) {
    case WindowCommand::Minimize: L.proxy_marshal(w->toplevel, kToplevelSetMinimized); break;
    case WindowCommand::ToggleMaximize:
        L.proxy_marshal(w->toplevel, a.chrome().maximized ? kToplevelUnsetMaximized : kToplevelSetMaximized);
        break;
    case WindowCommand::Close: a.requestClose(); break;
    }
    L.display_flush(w->b->dpy);
}

void freeBuf(ShmBuf& s) {
    if (s.buffer) destroyRequest(s.buffer, kBufferDestroy);
    if (s.data) ::munmap(s.data, s.bytes);
    s = {};
}
bool makeBuf(Backend* b, ShmBuf& s, int w, int h) {
    const std::size_t stride = std::size_t(w) * 4, bytes = stride * std::size_t(h);
    const int fd = ::memfd_create("fuse-wl-shm", MFD_CLOEXEC);
    if (fd < 0) return false;
    if (::ftruncate(fd, off_t(bytes)) != 0) {
        ::close(fd);
        return false;
    }
    void* data = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        ::close(fd);
        return false;
    }
    wl_proxy* pool = L.proxy_marshal_constructor(b->shm, kShmCreatePool, L.shmPool, nullptr, fd, int32_t(bytes));
    wl_proxy* buf = pool ? L.proxy_marshal_constructor(pool, kPoolCreateBuffer, L.buffer, nullptr, int32_t(0), int32_t(w), int32_t(h),
                                                       int32_t(stride), kShmXrgb8888)
                         : nullptr;
    destroyRequest(pool, kPoolDestroy); // the buffer keeps the memory
    ::close(fd);
    if (!buf) {
        ::munmap(data, bytes);
        return false;
    }
    s.buffer = buf;
    s.data = data;
    s.bytes = bytes;
    s.w = w;
    s.h = h;
    s.busy = false;
    L.proxy_add_listener(buf, const_cast<Fn*>(bufferListener()), &s);
    return true;
}
} // namespace

// ---- API -----------------------------------------------------------------------------------------------------------
bool wanted() {
    const char* be = std::getenv("FUSE_APP_BACKEND");
    if (be && std::string_view(be) == "wayland") return true;
    if (be && std::string_view(be) == "x11") return false;
    const char* d = std::getenv("DISPLAY");
    const char* wd = std::getenv("WAYLAND_DISPLAY");
    return !(d && *d) && wd && *wd;
}

Backend* open(std::string& err, bool dpiAware, KeysymMap keys) {
    if (gBackend) {
        err = "one Wayland backend per process";
        return nullptr;
    }
    if (!L.load(err)) return nullptr;
    fillProtocolTypes();
    wl_display* d = L.display_connect(nullptr);
    if (!d) {
        err = "cannot connect to the Wayland compositor";
        return nullptr;
    }
    auto* b = new Backend{};
    b->dpy = d;
    b->dpiAware = dpiAware;
    b->keys = keys;
    if (const char* e = std::getenv("FUSE_UI_SCALE"); e && *e) b->envScale = std::clamp(int(std::lround(std::strtof(e, nullptr))), 0, 8);
    gBackend = b;
    b->registry = L.proxy_marshal_constructor(reinterpret_cast<wl_proxy*>(d), kDisplayGetRegistry, L.registry, nullptr);
    L.proxy_add_listener(b->registry, const_cast<Fn*>(registryListener()), b);
    L.display_roundtrip(d); // globals
    L.display_roundtrip(d); // output modes / scales, seat capabilities
    if (!b->compositor || !b->shm || !b->wmBase) {
        err = !b->wmBase ? "the compositor has no xdg_wm_base" : "the compositor lacks wl_compositor / wl_shm";
        close(b);
        return nullptr;
    }
    return b;
}

void close(Backend* b) {
    if (!b) return;
    while (!b->windows.empty()) destroyWindow(b, b->windows.back());
    if (b->theme) C.theme_destroy(b->theme);
    if (b->cursorSurface) destroyRequest(b->cursorSurface, kSurfaceDestroy);
    if (b->xstate) X.state_unref(b->xstate);
    if (b->keymap) X.keymap_unref(b->keymap);
    if (b->xctx) X.context_unref(b->xctx);
    if (b->pointer) L.proxy_destroy(b->pointer);
    if (b->keyboard) L.proxy_destroy(b->keyboard);
    for (Output* o : b->outputs) {
        L.proxy_destroy(o->proxy);
        delete o;
    }
    if (b->seat) L.proxy_destroy(b->seat);
    if (b->decoMgr) destroyRequest(b->decoMgr, kDecoManagerDestroy);
    if (b->wmBase) destroyRequest(b->wmBase, kWmBaseDestroy);
    if (b->shm) L.proxy_destroy(b->shm);
    if (b->compositor) L.proxy_destroy(b->compositor);
    if (b->registry) L.proxy_destroy(b->registry);
    L.display_flush(b->dpy);
    L.display_disconnect(b->dpy);
    if (gBackend == b) gBackend = nullptr;
    delete b;
}

Backend* current() { return gBackend; }
int fd(const Backend* b) { return L.display_get_fd(b->dpy); }
void* display(const Backend* b) { return b->dpy; }

int beforePoll(Backend* b) {
    while (L.display_prepare_read(b->dpy) != 0) L.display_dispatch_pending(b->dpy);
    b->readPrepared = true;
    L.display_flush(b->dpy);
    return L.display_get_fd(b->dpy);
}
void afterPoll(Backend* b, bool readable) {
    if (b->readPrepared) {
        if (readable) L.display_read_events(b->dpy);
        else L.display_cancel_read(b->dpy);
        b->readPrepared = false;
    }
    L.display_dispatch_pending(b->dpy);
    if (b->repeatKey && b->kbdFocus && b->repeatRate > 0) {
        const auto now = std::chrono::steady_clock::now();
        const auto step = std::chrono::microseconds(1000000 / b->repeatRate);
        for (int n = 0; now >= b->repeatAt && n < 8; ++n) { // a long stall repeats a few times, not hundreds
            deliverKey(b, b->kbdFocus, b->repeatKey, true, true);
            b->repeatAt += step;
        }
        if (now >= b->repeatAt) b->repeatAt = now + step;
    }
    L.display_flush(b->dpy);
}
f32 nextTimeout(const Backend* b) {
    if (!b->repeatKey || !b->kbdFocus) return std::numeric_limits<f32>::infinity();
    const auto d = b->repeatAt - std::chrono::steady_clock::now();
    return std::max(0.f, std::chrono::duration<f32>(d).count());
}
bool roundtrip(Backend* b) { return L.display_roundtrip(b->dpy) >= 0; }

Window* createWindow(Backend* b, AppWindow& a, bool* focused, std::string& err) {
    auto* w = new Window{};
    w->b = b;
    w->win = &a;
    w->focused = focused;
    w->surface = L.proxy_marshal_constructor(b->compositor, kCompositorCreateSurface, L.surface, nullptr);
    if (!w->surface) {
        err = "wl_compositor.create_surface failed";
        delete w;
        return nullptr;
    }
    static const Fn surfaceL[] = {fn(&surfaceEnter), fn(&surfaceLeave), fn(&surfaceInt), fn(&surfaceUint)};
    L.proxy_add_listener(w->surface, const_cast<Fn*>(surfaceL), w);
    w->xdgSurf = L.proxy_marshal_constructor(b->wmBase, kWmBaseGetXdgSurface, &xdgSurface, nullptr, w->surface);
    static const Fn xsL[] = {fn(&xdgSurfaceConfigure)};
    L.proxy_add_listener(w->xdgSurf, const_cast<Fn*>(xsL), w);
    w->toplevel = L.proxy_marshal_constructor(w->xdgSurf, kXdgSurfaceGetToplevel, &xdgToplevel, nullptr);
    static const Fn tL[] = {fn(&toplevelConfigure), fn(&toplevelClose)};
    L.proxy_add_listener(w->toplevel, const_cast<Fn*>(tL), w);
    L.proxy_marshal(w->toplevel, kToplevelSetTitle, a.title().c_str());
    L.proxy_marshal(w->toplevel, kToplevelSetAppId, "fuse");
    L.proxy_marshal(w->toplevel, kToplevelSetMinSize, int32_t(64), int32_t(48));
    WindowChrome& c = a.chrome();
    if (c.frame == WindowFrame::System && b->decoMgr) { // server-side frame where the compositor draws one (KDE, sway)
        w->deco = L.proxy_marshal_constructor(b->decoMgr, kDecoManagerGet, &decoToplevel, nullptr, w->toplevel);
        static const Fn dL[] = {fn(&decoConfigure)};
        L.proxy_add_listener(w->deco, const_cast<Fn*>(dL), w);
        L.proxy_marshal(w->deco, kDecoSetMode, 2u /*server_side*/);
    }
    if (c.frame != WindowFrame::System) {
        c.user = w;
        c.command = &chromeCommand;
    }
    b->windows.push_back(w);
    if (b->dpiAware) applyScale(*w, b->envScale > 0 ? b->envScale : outputScale(*w)); // until the surface enters an output
    else applyScale(*w, 1);
    sendBufferScale(*w);
    L.proxy_marshal(w->surface, kSurfaceCommit); // no buffer: asks for the first configure
    for (int i = 0; i < 50 && !w->configured; ++i)
        if (L.display_roundtrip(b->dpy) < 0) break;
    if (!w->configured) {
        err = "no xdg_surface.configure from the compositor";
        destroyWindow(b, w);
        return nullptr;
    }
    return w;
}

void destroyWindow(Backend* b, Window* w) {
    if (!w) return;
    if (w->win && w->win->chrome().command == &chromeCommand) {
        w->win->chrome().command = nullptr;
        w->win->chrome().user = nullptr;
    }
    if (b->ptrFocus == w) b->ptrFocus = nullptr;
    if (b->kbdFocus == w) {
        b->kbdFocus = nullptr;
        b->repeatKey = 0;
    }
    if (w->deco) destroyRequest(w->deco, kDecoDestroy);
    destroyRequest(w->toplevel, kToplevelDestroy);
    destroyRequest(w->xdgSurf, kXdgSurfaceDestroy);
    destroyRequest(w->surface, kSurfaceDestroy);
    for (ShmBuf& s : w->bufs) freeBuf(s);
    std::erase(b->windows, w);
    L.display_flush(b->dpy);
    delete w;
}

void* surface(const Window* w) { return w ? w->surface : nullptr; }
int bufferScale(const Window* w) { return w ? w->scale : 1; }
unsigned long long serialOf(const Window* w) { return w ? w->acks : 0; }

void presentShm(Backend* b, Window* w, const Image& img) {
    if (!w->configured || img.pixels.empty()) return;
    const int iw = img.width, ih = img.height;
    // a buffer of this size the compositor has released (at most 3; free or wrong-size ones are (re)made)
    auto released = [&] {
        for (int i = 0; i < 3; ++i)
            if (w->bufs[i].buffer && !w->bufs[i].busy && w->bufs[i].w == iw && w->bufs[i].h == ih) return i;
        return -1;
    };
    int pick = released();
    for (int i = 0; i < 3 && pick < 0; ++i)
        if (!w->bufs[i].buffer || !w->bufs[i].busy) {
            freeBuf(w->bufs[i]);
            if (makeBuf(b, w->bufs[i], iw, ih)) pick = i;
            break;
        }
    if (pick < 0) { // all busy: wait for a release
        L.display_roundtrip(b->dpy);
        pick = released();
        if (pick < 0) {
            w->win->requestRedraw();
            return;
        }
    }
    ShmBuf& s = w->bufs[std::size_t(pick)];
    u32* dst = static_cast<u32*>(s.data);
    const std::size_t n = std::size_t(iw) * std::size_t(ih);
    for (std::size_t i = 0; i < n; ++i) { // 0xAABBGGRR -> XRGB8888 0x00RRGGBB
        const u32 q = img.pixels[i];
        dst[i] = 0xFF000000u | ((q & 0xFFu) << 16) | (q & 0xFF00u) | ((q >> 16) & 0xFFu);
    }
    sendBufferScale(*w);
    L.proxy_marshal(w->surface, kSurfaceAttach, s.buffer, int32_t(0), int32_t(0));
    if (b->compositorVersion >= 4) L.proxy_marshal(w->surface, kSurfaceDamageBuffer, int32_t(0), int32_t(0), int32_t(iw), int32_t(ih));
    else L.proxy_marshal(w->surface, kSurfaceDamage, int32_t(0), int32_t(0), int32_t(INT_MAX), int32_t(INT_MAX));
    L.proxy_marshal(w->surface, kSurfaceCommit);
    s.busy = true;
    w->last = pick;
    L.display_flush(b->dpy);
}

void beforeVulkanPresent(Backend*, Window* w) {
    w->last = -1; // the swapchain owns the surface's buffers now
    sendBufferScale(*w);
}

bool capture(const Window* w, Image& out) {
    if (!w || w->last < 0) return false;
    const ShmBuf& s = w->bufs[std::size_t(w->last)];
    out.resize(s.w, s.h);
    const u32* src = static_cast<const u32*>(s.data);
    for (std::size_t i = 0, n = std::size_t(s.w) * std::size_t(s.h); i < n; ++i) {
        const u32 p = src[i];
        out.pixels[i] = 0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
    }
    return true;
}

void setCursor(Backend* b, Window* w, CursorShape) { applyCursor(b, w); }
void setScale(Backend*, Window* w, f32 scale) { applyScale(*w, int(std::lround(scale))); }
void resize(Backend*, Window* w, unsigned width, unsigned height) {
    w->win->resize(std::max(1u, width), std::max(1u, height));
    w->win->requestRedraw();
}

std::vector<DisplayInfo> displays(const Backend* b) {
    std::vector<DisplayInfo> out;
    for (const Output* o : b->outputs) {
        DisplayInfo d;
        d.name = o->make.empty() && o->model.empty() ? "wayland" : o->make + " " + o->model;
        d.bounds = {f32(o->x), f32(o->y), f32(o->w), f32(o->h)};
        d.work = d.bounds;
        d.scale = f32(o->scale);
        d.refreshHz = f32(o->refresh) / 1000.f;
        d.primary = out.empty();
        out.push_back(std::move(d));
    }
    return out;
}

void injectPointer(Backend* b, Window* w, Vec2 at, int kind) {
    b->ptrFocus = w;
    b->ptrPos = {at.x / f32(w->scale), at.y / f32(w->scale)};
    if (kind == 0) {
        w->win->ui().setModifiers(b->mods);
        w->win->ui().pointerMove(b->ptrPos);
    } else {
        deliverButton(b, w, b->ptrSerial, kBtnLeft, kind == 1);
    }
}
void injectKey(Backend* b, Window* w, unsigned code) {
    if (!b->xstate && X.load()) { // no keyboard from the compositor (headless): the default keymap
        if (!b->xctx) b->xctx = X.context_new(0);
        const RuleNames names{"evdev", "pc105", "us", "", ""};
        if (b->xctx && (b->keymap = X.keymap_new_from_names(b->xctx, &names, 0))) b->xstate = X.state_new(b->keymap);
    }
    deliverKey(b, w, code, true, false);
    deliverKey(b, w, code, false, false);
}
void injectClose(Window* w) { toplevelClose(w, w->toplevel); }

std::string selfTest(KeysymMap keys) {
    std::string err;
    if (!L.load(err)) return err;
    fillProtocolTypes();
    // every message: one types entry per argument (wl_closure reads types[i] for each), opcodes as used above
    const wl_interface* all[] = {&xdgWmBase, &xdgPositioner, &xdgSurface, &xdgToplevel, &xdgPopup, &decoManager, &decoToplevel};
    for (const wl_interface* i : all) {
        for (int side = 0; side < 2; ++side) {
            const int n = side ? i->event_count : i->method_count;
            const wl_message* m = side ? i->events : i->methods;
            for (int k = 0; k < n; ++k) {
                int args = 0;
                for (const char* p = m[k].signature; *p; ++p)
                    if (*p != '?' && !(*p >= '0' && *p <= '9')) ++args;
                if (args > 8 || !m[k].types) return std::string(i->name) + "." + m[k].name + ": bad types";
                for (int a = 0; a < args; ++a) {
                    // object / new_id arguments of requests we send must name their interface
                    int idx = 0;
                    char t = 0;
                    for (const char* p = m[k].signature; *p; ++p) {
                        if (*p == '?' || (*p >= '0' && *p <= '9')) continue;
                        if (idx++ == a) { t = *p; break; }
                    }
                    if ((t == 'n' || t == 'o') && !side && !m[k].types[a]) return std::string(i->name) + "." + m[k].name + ": untyped object";
                }
            }
        }
    }
    struct Op { const wl_interface* i; uint32_t op; const char* name; };
    const Op ops[] = {{&xdgWmBase, kWmBaseGetXdgSurface, "get_xdg_surface"}, {&xdgWmBase, kWmBasePong, "pong"},
                      {&xdgSurface, kXdgSurfaceGetToplevel, "get_toplevel"}, {&xdgSurface, kXdgSurfaceAck, "ack_configure"},
                      {&xdgToplevel, kToplevelSetTitle, "set_title"}, {&xdgToplevel, kToplevelMove, "move"},
                      {&xdgToplevel, kToplevelResize, "resize"}, {&xdgToplevel, kToplevelSetMaximized, "set_maximized"},
                      {&xdgToplevel, kToplevelUnsetMaximized, "unset_maximized"}, {&xdgToplevel, kToplevelSetMinimized, "set_minimized"},
                      {&xdgToplevel, kToplevelSetMinSize, "set_min_size"}, {&decoManager, kDecoManagerGet, "get_toplevel_decoration"},
                      {&decoToplevel, kDecoSetMode, "set_mode"}};
    for (const Op& o : ops)
        if (int(o.op) >= o.i->method_count || std::string_view(o.i->methods[o.op].name) != o.name)
            return std::string("opcode of ") + o.i->name + "." + o.name;
    // core interfaces from libwayland-client: the opcodes used here
    struct Core { const wl_interface* i; uint32_t op; const char* name; };
    const Core core[] = {{L.surface, kSurfaceAttach, "attach"}, {L.surface, kSurfaceCommit, "commit"},
                         {L.surface, kSurfaceSetBufferScale, "set_buffer_scale"}, {L.surface, kSurfaceDamageBuffer, "damage_buffer"},
                         {L.shm, kShmCreatePool, "create_pool"}, {L.shmPool, kPoolCreateBuffer, "create_buffer"},
                         {L.shmPool, kPoolDestroy, "destroy"}, {L.seat, kSeatGetPointer, "get_pointer"},
                         {L.seat, kSeatGetKeyboard, "get_keyboard"}, {L.pointer, kPointerSetCursor, "set_cursor"},
                         {L.registry, kRegistryBind, "bind"}, {L.compositor, kCompositorCreateSurface, "create_surface"}};
    for (const Core& c : core)
        if (int(c.op) >= c.i->method_count || std::string_view(c.i->methods[c.op].name) != c.name)
            return std::string("core opcode ") + c.i->name + "." + c.name;
    // keys: xkbcommon's default US keymap (evdev codes) and the no-xkb fallback both reach the same FUSE keys
    struct K { unsigned code; Key key; const char* text; };
    const K ks[] = {{30, Key::A, "a"}, {28, Key::Enter, ""}, {105, Key::Left, ""}, {2, Key::Num1, "1"}, {57, Key::Space, " "}, {1, Key::Escape, ""}};
    for (int pass = 0; pass < 2; ++pass) {
        xkb_context* ctx = nullptr;
        xkb_keymap* km = nullptr;
        xkb_state* st = nullptr;
        if (pass == 0) {
            if (!X.load()) continue; // optional
            ctx = X.context_new(0);
            const RuleNames names{"evdev", "pc105", "us", "", ""};
            km = ctx ? X.keymap_new_from_names(ctx, &names, 0) : nullptr;
            st = km ? X.state_new(km) : nullptr;
            if (!st) return "xkbcommon could not compile the default keymap";
        }
        for (const K& k : ks) {
            unsigned long sym = 0;
            char text[16] = {};
            if (st) {
                sym = X.state_key_get_one_sym(st, k.code + 8);
                X.state_key_get_utf8(st, k.code + 8, text, sizeof text);
            } else {
                sym = evdevSym(k.code, false);
                if (sym >= 0x20 && sym < 0x7f) text[0] = char(sym);
            }
            bool ascii = false;
            if (keys(sym, ascii) != k.key) return std::string(pass ? "fallback" : "xkb") + " key " + std::to_string(k.code);
            if (*k.text && std::string_view(text) != k.text) return std::string(pass ? "fallback" : "xkb") + " text " + std::to_string(k.code);
        }
        if (st) X.state_unref(st);
        if (km) X.keymap_unref(km);
        if (ctx) X.context_unref(ctx);
    }
    return {};
}

} // namespace fuse::app::wl
#endif
