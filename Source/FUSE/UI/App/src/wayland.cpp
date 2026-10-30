// Wayland backend: see fuse/app/wayland.hpp. libwayland-client and libxkbcommon are loaded through fuse::ui::os
// (FORGE); shared-memory buffers, keymaps and data transfer use the seam's fd helpers.
#include "fuse/app/wayland.hpp"
#include "wl_protocol.hpp"
#include "native_internal.hpp"

#include "fuse/ui/os.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <poll.h>
#endif

namespace fuse::app {
using namespace fuse::ui;
namespace detail { Key keyFromKeysym(unsigned long sym, bool& ascii); }

std::string_view preferredWindowSystem() {
#if defined(_WIN32)
    return "win32";
#elif defined(__APPLE__)
    return "cocoa";
#else
    const std::string forced = os::env("FUSE_UI_BACKEND");
    if (forced == "x11") return "x11";
    if (forced == "wayland" || !os::env("WAYLAND_DISPLAY").empty()) return "wayland";
    return "x11";
#endif
}

#if defined(_WIN32) || defined(__APPLE__)
struct WaylandPlatform::Impl {};
WaylandPlatform::WaylandPlatform(Application&, const char*, const NativeOptions&) : m(std::make_unique<Impl>()) {}
WaylandPlatform::~WaylandPlatform() = default;
bool WaylandPlatform::available() const { return false; }
std::string_view WaylandPlatform::lastError() const { return "Wayland is not available on this platform"; }
bool WaylandPlatform::show(AppWindow&) { return false; }
bool WaylandPlatform::showPopup(AppWindow&, AppWindow&, Rect, bool) { return false; }
bool WaylandPlatform::capture(const AppWindow&, Image&) const { return false; }
bool WaylandPlatform::configured(const AppWindow&) const { return false; }
void* WaylandPlatform::nativeDisplay() const { return nullptr; }
bool WaylandPlatform::roundtrip() { return false; }
std::string_view WaylandPlatform::cursorBackend() const { return {}; }
bool WaylandPlatform::gpuPresents(const AppWindow&) const { return false; }
bool WaylandPlatform::gpuDraws(const AppWindow&) const { return false; }
bool WaylandPlatform::hdrContent(const AppWindow&) const { return false; }
std::string_view WaylandPlatform::gpuName() const { return {}; }
#else
using detail::SurfaceSource;

namespace {
struct Proxy; // wl_proxy / wl_display
using wl::Interface;
constexpr unsigned kDestroy = 1; // WL_MARSHAL_FLAG_DESTROY
constexpr unsigned kShmXrgb = 1; // WL_SHM_FORMAT_XRGB8888

struct Lib {
    std::unique_ptr<os::Library> so;
    Proxy* (*connect)(const char*) = nullptr;
    void (*disconnect)(Proxy*) = nullptr;
    int (*roundtrip)(Proxy*) = nullptr;
    int (*dispatchPending)(Proxy*) = nullptr;
    int (*flush)(Proxy*) = nullptr;
    int (*getFd)(Proxy*) = nullptr;
    int (*prepareRead)(Proxy*) = nullptr;
    int (*readEvents)(Proxy*) = nullptr;
    void (*cancelRead)(Proxy*) = nullptr;
    Proxy* (*marshal)(Proxy*, uint32_t, const Interface*, uint32_t, uint32_t, ...) = nullptr;
    int (*addListener)(Proxy*, void (**)(void), void*) = nullptr;
    void (*destroy)(Proxy*) = nullptr;
    uint32_t (*version)(Proxy*) = nullptr;
    const Interface *registry = nullptr, *compositor = nullptr, *surface = nullptr, *shm = nullptr, *shmPool = nullptr,
                    *buffer = nullptr, *seat = nullptr, *pointer = nullptr, *keyboard = nullptr, *output = nullptr,
                    *ddm = nullptr, *dataDevice = nullptr, *dataOffer = nullptr, *dataSource = nullptr, *callback = nullptr;
    bool load() {
        so = os::loadLibrary({"libwayland-client.so.0", "libwayland-client.so"});
        if (!so) return false;
        bool all = true;
        auto f = [&](auto& fn, const char* n) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(so->symbol(n)); all = all && fn; };
        auto i = [&](const Interface*& p, const char* n) { p = static_cast<const Interface*>(so->symbol(n)); all = all && p; };
        f(connect, "wl_display_connect"); f(disconnect, "wl_display_disconnect"); f(roundtrip, "wl_display_roundtrip");
        f(dispatchPending, "wl_display_dispatch_pending"); f(flush, "wl_display_flush"); f(getFd, "wl_display_get_fd");
        f(prepareRead, "wl_display_prepare_read"); f(readEvents, "wl_display_read_events"); f(cancelRead, "wl_display_cancel_read");
        f(marshal, "wl_proxy_marshal_flags"); f(addListener, "wl_proxy_add_listener"); f(destroy, "wl_proxy_destroy");
        f(version, "wl_proxy_get_version");
        i(registry, "wl_registry_interface"); i(compositor, "wl_compositor_interface"); i(surface, "wl_surface_interface");
        i(shm, "wl_shm_interface"); i(shmPool, "wl_shm_pool_interface"); i(buffer, "wl_buffer_interface");
        i(seat, "wl_seat_interface"); i(pointer, "wl_pointer_interface"); i(keyboard, "wl_keyboard_interface");
        i(output, "wl_output_interface"); i(ddm, "wl_data_device_manager_interface"); i(dataDevice, "wl_data_device_interface");
        i(dataOffer, "wl_data_offer_interface"); i(dataSource, "wl_data_source_interface"); i(callback, "wl_callback_interface");
        return all;
    }
};

/// libwayland-cursor (optional): the themed cursor fallback when the compositor lacks cursor-shape-v1.
struct CursorLib {
    struct Image { uint32_t width, height, hotspot_x, hotspot_y, delay; };
    struct Cursor { unsigned image_count; Image** images; char* name; };
    std::unique_ptr<os::Library> so;
    void* (*themeLoad)(const char*, int, Proxy*) = nullptr;
    void (*themeDestroy)(void*) = nullptr;
    Cursor* (*getCursor)(void*, const char*) = nullptr;
    Proxy* (*imageBuffer)(Image*) = nullptr;
    bool load() {
        so = os::loadLibrary({"libwayland-cursor.so.0", "libwayland-cursor.so"});
        if (!so) return false;
        bool all = true;
        auto f = [&](auto& fn, const char* n) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(so->symbol(n)); all = all && fn; };
        f(themeLoad, "wl_cursor_theme_load"); f(themeDestroy, "wl_cursor_theme_destroy");
        f(getCursor, "wl_cursor_theme_get_cursor"); f(imageBuffer, "wl_cursor_image_get_buffer");
        return all;
    }
};

