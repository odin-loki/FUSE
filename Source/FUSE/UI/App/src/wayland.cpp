// Wayland backend: see fuse/app/wayland.hpp. libwayland-client and libxkbcommon are loaded through fuse::ui::os
// (FORGE); shared-memory buffers, keymaps and data transfer use the seam's fd helpers.
#include "fuse/app/wayland.hpp"
#include "wl_protocol.hpp"

#include "fuse/ui/os.hpp"

#include <algorithm>
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
WaylandPlatform::WaylandPlatform(Application&, const char*) : m(std::make_unique<Impl>()) {}
WaylandPlatform::~WaylandPlatform() = default;
bool WaylandPlatform::available() const { return false; }
std::string_view WaylandPlatform::lastError() const { return "Wayland is not available on this platform"; }
bool WaylandPlatform::show(AppWindow&) { return false; }
bool WaylandPlatform::capture(const AppWindow&, Image&) const { return false; }
bool WaylandPlatform::configured(const AppWindow&) const { return false; }
void* WaylandPlatform::nativeDisplay() const { return nullptr; }
bool WaylandPlatform::roundtrip() { return false; }
#else

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
        Proxy *surface = nullptr, *xsurf = nullptr, *top = nullptr;
        bool configured = false;
        int pendingW = 0, pendingH = 0;
        Buffer bufs[2];
        Image shown;
    };
    std::vector<std::unique_ptr<Win>> wins;
    Win* pointerFocus = nullptr;
    Win* keyFocus = nullptr;
    Vec2 pointerPos{};
    uint32_t lastButtonSerial = 0, lastEnterSerial = 0;
    bool buttonDown = false;
    u8 modsNow = 0;
    void* keymap = nullptr;
    void* xkbState = nullptr;

    // data device (DnD)
    std::map<Proxy*, std::vector<std::string>> offers;
    Proxy* dndOffer = nullptr;
    Win* dndWin = nullptr;
    std::string dndMime;
    Vec2 dndPos{};
    struct Source {
        Proxy* src = nullptr;
        Win* win = nullptr;
        std::string mime, data;
    } source;

    explicit Impl(Application& a) : app(a) {}

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
        if (n == "wl_compositor") m.compositor = m.bind(name, m.l.compositor, std::min(ver, 4u));
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
    }
    static void ptrLeave(void* d, Proxy*, uint32_t, Proxy*) { static_cast<Impl*>(d)->pointerFocus = nullptr; }
    static void ptrMotion(void* d, Proxy*, uint32_t, wl::Fixed x, wl::Fixed y) {
        Impl& m = *static_cast<Impl*>(d);
        m.pointerPos = {f32(wl::fromFixed(x)), f32(wl::fromFixed(y))};
        if (!m.pointerFocus) return;
        AppWindow& w = *m.pointerFocus->win;
        w.ui().setModifiers(m.modsNow);
        w.ui().pointerMove(m.pointerPos);
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
    static void kbLeave(void* d, Proxy*, uint32_t, Proxy*) { static_cast<Impl*>(d)->keyFocus = nullptr; }
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
        if (state == 1 && !(m.modsNow & (kModCtrl | kModAlt | kModSuper))) {
            char buf[64] = {};
            const int n = m.xkb.utf8(m.xkbState, code, buf, sizeof buf);
            if (n > 0 && u8(buf[0]) >= 0x20 && buf[0] != 0x7F) ui.textInput(std::string_view(buf, std::size_t(n)));
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
    static void kbRepeat(void*, Proxy*, int32_t, int32_t) {}
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
    static void bufRelease(void* d, Proxy*) { static_cast<Buffer*>(d)->busy = false; }

    // ---- presentation (wl_shm) ----
    static bool present(void* user, AppWindow& aw, Rect damage) {
        Win& w = *static_cast<Win*>(user);
        Impl& m = *w.impl;
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
        m.l.marshal(w.surface, 6, nullptr, m.l.version(w.surface), 0);
        m.l.flush(m.dpy);
        return true;
    }
    static void destroyWin(void* user) {
        Win* w = static_cast<Win*>(user);
        Impl& m = *w->impl;
        for (auto& b : w->bufs)
            if (b.buf) m.l.marshal(b.buf, 0, nullptr, 1, kDestroy);
        if (w->top) m.l.marshal(w->top, 0, nullptr, 1, kDestroy);
        if (w->xsurf) m.l.marshal(w->xsurf, 0, nullptr, 1, kDestroy);
        if (w->surface) m.l.marshal(w->surface, 0, nullptr, m.l.version(w->surface), kDestroy);
        m.l.flush(m.dpy);
        if (m.pointerFocus == w) m.pointerFocus = nullptr;
        if (m.keyFocus == w) m.keyFocus = nullptr;
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
        if (m.source.src && m.source.win) {
            drop.data = m.source.data; // our own drag: no round trip through a pipe we would block on
        } else if (auto p = os::makePipe()) {
            m.l.marshal(offer, 1, nullptr, m.l.version(offer), 0, m.dndMime.c_str(), p->second);
            os::closeFd(p->second);
            m.l.flush(m.dpy);
            drop.data = os::readAllAndClose(p->first);
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
        if (!d.paths.empty()) { source.mime = "text/uri-list"; source.data = AppWindow::uriListFromPaths(d.paths); }
        else { source.mime = d.mime.empty() || d.mime == "text/plain" ? "text/plain;charset=utf-8" : d.mime; source.data = std::move(d.data); }
        source.src = l.marshal(ddm, 0, l.dataSource, l.version(ddm), 0, nullptr);
        static const auto lis = listen(&srcTarget, &srcSend, &srcCancelled, &srcDropPerformed, &srcFinished, &srcAction);
        l.addListener(source.src, const_cast<void (**)(void)>(lis.fns), this);
        l.marshal(source.src, 0, nullptr, l.version(source.src), 0, source.mime.c_str());
        if (source.mime == "text/plain;charset=utf-8") l.marshal(source.src, 0, nullptr, l.version(source.src), 0, "text/plain");
        if (l.version(source.src) >= 3) l.marshal(source.src, 2, nullptr, l.version(source.src), 0, 1u);
        l.marshal(dataDevice, 0, nullptr, l.version(dataDevice), 0, source.src, w.surface, nullptr, lastButtonSerial);
        l.flush(dpy);
    }
    void endDrag(bool ok) {
        if (!source.src) return;
        l.marshal(source.src, 1, nullptr, l.version(source.src), kDestroy);
        AppWindow* w = source.win ? source.win->win : nullptr;
        source = {};
        if (w) w->deliverExternalDragEnd(ok);
    }
    static void srcTarget(void*, Proxy*, const char*) {}
    static void srcSend(void* d, Proxy*, const char*, int32_t fd) { os::writeAllAndClose(fd, static_cast<Impl*>(d)->source.data); }
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
        pollfd p{m.l.getFd(m.dpy), POLLIN, 0};
        const int ms = wait <= 0.f ? 0 : std::isinf(wait) ? -1 : int(std::min(std::ceil(double(wait) * 1000.0), 2.0e9));
        if (::poll(&p, 1, ms) > 0 && (p.revents & POLLIN)) m.l.readEvents(m.dpy);
        else m.l.cancelRead(m.dpy);
        if (m.l.dispatchPending(m.dpy) < 0) { m.error = "Wayland connection lost"; return false; }
        return true;
    }
};

WaylandPlatform::WaylandPlatform(Application& app, const char* name) : m(std::make_unique<Impl>(app)) {
    if (!name && os::env("WAYLAND_DISPLAY").empty()) { m->error = "no WAYLAND_DISPLAY"; return; }
    if (!m->l.load()) { m->error = "libwayland-client.so.0 not found"; return; }
    m->haveXkb = m->xkb.load();
    m->xdg.init(m->l.surface, m->l.seat, m->l.output);
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
    m->l.disconnect(m->dpy);
}
bool WaylandPlatform::available() const { return m->dpy && m->compositor && m->shm && m->wmBase; }
std::string_view WaylandPlatform::lastError() const { return m->error; }
void* WaylandPlatform::nativeDisplay() const { return m->dpy; }
bool WaylandPlatform::roundtrip() { return m->dpy && m->l.roundtrip(m->dpy) >= 0; }

bool WaylandPlatform::show(AppWindow& aw) {
    if (!available()) return false;
    auto w = std::make_unique<Impl::Win>();
    w->impl = m.get();
    w->win = &aw;
    auto& l = m->l;
    w->surface = l.marshal(m->compositor, 0, l.surface, l.version(m->compositor), 0, nullptr);
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
    aw.setPresenter({raw, &Impl::present, &Impl::destroyWin});
    l.roundtrip(m->dpy);
    return true;
}
bool WaylandPlatform::capture(const AppWindow& aw, Image& out) const {
    for (auto& w : m->wins)
        if (w->win == &aw && w->shown.width > 0) { out = w->shown; return true; }
    return false;
}
bool WaylandPlatform::configured(const AppWindow& aw) const {
    for (auto& w : m->wins)
        if (w->win == &aw) return w->configured;
    return false;
}
#endif

} // namespace fuse::app