struct Xkb {
    std::unique_ptr<os::Library> so;
    void* (*contextNew)(int) = nullptr;
    void* (*keymapNewFromString)(void*, const char*, int, int) = nullptr;
    void (*keymapUnref)(void*) = nullptr;
    void* (*stateNew)(void*) = nullptr;
    void (*stateUnref)(void*) = nullptr;
    int (*updateMask)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) = nullptr;
    uint32_t (*oneSym)(void*, uint32_t) = nullptr;
    int (*utf8)(void*, uint32_t, char*, std::size_t) = nullptr;
    int (*modActive)(void*, const char*, int) = nullptr;
    void* ctx = nullptr;
    bool load() {
        so = os::loadLibrary({"libxkbcommon.so.0", "libxkbcommon.so"});
        if (!so) return false;
        bool all = true;
        auto f = [&](auto& fn, const char* n) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(so->symbol(n)); all = all && fn; };
        f(contextNew, "xkb_context_new"); f(keymapNewFromString, "xkb_keymap_new_from_string"); f(keymapUnref, "xkb_keymap_unref");
        f(stateNew, "xkb_state_new"); f(stateUnref, "xkb_state_unref"); f(updateMask, "xkb_state_update_mask");
        f(oneSym, "xkb_state_key_get_one_sym"); f(utf8, "xkb_state_key_get_utf8"); f(modActive, "xkb_state_mod_name_is_active");
        if (all) ctx = contextNew(0);
        return all && ctx;
    }
};

template <class... F> struct Listener { void (*fns[sizeof...(F)])(void); };
template <class... F> Listener<F...> listen(F... f) { return {{reinterpret_cast<void (*)(void)>(f)...}}; }
} // namespace

struct WaylandPlatform::Impl {
    Application& app;
    Lib l;
    Xkb xkb;
    bool haveXkb = false;
    wl::XdgTables xdg;
    wl::TextInputTables tiTables;
    wl::ScaleTables scaleTables;
    Proxy *viewporter = nullptr, *fracMgr = nullptr;
    Proxy* dpy = nullptr;
    Proxy *registry = nullptr, *compositor = nullptr, *shm = nullptr, *wmBase = nullptr, *seat = nullptr,
          *pointer = nullptr, *keyboard = nullptr, *ddm = nullptr, *dataDevice = nullptr;
    std::string error;
    EventSource previous{};

    struct Buffer {
        os::SharedBuffer mem;
        Proxy* buf = nullptr;
        int w = 0, h = 0;
        bool busy = false;
    };
    struct Win {
        Impl* impl = nullptr;
        AppWindow* win = nullptr;
        Proxy *surface = nullptr, *xsurf = nullptr, *top = nullptr, *popup = nullptr;
        bool configured = false;
        u64 titleRev = 0; ///< AppWindow::titleRevision() sent with set_title
        int pendingW = 0, pendingH = 0;
        Buffer bufs[2];
        Image shown;
        detail::VkWin* vk = nullptr; ///< Vulkan swapchain on `surface` (then no wl_shm buffers are attached)
        bool vkTried = false;
        Proxy* frameCb = nullptr; ///< wl_surface.frame of the last SHM commit (AppWindow::framePending until done)
        // HiDPI: fractional-scale-v1 + viewporter when offered, else wl_surface.preferred_buffer_scale (v6)
        Proxy *viewport = nullptr, *frac = nullptr;
        uint32_t scale120 = 0;     ///< preferred_scale (0: none received)
        int32_t bufferScale = 1;   ///< integer preferred_buffer_scale
        int32_t destW = 0, destH = 0, sentBufferScale = 1;
    };
    std::vector<std::unique_ptr<Win>> wins;
    Win* pointerFocus = nullptr;
    Win* keyFocus = nullptr;
    // pointer cursor: cursor-shape-v1 when offered, else a libwayland-cursor theme on a cursor surface
    wl::CursorShapeTables shapeTables;
    Proxy *shapeMgr = nullptr, *shapeDevice = nullptr, *cursorSurface = nullptr;
    CursorLib cursorLib;
    bool cursorLibTried = false;
    void* cursorTheme = nullptr;
    const char* cursorVia = "";
    int appliedShape = -1;     ///< last shape set for lastEnterSerial (-1: none since the enter)
    u32 appliedCursorRev = ~0u; ///< focused window's UiContext::cursorRevision() when applied
    Vec2 pointerPos{};
    uint32_t lastButtonSerial = 0, lastEnterSerial = 0;
    bool buttonDown = false;
    u8 modsNow = 0;
    void* keymap = nullptr;
    void* xkbState = nullptr;

    // key repeat (wl_keyboard.repeat_info; the client repeats, not the compositor)
    int32_t repRate = 25, repDelay = 600;
    uint32_t repCode = 0; // evdev code + 8 of the held key, 0 = none
    Key repKey = Key::Other;
    std::string repText;
    double repNext = 0.0;
    static double clockNow() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // text-input-v3 (IME)
    Proxy *tiMgr = nullptr, *textInput = nullptr;
    Win* tiFocus = nullptr;
    bool tiEnabled = false;
    Rect tiCaret{};
    std::string tiPreedit, tiCommit;
    int32_t tiPreCursor = 0;
    bool tiHavePreedit = false, tiHaveCommit = false;
    uint32_t tiCommits = 0; // number of zwp_text_input_v3.commit requests (done serial matching)

    // data device (DnD)
    std::map<Proxy*, std::vector<std::string>> offers;
    Proxy* dndOffer = nullptr;
    Win* dndWin = nullptr;
    std::string dndMime;
    Vec2 dndPos{};
    struct Source {
        Proxy* src = nullptr;
        Win* win = nullptr;
        std::vector<std::pair<std::string, std::string>> formats; ///< offered mime -> data ([0] preferred)
        Proxy *icon = nullptr, *iconBuf = nullptr;                 ///< drag icon surface + its wl_shm buffer
        os::SharedBuffer iconMem;
        const std::string* find(std::string_view mime) const {
            for (const auto& [m, d] : formats) if (m == mime) return &d;
            return nullptr;
        }
    } source;

    NativeOptions opts;
    detail::VkCtx* vk = nullptr;
    bool vkCtxTried = false;
    std::string vkError;

    Impl(Application& a, const NativeOptions& o) : app(a), opts(o) {}

    /// The window's Vulkan presenter, created on its first configured present (the surface has its xdg role and
    /// has acked a configure, so the WSI's first attach + commit is legal). Null: present with wl_shm.
    detail::VkWin* vulkanFor(Win& w) {
        if (w.vkTried) return w.vk;
        w.vkTried = true;
        if (!opts.vulkan) return nullptr;
        if (!vkCtxTried) {
            vkCtxTried = true;
            vk = opts.adoptDevice.instance && opts.adoptDevice.device
                     ? detail::vkCtxAdopt(SurfaceSource::Wayland, opts.adoptDevice, vkError)
                     : detail::vkCtxCreate(SurfaceSource::Wayland, vkError);
        }
        if (!vk) return nullptr;
        SurfaceSource s;
        s.kind = SurfaceSource::Wayland;
        s.display = dpy;
        s.surface = w.surface;
        w.vk = detail::vkWinCreate(vk, s, opts, vkError);
        return w.vk;
    }

    Win* find(Proxy* surface) {
        for (auto& w : wins)
            if (w->surface == surface) return w.get();
        return nullptr;
    }
    Proxy* bind(uint32_t name, const Interface* iface, uint32_t ver) {
        return l.marshal(registry, 0, iface, ver, 0, name, iface->name, ver, nullptr);
    }

    // ---- registry ----
    static void onGlobal(void* d, Proxy*, uint32_t name, const char* iface, uint32_t ver) {
        Impl& m = *static_cast<Impl*>(d);
        const std::string_view n(iface);
        if (n == "wl_compositor") m.compositor = m.bind(name, m.l.compositor, std::min(ver, 6u));
        else if (n == "wp_viewporter") m.viewporter = m.bind(name, &m.scaleTables.viewporter, 1);
        else if (n == "wp_fractional_scale_manager_v1") m.fracMgr = m.bind(name, &m.scaleTables.fracManager, 1);
        else if (n == "wl_shm") m.shm = m.bind(name, m.l.shm, 1);
        else if (n == "xdg_wm_base") {
            m.wmBase = m.bind(name, &m.xdg.wmBase, 1);
            static const auto lis = listen(&onPing);
            m.l.addListener(m.wmBase, const_cast<void (**)(void)>(lis.fns), &m);
        } else if (n == "wl_seat" && !m.seat) {
            m.seat = m.bind(name, m.l.seat, std::min(ver, 5u));
            static const auto lis = listen(&onSeatCaps, &onSeatName);
            m.l.addListener(m.seat, const_cast<void (**)(void)>(lis.fns), &m);
        } else if (n == "wl_data_device_manager") m.ddm = m.bind(name, m.l.ddm, std::min(ver, 3u));
        else if (n == "zwp_text_input_manager_v3") m.tiMgr = m.bind(name, &m.tiTables.manager, 1);
        else if (n == "wp_cursor_shape_manager_v1") m.shapeMgr = m.bind(name, &m.shapeTables.manager, 1);
    }
    static void onGlobalRemove(void*, Proxy*, uint32_t) {}
    static void onPing(void* d, Proxy* wm, uint32_t serial) {
        Impl& m = *static_cast<Impl*>(d);
        m.l.marshal(wm, 3, nullptr, m.l.version(wm), 0, serial);
    }
    // ---- seat ----
    static void onSeatName(void*, Proxy*, const char*) {}
    static void onSeatCaps(void* d, Proxy* seat, uint32_t caps) {
        Impl& m = *static_cast<Impl*>(d);
        if ((caps & 1) && !m.pointer) {
            m.pointer = m.l.marshal(seat, 0, m.l.pointer, m.l.version(seat), 0, nullptr);
            static const auto lis = listen(&ptrEnter, &ptrLeave, &ptrMotion, &ptrButton, &ptrAxis, &ptrFrame,
                                           &ptrAxisSource, &ptrAxisStop, &ptrAxisDiscrete);
            m.l.addListener(m.pointer, const_cast<void (**)(void)>(lis.fns), &m);
            if (m.shapeMgr) m.shapeDevice = m.l.marshal(m.shapeMgr, 1, &m.shapeTables.device, 1, 0, nullptr, m.pointer);
        }
        if ((caps & 2) && !m.keyboard) {
            m.keyboard = m.l.marshal(seat, 1, m.l.keyboard, m.l.version(seat), 0, nullptr);
            static const auto lis = listen(&kbKeymap, &kbEnter, &kbLeave, &kbKey, &kbModifiers, &kbRepeat);
            m.l.addListener(m.keyboard, const_cast<void (**)(void)>(lis.fns), &m);
        }
    }
    static void ptrEnter(void* d, Proxy*, uint32_t serial, Proxy* surface, wl::Fixed x, wl::Fixed y) {
        Impl& m = *static_cast<Impl*>(d);
        m.pointerFocus = m.find(surface);
        m.lastEnterSerial = serial;
        m.pointerPos = {f32(wl::fromFixed(x)), f32(wl::fromFixed(y))};
        if (m.pointerFocus) m.pointerFocus->win->ui().pointerMove(m.pointerPos);
        m.appliedShape = -1; // a new enter serial: the cursor must be set again
        m.syncCursor();
    }
    /// The focused window's UiContext::cursorShape() -> the pointer cursor (set_shape, or a themed cursor surface).
    /// Wayland only accepts it with the latest wl_pointer.enter serial, so it is re-sent on every enter.
    void syncCursor() {
        if (!pointerFocus || !pointer) return;
        UiContext& ui = pointerFocus->win->ui();
        if (appliedShape >= 0 && appliedCursorRev == ui.cursorRevision()) return;
        appliedCursorRev = ui.cursorRevision();
        const u32 s = u32(ui.cursorShape()) < 8 ? u32(ui.cursorShape()) : 0u;
        if (int(s) == appliedShape) return;
        appliedShape = int(s);
        if (shapeDevice) {
            l.marshal(shapeDevice, 1, nullptr, 1, 0, lastEnterSerial, wl::kCursorShapes[s]);
            l.flush(dpy);
            cursorVia = "cursor-shape-v1";
            return;
        }
        if (!cursorLibTried) {
            cursorLibTried = true;
            if (cursorLib.load() && shm) {
                const std::string size = os::env("XCURSOR_SIZE");
                const std::string theme = os::env("XCURSOR_THEME");
                cursorTheme = cursorLib.themeLoad(theme.empty() ? nullptr : theme.c_str(), size.empty() ? 24 : std::max(8, std::atoi(size.c_str())), shm);
            }
        }
        if (!cursorTheme) return; // compositor default
        CursorLib::Cursor* c = cursorLib.getCursor(cursorTheme, wl::kCursorNames[s]);
        if (!c && s != 0) c = cursorLib.getCursor(cursorTheme, "left_ptr");
        if (!c || !c->image_count) return;
        CursorLib::Image* im = c->images[0];
        Proxy* buf = cursorLib.imageBuffer(im);
        if (!buf) return;
        if (!cursorSurface) cursorSurface = l.marshal(compositor, 0, l.surface, l.version(compositor), 0, nullptr);
        if (!cursorSurface) return;
        l.marshal(cursorSurface, 1, nullptr, l.version(cursorSurface), 0, buf, 0, 0);
        l.marshal(cursorSurface, 2, nullptr, l.version(cursorSurface), 0, 0, 0, int32_t(im->width), int32_t(im->height));
        l.marshal(cursorSurface, 6, nullptr, l.version(cursorSurface), 0);
        l.marshal(pointer, 0, nullptr, l.version(pointer), 0, lastEnterSerial, cursorSurface, int32_t(im->hotspot_x), int32_t(im->hotspot_y));
        l.flush(dpy);
        cursorVia = "libwayland-cursor";
    }
    static void ptrLeave(void* d, Proxy*, uint32_t, Proxy*) { static_cast<Impl*>(d)->pointerFocus = nullptr; }
    static void ptrMotion(void* d, Proxy*, uint32_t, wl::Fixed x, wl::Fixed y) {
        Impl& m = *static_cast<Impl*>(d);
        m.pointerPos = {f32(wl::fromFixed(x)), f32(wl::fromFixed(y))};
        if (!m.pointerFocus) return;
        AppWindow& w = *m.pointerFocus->win;
        w.ui().setModifiers(m.modsNow);
        w.ui().pointerMove(m.pointerPos);
        m.syncCursor();
        if (m.buttonDown)
            if (auto drag = w.takeExternalDrag()) m.startDrag(*m.pointerFocus, std::move(*drag));
    }
    static void ptrButton(void* d, Proxy*, uint32_t serial, uint32_t, uint32_t button, uint32_t state) {
        Impl& m = *static_cast<Impl*>(d);
        if (!m.pointerFocus) return;
        UiContext& ui = m.pointerFocus->win->ui();
        ui.setModifiers(m.modsNow);
        if (state == 1) {
            m.lastButtonSerial = serial;
            m.buttonDown = true;
            ui.pointerDown(m.pointerPos, button == 0x111 ? MouseButton::Right : button == 0x112 ? MouseButton::Middle : MouseButton::Left);
        } else {
            m.buttonDown = false;
            ui.pointerUp(m.pointerPos);
        }
        m.pointerFocus->win->requestRedraw();
    }
    static void ptrAxis(void* d, Proxy*, uint32_t, uint32_t axis, wl::Fixed v) {
        Impl& m = *static_cast<Impl*>(d);
        if (!m.pointerFocus) return;
        const f32 s = -f32(wl::fromFixed(v)) / 10.f; // 10 px per detent: wheel up = +1
        m.pointerFocus->win->ui().wheel(m.pointerPos, axis == 0 ? Vec2{0.f, s} : Vec2{-s, 0.f});
    }
    static void ptrFrame(void*, Proxy*) {}
    static void ptrAxisSource(void*, Proxy*, uint32_t) {}
    static void ptrAxisStop(void*, Proxy*, uint32_t, uint32_t) {}
    static void ptrAxisDiscrete(void*, Proxy*, uint32_t, int32_t) {}
    // ---- keyboard ----
    static void kbKeymap(void* d, Proxy*, uint32_t format, int32_t fd, uint32_t size) {
        Impl& m = *static_cast<Impl*>(d);
        if (format == 1 && m.haveXkb) {
            if (auto text = os::readMappedFd(fd, size)) {
                if (m.xkbState) m.xkb.stateUnref(m.xkbState);
                if (m.keymap) m.xkb.keymapUnref(m.keymap);
                m.keymap = m.xkb.keymapNewFromString(m.xkb.ctx, text->c_str(), 1, 0);
                m.xkbState = m.keymap ? m.xkb.stateNew(m.keymap) : nullptr;
            }
        }
        os::closeFd(fd);
    }
    static void kbEnter(void* d, Proxy*, uint32_t, Proxy* surface, wl::Array*) {
        Impl& m = *static_cast<Impl*>(d);
        m.keyFocus = m.find(surface);
    }
    static void kbLeave(void* d, Proxy*, uint32_t, Proxy*) {
        Impl& m = *static_cast<Impl*>(d);
        m.keyFocus = nullptr;
        m.repCode = 0;
    }
    static void kbKey(void* d, Proxy*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
        Impl& m = *static_cast<Impl*>(d);
        if (!m.keyFocus || !m.xkbState) return;
        UiContext& ui = m.keyFocus->win->ui();
        const uint32_t code = key + 8;
        bool ascii = false;
        const Key k = detail::keyFromKeysym(m.xkb.oneSym(m.xkbState, code), ascii);
        if (k != Key::Other) {
            if (state == 1) ui.keyDown(k, m.modsNow);
            else ui.keyUp(k, m.modsNow);
        }
        std::string text;
        if (state == 1 && !(m.modsNow & (kModCtrl | kModAlt | kModSuper))) {
            char buf[64] = {};
            const int n = m.xkb.utf8(m.xkbState, code, buf, sizeof buf);
            if (n > 0 && u8(buf[0]) >= 0x20 && buf[0] != 0x7F) {
                text.assign(buf, std::size_t(n));
                ui.textInput(text);
            }
        }
        if (state == 1 && m.repRate > 0 && (k != Key::Other || !text.empty())) { // modifiers map to Key::Other with no text
            m.repCode = code;
            m.repKey = k;
            m.repText = std::move(text);
            m.repNext = clockNow() + double(m.repDelay) / 1000.0;
        } else if (state == 0 && code == m.repCode) {
            m.repCode = 0;
        }
        m.keyFocus->win->requestRedraw();
    }
    static void kbModifiers(void* d, Proxy*, uint32_t, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
        Impl& m = *static_cast<Impl*>(d);
        if (!m.xkbState) return;
        m.xkb.updateMask(m.xkbState, dep, lat, lock, 0, 0, group);
        auto on = [&](const char* n) { return m.xkb.modActive(m.xkbState, n, 8 /*XKB_STATE_MODS_EFFECTIVE*/) > 0; };
        m.modsNow = u8((on("Shift") ? kModShift : 0) | (on("Control") ? kModCtrl : 0) | (on("Mod1") ? kModAlt : 0) |
                       (on("Mod4") ? kModSuper : 0));
    }
    static void kbRepeat(void* d, Proxy*, int32_t rate, int32_t delay) {
        Impl& m = *static_cast<Impl*>(d);
        m.repRate = rate; // 0 disables repeat
        m.repDelay = delay;
        if (rate <= 0) m.repCode = 0;
    }
    /// Seconds until the next repeat (inf when no key is held).
    f32 repeatWait() const {
        if (!repCode || !keyFocus) return INFINITY;
        return f32(std::max(0.0, repNext - clockNow()));
    }
    void fireRepeats() {
        if (!repCode || !keyFocus) return;
        const double now = clockNow();
        if (now < repNext) return;
        UiContext& ui = keyFocus->win->ui();
        const double period = 1.0 / double(repRate);
        for (int guard = 0; now >= repNext && guard < 64; ++guard) {
            if (repKey != Key::Other) ui.keyDown(repKey, modsNow);
            if (!repText.empty()) ui.textInput(repText);
            repNext += period;
        }
        if (now >= repNext) repNext = now + period; // fell far behind (suspended): do not burst
        keyFocus->win->requestRedraw();
    }
    // ---- text-input-v3 ----
    static void tiEnter(void* d, Proxy*, Proxy* surface) {
        Impl& m = *static_cast<Impl*>(d);
        m.tiFocus = m.find(surface);
        if (m.tiFocus) m.tiFocus->win->requestRedraw(); // present() syncs enable state
    }
    static void tiLeave(void* d, Proxy*, Proxy*) {
        Impl& m = *static_cast<Impl*>(d);
        if (m.tiFocus && m.tiEnabled) m.tiFocus->win->ui().textComposition({}, 0);
        m.tiFocus = nullptr;
        m.syncTextInput();
    }
    static void tiPreeditString(void* d, Proxy*, const char* text, int32_t begin, int32_t) {
        Impl& m = *static_cast<Impl*>(d);
        m.tiHavePreedit = true;
        m.tiPreedit = text ? text : "";
        m.tiPreCursor = begin < 0 ? int32_t(m.tiPreedit.size()) : begin; // -1: hide cursor -> end
    }
    static void tiCommitString(void* d, Proxy*, const char* text) {
        Impl& m = *static_cast<Impl*>(d);
        m.tiHaveCommit = true;
        m.tiCommit += text ? text : "";
    }
    static void tiDeleteSurrounding(void*, Proxy*, uint32_t, uint32_t) {} // no surrounding text is reported
    static void tiDone(void* d, Proxy*, uint32_t) {
        Impl& m = *static_cast<Impl*>(d);
        if (!m.tiFocus) { m.tiHaveCommit = m.tiHavePreedit = false; m.tiCommit.clear(); return; }
        UiContext& ui = m.tiFocus->win->ui();
        if (m.tiHaveCommit) {
            ui.textComposition({}, 0);
            if (!m.tiCommit.empty()) ui.textInput(m.tiCommit);
        }
        if (m.tiHavePreedit) ui.textComposition(m.tiPreedit, std::min(m.tiPreCursor, int32_t(m.tiPreedit.size())));
        else if (!ui.composition().empty()) ui.textComposition({}, 0);
        m.tiHaveCommit = m.tiHavePreedit = false;
        m.tiCommit.clear();
        m.tiPreedit.clear();
        m.tiFocus->win->requestRedraw();
    }
    /// Enables / disables the IME to follow UiContext::wantsTextInput and reports the caret rectangle.
    void syncTextInput() {
        if (!textInput) return;
        const bool want = tiFocus && tiFocus->win->ui().wantsTextInput();
        auto rq = [&](uint32_t op, auto... a) { l.marshal(textInput, op, nullptr, 1, 0, a...); };
        bool dirty = false;
        if (want != tiEnabled) {
            tiEnabled = want;
            if (want) { rq(1); rq(5, 0u, 0u); tiCaret = {-1, -1, -1, -1}; } // enable; content type normal/normal
            else rq(2);
            dirty = true;
        }
        if (want) {
            const Rect c = tiFocus->win->ui().textCaret();
            if (c.x != tiCaret.x || c.y != tiCaret.y || c.w != tiCaret.w || c.h != tiCaret.h) {
                tiCaret = c;
                rq(6, int32_t(c.x), int32_t(c.y), std::max(1, int32_t(c.w)), std::max(1, int32_t(c.h)));
                dirty = true;
            }
        }
        if (dirty) { rq(7); ++tiCommits; l.flush(dpy); }
    }
    // ---- xdg ----
    static void xsurfConfigure(void* d, Proxy* xs, uint32_t serial) {
        Win& w = *static_cast<Win*>(d);
        Impl& m = *w.impl;
        m.l.marshal(xs, 4, nullptr, m.l.version(xs), 0, serial);
        if (w.pendingW > 0 && w.pendingH > 0) {
            const Size s = w.win->size();
            if (u32(w.pendingW) != u32(s.w) || u32(w.pendingH) != u32(s.h)) w.win->resize(u32(w.pendingW), u32(w.pendingH));
        }
        w.configured = true;
        w.win->requestRedraw();
    }
    static void topConfigure(void* d, Proxy*, int32_t width, int32_t height, wl::Array*) {
        Win& w = *static_cast<Win*>(d);
        w.pendingW = width;
        w.pendingH = height;
    }
    static void topClose(void* d, Proxy*) { static_cast<Win*>(d)->win->requestClose(); }
    static void popConfigure(void* d, Proxy*, int32_t, int32_t, int32_t width, int32_t height) {
        Win& w = *static_cast<Win*>(d);
        w.pendingW = width;
        w.pendingH = height;
    }
    static void popDone(void* d, Proxy*) { static_cast<Win*>(d)->win->requestClose(); }
    static void bufRelease(void* d, Proxy*) { static_cast<Buffer*>(d)->busy = false; }

    // ---- HiDPI ----
    static void surfEnter(void*, Proxy*, Proxy*) {}
    static void surfLeave(void*, Proxy*, Proxy*) {}
    static void surfPreferredScale(void* d, Proxy*, int32_t s) {
        Win& w = *static_cast<Win*>(d);
        w.bufferScale = std::max(1, s);
        w.impl->applyScale(w);
    }
    static void surfPreferredTransform(void*, Proxy*, uint32_t) {}
    static void fracPreferred(void* d, Proxy*, uint32_t scale120) {
        Win& w = *static_cast<Win*>(d);
        w.scale120 = scale120;
        w.impl->applyScale(w);
    }
    void attachScale(Win& w) {
        static const auto sl = listen(&surfEnter, &surfLeave, &surfPreferredScale, &surfPreferredTransform);
        l.addListener(w.surface, const_cast<void (**)(void)>(sl.fns), &w);
        if (viewporter && fracMgr) {
            w.viewport = l.marshal(viewporter, 1, &scaleTables.viewport, 1, 0, nullptr, w.surface);
            w.frac = l.marshal(fracMgr, 1, &scaleTables.frac, 1, 0, nullptr, w.surface);
            static const auto fl = listen(&fracPreferred);
            l.addListener(w.frac, const_cast<void (**)(void)>(fl.fns), &w);
        }
        applyScale(w);
    }
    void applyScale(Win& w) {
        const f32 s = w.frac && w.scale120 ? f32(w.scale120) / 120.f : f32(w.bufferScale);
        if (s != w.win->scale()) w.win->setScale(s);
        w.win->requestRedraw();
    }
    /// Double-buffered surface state for the next commit (ours or the Vulkan WSI's): the viewport maps the
    /// physical-size buffer to the logical size; without fractional-scale the integer buffer scale does.
    void syncScaleState(Win& w) {
        const Size sz = w.win->size();
        if (w.viewport) {
            const int32_t dw = std::max(1, int32_t(std::lround(sz.w))), dh = std::max(1, int32_t(std::lround(sz.h)));
            if (dw != w.destW || dh != w.destH) {
                l.marshal(w.viewport, 2, nullptr, 1, 0, dw, dh);
                w.destW = dw;
                w.destH = dh;
            }
        } else if (l.version(w.surface) >= 3) {
            const int32_t bs = int32_t(std::lround(w.win->scale()));
            if (bs != w.sentBufferScale) {
                l.marshal(w.surface, 8, nullptr, l.version(w.surface), 0, bs); // wl_surface.set_buffer_scale
                w.sentBufferScale = bs;
            }
        }
    }

    // ---- presentation (wl_shm) ----
    static bool present(void* user, AppWindow& aw, Rect damage) {
        Win& w = *static_cast<Win*>(user);
        Impl& m = *w.impl;
        if (&w == m.tiFocus) m.syncTextInput();
        if (w.top && w.titleRev != aw.titleRevision()) { // live retitle: xdg_toplevel.set_title
            w.titleRev = aw.titleRevision();
            m.l.marshal(w.top, 2, nullptr, 1, 0, aw.title().c_str());
        }
        if (w.configured) m.syncScaleState(w);
        if (&w == m.pointerFocus) m.syncCursor();
        if (w.configured) {
            if (detail::VkWin* vk = m.vulkanFor(w)) {
                const bool ok = detail::vkWinPresent(vk, aw, damage); // acquire / blit / vkQueuePresentKHR commits
                m.l.flush(m.dpy);
                return ok;
            }
        }
        aw.rasterizeCpu(damage);
        if (!w.configured) return true;
        const Image& img = aw.image();
        Buffer* b = !w.bufs[0].busy ? &w.bufs[0] : !w.bufs[1].busy ? &w.bufs[1] : nullptr;
        if (!b) return false; // both with the compositor: try again next frame
        if (b->w != img.width || b->h != img.height || !b->buf) {
            if (b->buf) m.l.marshal(b->buf, 0, nullptr, 1, kDestroy);
            b->buf = nullptr;
            const int stride = img.width * 4;
            auto mem = os::createSharedBuffer(std::size_t(stride) * std::size_t(img.height));
            if (!mem) return false;
            b->mem = std::move(*mem);
            Proxy* pool = m.l.marshal(m.shm, 0, m.l.shmPool, 1, 0, nullptr, b->mem.fd, int32_t(b->mem.size));
            b->buf = m.l.marshal(pool, 0, m.l.buffer, 1, 0, nullptr, 0, img.width, img.height, stride, kShmXrgb);
            m.l.marshal(pool, 1, nullptr, 1, kDestroy);
            static const auto lis = listen(&bufRelease);
            m.l.addListener(b->buf, const_cast<void (**)(void)>(lis.fns), b);
            b->w = img.width;
            b->h = img.height;
        }
        // fuse::ui::Image is 0xAABBGGRR; wl_shm XRGB8888 is 0xXXRRGGBB (native endian)
        auto* dst = static_cast<u32*>(b->mem.data);
        for (std::size_t i = 0; i < img.pixels.size(); ++i) {
            const u32 p = img.pixels[i];
            dst[i] = 0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
        }
        w.shown = img;
        b->busy = true;
        m.l.marshal(w.surface, 1, nullptr, m.l.version(w.surface), 0, b->buf, 0, 0);
        if (m.l.version(w.surface) >= 4) m.l.marshal(w.surface, 9, nullptr, m.l.version(w.surface), 0, 0, 0, img.width, img.height);
        else m.l.marshal(w.surface, 2, nullptr, m.l.version(w.surface), 0, 0, 0, img.width, img.height);
        if (m.opts.vsync && !w.frameCb) { // throttle to the compositor's frame clock (no frames while hidden)
            w.frameCb = m.l.marshal(w.surface, 3, m.l.callback, 1, 0, nullptr);
            static const auto lis = listen(&frameDone);
            if (w.frameCb) {
                m.l.addListener(w.frameCb, const_cast<void (**)(void)>(lis.fns), &w);
                aw.setFramePending(true);
            }
        }
        m.l.marshal(w.surface, 6, nullptr, m.l.version(w.surface), 0);
        m.l.flush(m.dpy);
        return true;
    }
    static void frameDone(void* d, Proxy* cb, uint32_t) {
        Win& w = *static_cast<Win*>(d);
        w.impl->l.destroy(cb);
        w.frameCb = nullptr;
        if (w.win) w.win->setFramePending(false);
    }
    static void destroyWin(void* user) {
        Win* w = static_cast<Win*>(user);
        Impl& m = *w->impl;
        if (w->vk) { detail::vkWinDestroy(w->vk); w->vk = nullptr; } // the swapchain goes before its wl_surface
        if (w->frameCb) { m.l.destroy(w->frameCb); w->frameCb = nullptr; }
        for (auto& b : w->bufs)
            if (b.buf) m.l.marshal(b.buf, 0, nullptr, 1, kDestroy);
        if (w->frac) m.l.marshal(w->frac, 0, nullptr, 1, kDestroy);
        if (w->viewport) m.l.marshal(w->viewport, 0, nullptr, 1, kDestroy);
        if (w->popup) m.l.marshal(w->popup, 0, nullptr, 1, kDestroy);
        if (w->top) m.l.marshal(w->top, 0, nullptr, 1, kDestroy);
        if (w->xsurf) m.l.marshal(w->xsurf, 0, nullptr, 1, kDestroy);
        if (w->surface) m.l.marshal(w->surface, 0, nullptr, m.l.version(w->surface), kDestroy);
        m.l.flush(m.dpy);
        if (m.pointerFocus == w) m.pointerFocus = nullptr;
        if (m.keyFocus == w) { m.keyFocus = nullptr; m.repCode = 0; }
        if (m.tiFocus == w) m.tiFocus = nullptr;
        if (m.dndWin == w) m.dndWin = nullptr;
        if (m.source.win == w) m.source.win = nullptr;
        std::erase_if(m.wins, [&](auto& p) { return p.get() == w; });
    }

    // ---- data device: drops in ----
    static void offerMime(void* d, Proxy* offer, const char* mime) { static_cast<Impl*>(d)->offers[offer].emplace_back(mime); }
    static void offerSourceActions(void*, Proxy*, uint32_t) {}
    static void offerAction(void*, Proxy*, uint32_t) {}
    static void ddDataOffer(void* d, Proxy*, Proxy* offer) {
        Impl& m = *static_cast<Impl*>(d);
        m.offers[offer];
        static const auto lis = listen(&offerMime, &offerSourceActions, &offerAction);
        m.l.addListener(offer, const_cast<void (**)(void)>(lis.fns), &m);
    }
    void dropOffer(Proxy* o) {
        if (!o) return;
        offers.erase(o);
        l.marshal(o, 2, nullptr, l.version(o), kDestroy);
    }
    static void ddEnter(void* d, Proxy*, uint32_t serial, Proxy* surface, wl::Fixed x, wl::Fixed y, Proxy* offer) {
        Impl& m = *static_cast<Impl*>(d);
        if (m.dndOffer && m.dndOffer != offer) m.dropOffer(m.dndOffer);
        m.dndOffer = offer;
        m.dndWin = m.find(surface);
        m.dndPos = {f32(wl::fromFixed(x)), f32(wl::fromFixed(y))};
        m.dndMime.clear();
        if (offer && m.dndWin && m.dndWin->win->acceptsExternalDrops()) {
            for (const auto& t : m.offers[offer]) {
                if (t == "text/uri-list") { m.dndMime = t; break; }
                if (m.dndMime.empty() && (t == "text/plain;charset=utf-8" || t == "text/plain" || t == "UTF8_STRING")) m.dndMime = t;
            }
        }
        if (!offer) return;
        m.l.marshal(offer, 0, nullptr, m.l.version(offer), 0, serial, m.dndMime.empty() ? nullptr : m.dndMime.c_str());
        if (m.l.version(offer) >= 3) m.l.marshal(offer, 4, nullptr, m.l.version(offer), 0, m.dndMime.empty() ? 0u : 1u, 1u);
    }
    static void ddLeave(void* d, Proxy*) {
        Impl& m = *static_cast<Impl*>(d);
        m.dropOffer(m.dndOffer);
        m.dndOffer = nullptr;
        m.dndWin = nullptr;
    }
    static void ddMotion(void* d, Proxy*, uint32_t, wl::Fixed x, wl::Fixed y) {
        Impl& m = *static_cast<Impl*>(d);
        m.dndPos = {f32(wl::fromFixed(x)), f32(wl::fromFixed(y))};
        if (m.dndWin) m.dndWin->win->ui().pointerMove(m.dndPos);
    }
    static void ddDrop(void* d, Proxy*) {
        Impl& m = *static_cast<Impl*>(d);
        Proxy* offer = m.dndOffer;
        if (!offer || !m.dndWin || m.dndMime.empty()) { ddLeave(d, nullptr); return; }
        AppWindow::ExternalDrop drop;
        drop.pos = m.dndPos;
        drop.mime = m.dndMime == "text/uri-list" ? "text/uri-list" : "text/plain";
        // the preferred type, then the rest of the offer (up to 8 types): one pipe each
        std::vector<std::string> want{m.dndMime};
        for (const auto& t : m.offers[offer])
            if (want.size() < 8 && std::find(want.begin(), want.end(), t) == want.end()) want.push_back(t);
        const bool own = m.source.src && m.source.win;
        for (const auto& t : want) {
            std::string bytes;
            if (own) { // our own drag: no round trip through a pipe we would block on
                const std::string* v = m.source.find(t);
                if (!v) continue;
                bytes = *v;
            } else if (auto p = os::makePipe()) {
                m.l.marshal(offer, 1, nullptr, m.l.version(offer), 0, t.c_str(), p->second);
                os::closeFd(p->second);
                m.l.flush(m.dpy);
                bytes = os::readAllAndClose(p->first);
            } else continue;
            const std::string mime = t == "UTF8_STRING" || t.starts_with("text/plain") ? "text/plain" : t;
            bool dup = false;
            for (const auto& f : drop.formats) dup |= f.first == mime;
            if (&t == &want.front()) drop.data = bytes;
            if (!dup) drop.formats.emplace_back(mime, std::move(bytes));
        }
        if (drop.mime == "text/uri-list") drop.paths = AppWindow::pathsFromUriList(drop.data);
        if (m.l.version(offer) >= 3) m.l.marshal(offer, 3, nullptr, m.l.version(offer), 0);
        AppWindow* target = m.dndWin->win;
        m.dropOffer(offer);
        m.dndOffer = nullptr;
        m.dndWin = nullptr;
        target->deliverExternalDrop(drop);
    }
    static void ddSelection(void* d, Proxy*, Proxy* offer) {
        Impl& m = *static_cast<Impl*>(d);
        (void)m;
        (void)offer; // clipboard stays on os::clipboardData (wl-paste); the offer is owned by the library cache
    }

    // ---- data source: drags out ----
    void startDrag(Win& w, AppWindow::ExternalDrag d) {
        if (!ddm || !dataDevice || source.src) return;
        source = {};
        source.win = &w;
        for (auto& [mime, data] : d.offered()) {
            if (mime == "text/plain") { // the usual text spellings, all with the same bytes
                source.formats.emplace_back("text/plain;charset=utf-8", data);
                source.formats.emplace_back("UTF8_STRING", data);
            }
            source.formats.emplace_back(std::move(mime), std::move(data));
        }
        if (source.formats.empty()) { source = {}; return; }
        source.src = l.marshal(ddm, 0, l.dataSource, l.version(ddm), 0, nullptr);
        static const auto lis = listen(&srcTarget, &srcSend, &srcCancelled, &srcDropPerformed, &srcFinished, &srcAction);
        l.addListener(source.src, const_cast<void (**)(void)>(lis.fns), this);
        for (const auto& f : source.formats) l.marshal(source.src, 0, nullptr, l.version(source.src), 0, f.first.c_str());
        if (l.version(source.src) >= 3) l.marshal(source.src, 2, nullptr, l.version(source.src), 0, 1u);
        // drag icon: the pixmap on its own surface, offset so the hotspot sits under the pointer
        if (!d.image.empty() && d.imageW && d.imageH && d.image.size() == std::size_t(d.imageW) * d.imageH && shm) {
            const int stride = int(d.imageW) * 4;
            if (auto mem = os::createSharedBuffer(std::size_t(stride) * d.imageH)) {
                source.iconMem = std::move(*mem);
                auto* dst = static_cast<u32*>(source.iconMem.data);
                for (std::size_t i = 0; i < d.image.size(); ++i) { // premultiplied 0xAABBGGRR -> ARGB8888 (premultiplied)
                    const u32 p = d.image[i];
                    dst[i] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
                }
                Proxy* pool = l.marshal(shm, 0, l.shmPool, 1, 0, nullptr, source.iconMem.fd, int32_t(source.iconMem.size));
                source.iconBuf = l.marshal(pool, 0, l.buffer, 1, 0, nullptr, 0, int32_t(d.imageW), int32_t(d.imageH), stride, 0u /*ARGB8888*/);
                l.marshal(pool, 1, nullptr, 1, kDestroy);
                source.icon = l.marshal(compositor, 0, l.surface, l.version(compositor), 0, nullptr);
                // hotspot: wl_surface.offset from v5 (attach with a non-zero offset is a protocol error there)
                const bool v5 = l.version(source.icon) >= 5;
                l.marshal(source.icon, 1, nullptr, l.version(source.icon), 0, source.iconBuf, v5 ? 0 : -d.hotX, v5 ? 0 : -d.hotY);
                if (v5) l.marshal(source.icon, 10, nullptr, l.version(source.icon), 0, -d.hotX, -d.hotY);
                l.marshal(source.icon, 2, nullptr, l.version(source.icon), 0, 0, 0, int32_t(d.imageW), int32_t(d.imageH));
                l.marshal(source.icon, 6, nullptr, l.version(source.icon), 0);
            }
        }
        l.marshal(dataDevice, 0, nullptr, l.version(dataDevice), 0, source.src, w.surface, source.icon, lastButtonSerial);
        l.flush(dpy);
    }
    void endDrag(bool ok) {
        if (!source.src) return;
        l.marshal(source.src, 1, nullptr, l.version(source.src), kDestroy);
        if (source.icon) l.marshal(source.icon, 0, nullptr, l.version(source.icon), kDestroy);
        if (source.iconBuf) l.marshal(source.iconBuf, 0, nullptr, 1, kDestroy);
        AppWindow* w = source.win ? source.win->win : nullptr;
        source = {};
        if (w) w->deliverExternalDragEnd(ok);
    }
    static void srcTarget(void*, Proxy*, const char*) {}
    static void srcSend(void* d, Proxy*, const char* mime, int32_t fd) {
        const std::string* v = static_cast<Impl*>(d)->source.find(mime ? mime : "");
        os::writeAllAndClose(fd, v ? *v : std::string());
    }
    static void srcCancelled(void* d, Proxy*) { static_cast<Impl*>(d)->endDrag(false); }
    static void srcDropPerformed(void*, Proxy*) {}
    static void srcFinished(void* d, Proxy*) { static_cast<Impl*>(d)->endDrag(true); }
    static void srcAction(void*, Proxy*, uint32_t) {}

    // ---- event pump ----
    static bool pump(void* user, Application& app, f32 wait) {
        Impl& m = *static_cast<Impl*>(user);
        if (m.previous.pump) m.previous.pump(m.previous.user, app, 0.f);
        m.l.flush(m.dpy);
        while (m.l.prepareRead(m.dpy) != 0) m.l.dispatchPending(m.dpy);
        wait = std::min(wait, m.repeatWait());
        pollfd p{m.l.getFd(m.dpy), POLLIN, 0};
        const int ms = wait <= 0.f ? 0 : std::isinf(wait) ? -1 : int(std::min(std::ceil(double(wait) * 1000.0), 2.0e9));
        if (::poll(&p, 1, ms) > 0 && (p.revents & POLLIN)) m.l.readEvents(m.dpy);
        else m.l.cancelRead(m.dpy);
        if (m.l.dispatchPending(m.dpy) < 0) { m.error = "Wayland connection lost"; return false; }
        m.fireRepeats();
        return true;
    }
};

WaylandPlatform::WaylandPlatform(Application& app, const char* name, const NativeOptions& o)
    : m(std::make_unique<Impl>(app, o)) {
    if (!name && os::env("WAYLAND_DISPLAY").empty()) { m->error = "no WAYLAND_DISPLAY"; return; }
    if (!m->l.load()) { m->error = "libwayland-client.so.0 not found"; return; }
    m->haveXkb = m->xkb.load();
    m->xdg.init(m->l.surface, m->l.seat, m->l.output);
    m->tiTables.init(m->l.surface, m->l.seat);
    m->scaleTables.init(m->l.surface);
    m->shapeTables.init(m->l.pointer);
    m->dpy = m->l.connect(name);
    if (!m->dpy) { m->error = "cannot connect to the Wayland display"; return; }
    m->registry = m->l.marshal(m->dpy, 1, m->l.registry, m->l.version(m->dpy), 0, nullptr);
    static const auto lis = listen(&Impl::onGlobal, &Impl::onGlobalRemove);
    m->l.addListener(m->registry, const_cast<void (**)(void)>(lis.fns), m.get());
    m->l.roundtrip(m->dpy);
    m->l.roundtrip(m->dpy); // seat capabilities
    if (m->ddm && m->seat) {
        m->dataDevice = m->l.marshal(m->ddm, 1, m->l.dataDevice, m->l.version(m->ddm), 0, nullptr, m->seat);
        static const auto dl = listen(&Impl::ddDataOffer, &Impl::ddEnter, &Impl::ddLeave, &Impl::ddMotion, &Impl::ddDrop, &Impl::ddSelection);
        m->l.addListener(m->dataDevice, const_cast<void (**)(void)>(dl.fns), m.get());
    }
    if (m->tiMgr && m->seat) {
        m->textInput = m->l.marshal(m->tiMgr, 1, &m->tiTables.input, 1, 0, nullptr, m->seat);
        static const auto tl = listen(&Impl::tiEnter, &Impl::tiLeave, &Impl::tiPreeditString, &Impl::tiCommitString,
                                      &Impl::tiDeleteSurrounding, &Impl::tiDone);
        m->l.addListener(m->textInput, const_cast<void (**)(void)>(tl.fns), m.get());
    }
    if (!m->compositor || !m->shm || !m->wmBase) {
        m->error = "compositor lacks wl_compositor / wl_shm / xdg_wm_base";
        return;
    }
    m->previous = app.eventSource();
    app.setEventSource({m.get(), &Impl::pump});
}

WaylandPlatform::~WaylandPlatform() {
    if (!m->dpy) return;
    while (!m->wins.empty()) m->wins.back()->win->setPresenter({}); // destroyWin erases
    if (available()) m->app.setEventSource(m->previous);
    detail::vkCtxDestroy(m->vk); // after every VkWin (destroyWin), before the wl_display goes
    if (m->shapeDevice) m->l.marshal(m->shapeDevice, 0, nullptr, 1, kDestroy);
    if (m->cursorSurface) m->l.marshal(m->cursorSurface, 0, nullptr, m->l.version(m->cursorSurface), kDestroy);
    if (m->cursorTheme) m->cursorLib.themeDestroy(m->cursorTheme); // owns the cursor wl_buffers
    m->l.disconnect(m->dpy);
}
bool WaylandPlatform::available() const { return m->dpy && m->compositor && m->shm && m->wmBase; }
std::string_view WaylandPlatform::lastError() const { return m->error.empty() ? std::string_view(m->vkError) : std::string_view(m->error); }
void* WaylandPlatform::nativeDisplay() const { return m->dpy; }
bool WaylandPlatform::roundtrip() { return m->dpy && m->l.roundtrip(m->dpy) >= 0; }
std::string_view WaylandPlatform::cursorBackend() const { return m->cursorVia; }

bool WaylandPlatform::show(AppWindow& aw) {
    if (!available()) return false;
    auto w = std::make_unique<Impl::Win>();
    w->impl = m.get();
    w->win = &aw;
    auto& l = m->l;
    w->surface = l.marshal(m->compositor, 0, l.surface, l.version(m->compositor), 0, nullptr);
    m->attachScale(*w);
    w->xsurf = l.marshal(m->wmBase, 2, &m->xdg.surface, 1, 0, nullptr, w->surface);
    static const auto xl = listen(&Impl::xsurfConfigure);
    l.addListener(w->xsurf, const_cast<void (**)(void)>(xl.fns), w.get());
    w->top = l.marshal(w->xsurf, 1, &m->xdg.toplevel, 1, 0, nullptr);
    static const auto tl = listen(&Impl::topConfigure, &Impl::topClose);
    l.addListener(w->top, const_cast<void (**)(void)>(tl.fns), w.get());
    l.marshal(w->top, 2, nullptr, 1, 0, aw.title().c_str());
    l.marshal(w->top, 3, nullptr, 1, 0, m->app.name().c_str());
    l.marshal(w->surface, 6, nullptr, l.version(w->surface), 0); // initial commit -> configure
    l.flush(m->dpy);
    Impl::Win* raw = w.get();
    m->wins.push_back(std::move(w));
    aw.setPresenter({raw, &Impl::present, &Impl::destroyWin, m->opts.vsync}); // frame callbacks / FIFO pace
    l.roundtrip(m->dpy);
    return true;
}
bool WaylandPlatform::showPopup(AppWindow& aw, AppWindow& parent, Rect anchor, bool grab) {
    if (!available()) return false;
    Impl::Win* par = nullptr;
    for (auto& w : m->wins)
        if (w->win == &parent) par = w.get();
    if (!par || !par->xsurf) return false;
    auto& l = m->l;
    auto w = std::make_unique<Impl::Win>();
    w->impl = m.get();
    w->win = &aw;
    w->surface = l.marshal(m->compositor, 0, l.surface, l.version(m->compositor), 0, nullptr);
    m->attachScale(*w);
    w->xsurf = l.marshal(m->wmBase, 2, &m->xdg.surface, 1, 0, nullptr, w->surface);
    static const auto xl = listen(&Impl::xsurfConfigure);
    l.addListener(w->xsurf, const_cast<void (**)(void)>(xl.fns), w.get());
    // Below the anchor, sliding / flipping to stay on screen (the compositor knows the output; we do not).
    Proxy* pos = l.marshal(m->wmBase, 1, &m->xdg.positioner, 1, 0, nullptr);
    const Size sz = aw.size();
    l.marshal(pos, 1, nullptr, 1, 0, std::max(1, int32_t(sz.w)), std::max(1, int32_t(sz.h)));
    l.marshal(pos, 2, nullptr, 1, 0, int32_t(anchor.x), int32_t(anchor.y), std::max(1, int32_t(anchor.w)),
              std::max(1, int32_t(anchor.h)));
    l.marshal(pos, 3, nullptr, 1, 0, 6u);         // anchor: bottom_left
    l.marshal(pos, 4, nullptr, 1, 0, 8u);         // gravity: bottom_right
    l.marshal(pos, 5, nullptr, 1, 0, 1u | 2u | 8u); // slide_x | slide_y | flip_y
    w->popup = l.marshal(w->xsurf, 2, &m->xdg.popup, 1, 0, nullptr, par->xsurf, pos);
    l.marshal(pos, 0, nullptr, 1, kDestroy);
    static const auto pl = listen(&Impl::popConfigure, &Impl::popDone);
    l.addListener(w->popup, const_cast<void (**)(void)>(pl.fns), w.get());
    if (grab && m->seat && m->lastButtonSerial) l.marshal(w->popup, 1, nullptr, 1, 0, m->seat, m->lastButtonSerial);
    l.marshal(w->surface, 6, nullptr, l.version(w->surface), 0);
    l.flush(m->dpy);
    Impl::Win* raw = w.get();
    m->wins.push_back(std::move(w));
    aw.setPresenter({raw, &Impl::present, &Impl::destroyWin, m->opts.vsync}); // frame callbacks / FIFO pace
    l.roundtrip(m->dpy);
    return true;
}
bool WaylandPlatform::capture(const AppWindow& aw, Image& out) const {
    for (auto& w : m->wins) {
        if (w->win != &aw) continue;
        if (w->vk) return detail::vkWinReadback(w->vk, out);
        if (w->shown.width > 0) { out = w->shown; return true; }
    }
    return false;
}
bool WaylandPlatform::gpuPresents(const AppWindow& aw) const {
    for (auto& w : m->wins)
        if (w->win == &aw) return w->vk != nullptr;
    return false;
}
bool WaylandPlatform::gpuDraws(const AppWindow& aw) const {
    for (auto& w : m->wins)
        if (w->win == &aw) return w->vk && detail::vkWinGpuDraws(w->vk);
    return false;
}
bool WaylandPlatform::hdrContent(const AppWindow& aw) const {
    for (auto& w : m->wins)
        if (w->win == &aw) return w->vk && detail::vkWinHdrContent(w->vk);
    return false;
}
std::string_view WaylandPlatform::gpuName() const { return detail::vkCtxDeviceName(m->vk); }
bool WaylandPlatform::configured(const AppWindow& aw) const {
    for (auto& w : m->wins)
        if (w->win == &aw) return w->configured;
    return false;
}
#endif

} // namespace fuse::app
