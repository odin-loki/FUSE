// NativePlatform's Wayland backend: see native_wayland_platform.hpp.
#include "native_wayland_platform.hpp"

#if !defined(_WIN32) && !defined(__ANDROID__) && !defined(__APPLE__)
#include "fuse/app/chrome.hpp"
#include "fuse/ui/widgets/widgets.hpp"
#include "wayland_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <poll.h>

namespace fuse::app::detail {
using namespace fuse::ui;

struct WaylandPlatform::State {
    struct Rec {
        State* st = nullptr;
        AppWindow* win = nullptr;
        wl::Window* wl = nullptr;
        VkWin* vk = nullptr;
        PresenterKind kind = PresenterKind::Headless;
        struct Popup {
            wl::Child* child = nullptr;
            Rect rect;        ///< context rect, logical units (input offset)
            Size pixels;      ///< raster size, device pixels
            u64 revision = ~0ull;
            Image img;
        };
        std::vector<Popup> popups;
        bool csd = false;     ///< the compositor does not decorate the window: it was given the client-side chrome
        NodeHandle csdBar;    ///< ... and this title bar, unless the app had made its own
    };

    Application& app;
    NativeOptions opts;
    wl::Backend* b = nullptr;
    std::vector<Rec*> recs;
    VkCtx* vk = nullptr;
    bool vkTried = false, lost = false;
    std::string error;
    std::vector<ClipboardItem> clip[2]; ///< what we serve while we own the selection: [0] clipboard, [1] primary

    State(Application& a, const NativeOptions& o) : app(a), opts(o) {}

    Rec* find(const AppWindow& w) const {
        for (Rec* r : recs)
            if (r->win == &w) return r;
        return nullptr;
    }
    Rec* find(const wl::Window* w) const {
        for (Rec* r : recs)
            if (r->wl == w) return r;
        return nullptr;
    }
    VkCtx* vkContext() {
        if (!vkTried && opts.vulkan) {
            vkTried = true;
            vk = opts.adoptDevice.instance && opts.adoptDevice.device ? vkCtxAdopt(SurfaceSource::Wayland, opts.adoptDevice, error)
                                                                      : vkCtxCreate(SurfaceSource::Wayland, error);
        }
        return vk;
    }

    // ---- OS popups: xdg_popups on the window (nested on the popup below for submenus) ----
    static void closeAllPopups(Rec& r) {
        UiContext& ui = r.win->ui();
        std::vector<NodeHandle> nodes;
        for (const auto& ps : ui.popupSurfaces()) nodes.push_back(ps.node);
        for (NodeHandle n : nodes)
            if (ui.alive(n)) fuse::ui::widgets::closePopup(ui, n);
        r.win->requestRedraw();
    }
    void syncPopups(Rec& r) {
        if (!opts.osPopups || !r.wl) return;
        UiContext& ui = r.win->ui();
        const f32 s = r.win->scale();
        const auto& surf = ui.popupSurfaces();
        // a popup whose place or size changed is made again (an xdg_popup cannot be moved before xdg-shell 3), and with
        // it everything stacked on it
        std::size_t keep = 0;
        while (keep < r.popups.size() && keep < surf.size() && r.popups[keep].rect.x == surf[keep].rect.x &&
               r.popups[keep].rect.y == surf[keep].rect.y && r.popups[keep].rect.w == surf[keep].rect.w &&
               r.popups[keep].rect.h == surf[keep].rect.h && !wl::childDismissed(r.popups[keep].child))
            ++keep;
        while (r.popups.size() > keep) { // topmost first (protocol)
            wl::destroyChild(b, r.popups.back().child);
            r.popups.pop_back();
        }
        for (std::size_t i = 0; i < surf.size(); ++i) {
            const auto& ps = surf[i];
            if (i == r.popups.size()) {
                Rec::Popup p;
                p.rect = ps.rect;
                const Rec::Popup* below = i > 0 ? &r.popups[i - 1] : nullptr;
                const Vec2 base = below ? Vec2{below->rect.x, below->rect.y} : Vec2{};
                p.child = wl::createPopup(b, r.wl, below ? below->child : nullptr, {ps.rect.x - base.x, ps.rect.y - base.y},
                                          {ps.rect.w, ps.rect.h}, {ps.rect.x, ps.rect.y}, error);
                if (!p.child) return;
                r.popups.push_back(std::move(p));
            }
            Rec::Popup& p = r.popups[i];
            if (p.revision == ps.revision) continue;
            p.revision = ps.revision;
            p.pixels = {std::max(1.f, std::ceil(ps.rect.w * s)), std::max(1.f, std::ceil(ps.rect.h * s))};
            u32 clear = 0xFF000000u; // the panel's own fill shows in the corners its radius leaves
            for (const DrawCmd& c : ps.draw.cmds())
                if (c.op != DrawOp::Shadow && c.fill.a > 0.9f) { clear = packPremultiplied(c.fill) | 0xFF000000u; break; }
            p.img.resize(int(p.pixels.w), int(p.pixels.h), clear);
            CpuRasterizer rz = r.win->rasterizer(); // the window's images + the current glyph atlas
            rz.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
            rz.render(ps.draw, p.img); // already device pixels, popup-local (UiContext::PopupSurface)
            wl::presentChild(b, p.child, p.img);
        }
    }
    void destroyPopups(Rec& r) {
        while (!r.popups.empty()) {
            wl::destroyChild(b, r.popups.back().child);
            r.popups.pop_back();
        }
    }

    // ---- presenter ----
    static bool present(void* user, AppWindow& w, Rect damage) {
        Rec& r = *static_cast<Rec*>(user);
        State& st = *r.st;
        if (!w.visible()) return false; // unmapped (AppWindow::setVisible(false)): nothing to show it on
        bool ok = false;
        if (r.kind == PresenterKind::Vulkan) {
            wl::beforeVulkanPresent(st.b, r.wl);
            ok = vkWinPresent(r.vk, w, damage);
        } else {
            if (!wl::frameReady(st.b, r.wl)) return false; // the last frame is not on screen yet: redrawn when it is
            ok = wl::presentShm(st.b, r.wl, w.rasterize(damage));
        }
        st.syncPopups(r);
        return ok;
    }
    static void destroy(void* user) {
        Rec* r = static_cast<Rec*>(user);
        State& st = *r->st;
        vkWinDestroy(r->vk); // before the wl_surface it presents to
        r->vk = nullptr;
        st.destroyPopups(*r);
        if (r->win) {
            if (r->csd) { // the fallback chrome goes with the OS window
                WindowChrome& c = r->win->chrome();
                if (r->win->ui().alive(r->csdBar)) {
                    r->win->ui().destroy(r->csdBar);
                    c = {};
                }
                c.frame = WindowFrame::System;
                c.resizeBorder = 0.f;
            }
            r->win->ui().setOsPopups(false);
            r->win->ui().setClipboard({}); // back to the context-local buffer
            r->win->setWindowBackend({});
        }
        wl::destroyWindow(st.b, r->wl);
        std::erase(st.recs, r);
        delete r;
    }

    // ---- window management (AppWindow -> xdg_toplevel) ----
    static bool windowRequest(void* user, AppWindow& w, WindowRequest what) {
        Rec& r = *static_cast<Rec*>(user);
        State& st = *r.st;
        if (!r.wl || st.lost) return false;
        switch (what) {
        case WindowRequest::Title:
            wl::setTitle(st.b, r.wl, w.title());
            if (w.ui().alive(r.csdBar)) // the client-side title bar shows it too
                if (const NodeHandle label = w.ui().find("titlebar.title"); w.ui().alive(label)) w.ui().setText(label, w.title());
            return true;
        case WindowRequest::SizeLimits: {
            wl::setSizeLimits(st.b, r.wl, w.minimumSize(), w.maximumSize());
            // the limits bind the compositor's interactive resizes; the client's own size obeys them here
            const Size now = w.size(), want = w.clampSize(now);
            if (want.w != now.w || want.h != now.h) wl::resize(st.b, r.wl, unsigned(want.w), unsigned(want.h));
            return true;
        }
        case WindowRequest::Mode: wl::setMode(st.b, r.wl, w.requestedMode()); return true;
        case WindowRequest::Visible:
            if (!wl::setVisible(st.b, r.wl, w.requestedVisible())) return false;
            if (!w.requestedVisible()) st.destroyPopups(r);
            w.notifyVisible(w.requestedVisible());
            return true;
        case WindowRequest::Position: return false; // the compositor places toplevels; there are no global coordinates
        case WindowRequest::Activate: return false; // needs xdg-activation with a token from the activating client
        case WindowRequest::Menu: return false;     // no global menu bar: AppWindow builds the in-window one
        }
        return false;
    }

    // ---- clipboard / primary selection (wl_data_device, zwp_primary_selection_v1) ----
    static constexpr const char* kTextMimes[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING"};
    static bool isTextMime(std::string_view mime) {
        for (const char* m : kTextMimes)
            if (mime == m) return true;
        return false;
    }
    /// The bytes of `mime` from the items we own (lazy items are made now, images encoded once).
    static bool itemBytes(std::vector<ClipboardItem>& items, std::string_view mime, std::vector<unsigned char>& out) {
        const bool text = isTextMime(mime);
        for (ClipboardItem& it : items) {
            if (it.mime.empty() || (text ? !isTextMime(it.mime) : it.mime != mime)) continue;
            if (it.provide) { // lazy: made when first pasted
                const auto fn = it.provide;
                it.provide = nullptr;
                if (!fn(it.user, it)) {
                    it.mime.clear(); // refused: an empty answer from now on (the offer itself cannot be changed)
                    return false;
                }
            }
            if (text) {
                out.assign(it.text.begin(), it.text.end());
            } else {
                if (it.mime == kClipboardImage && it.bytes.empty() && !encodePng(it.image, it.bytes)) return false;
                out = it.bytes;
            }
            return true;
        }
        return false;
    }
    template <int Which>
    static bool provide(void* user, const std::string& mime, std::vector<unsigned char>& out) {
        return itemBytes(static_cast<State*>(user)->clip[Which], mime, out);
    }
    bool own(bool primary, std::vector<ClipboardItem> items) {
        if (lost || !wl::hasDataDevice(b, primary)) return false;
        std::vector<std::string> mimes;
        for (const ClipboardItem& it : items) {
            if (it.mime.empty()) continue;
            if (isTextMime(it.mime)) {
                for (const char* m : kTextMimes)
                    if (std::find(mimes.begin(), mimes.end(), m) == mimes.end()) mimes.emplace_back(m);
            } else if (std::find(mimes.begin(), mimes.end(), it.mime) == mimes.end()) {
                mimes.push_back(it.mime);
            }
        }
        std::vector<ClipboardItem>& mine = clip[primary ? 1 : 0];
        mine = std::move(items);
        if (!wl::setSelection(b, primary, mimes, primary ? &provide<1> : &provide<0>, this)) {
            mine.clear();
            return false;
        }
        return true;
    }
    bool read(bool primary, std::string_view mime, std::vector<unsigned char>& out) {
        out.clear();
        if (lost || !wl::hasDataDevice(b, primary) || mime.empty()) return false;
        if (wl::ownsSelection(b, primary)) return itemBytes(clip[primary ? 1 : 0], mime, out); // ours: no round trip
        const std::vector<std::string> have = wl::selectionMimes(b, primary);
        if (!isTextMime(mime)) return std::find(have.begin(), have.end(), mime) != have.end() && wl::receiveSelection(b, primary, std::string(mime), out);
        for (const char* m : kTextMimes) // whichever text type the other client offers
            if (std::find(have.begin(), have.end(), m) != have.end()) return wl::receiveSelection(b, primary, m, out);
        return false;
    }
    bool setText(bool primary, std::string_view utf8) {
        std::vector<ClipboardItem> items(1);
        items[0].mime = kClipboardText;
        items[0].text = utf8;
        return own(primary, std::move(items));
    }
    bool text(bool primary, std::string& out) {
        std::vector<unsigned char> bytes;
        if (!read(primary, kClipboardText, bytes)) return false;
        out.assign(bytes.begin(), bytes.end());
        return !out.empty();
    }
    static bool clipSet(void* user, std::string_view utf8) { return static_cast<State*>(user)->setText(false, utf8); }
    static bool clipGet(void* user, std::string& out) { return static_cast<State*>(user)->text(false, out); }
    bool clipboardUsable() const { return !lost && wl::hasDataDevice(b, false); }

    // ---- drops from other clients -> UiContext's external drag path ----
    static void dropped(void* user, wl::Window* win, int phase, Vec2 at, const std::string& mime, const std::string& data) {
        State& st = *static_cast<State*>(user);
        Rec* r = st.find(win);
        if (!r) return;
        UiContext& ui = r->win->ui();
        DragData d;
        const bool files = mime == "text/uri-list";
        d.type = files ? kDragFiles : kDragText;
        if (phase == 1) {
            ui.dragExternalLeave();
        } else if (phase == 0) {
            ui.dragExternalMove(at, d);
        } else {
            d.text = files ? wl::uriListToPaths(data) : data;
            ui.dropExternal(at, d);
        }
        r->win->requestRedraw();
    }

    // ---- event pump ----
    static bool pump(void* user, Application&, f32 wait) {
        State& st = *static_cast<State*>(user);
        if (st.lost) { // the compositor is gone: nothing to wait on (the windows were asked to close)
            if (wait > 0.f) ::poll(nullptr, 0, std::isinf(wait) ? 100 : int(std::min(std::ceil(double(wait) * 1000.0), 1000.0)));
            return true;
        }
        wait = std::min(wait, wl::nextTimeout(st.b)); // key repeat, overdue frame callbacks
        pollfd p{wl::beforePoll(st.b), POLLIN, 0};
        const int ms = std::isinf(wait) ? -1 : int(std::min(std::ceil(double(std::max(0.f, wait)) * 1000.0), 2.0e9));
        const int n = ::poll(&p, 1, ms);
        wl::afterPoll(st.b, n > 0 && (p.revents & POLLIN) != 0);
        if (!wl::alive(st.b) || (n > 0 && (p.revents & (POLLHUP | POLLERR)) != 0 && !(p.revents & POLLIN))) {
            st.lost = true;
            st.error = "the Wayland connection was lost";
            for (Rec* r : st.recs) r->win->requestClose();
            return true;
        }
        for (Rec* r : st.recs) {
            wl::syncCursor(st.b, r->wl); // what the UI wants after this batch of input
            UiContext& ui = r->win->ui();
            wl::syncTextInput(st.b, r->wl, ui.wantsTextInput(), ui.textCaret()); // IME on while a text field has focus
        }
        return true;
    }
};

WaylandPlatform::WaylandPlatform(std::unique_ptr<State> s) : m(std::move(s)) {}

std::unique_ptr<WaylandPlatform> WaylandPlatform::open(Application& app, const NativeOptions& o, std::string& err) {
    wl::Backend* b = wl::open(err, o.scale);
    if (!b) return nullptr;
    auto st = std::make_unique<State>(app, o);
    st->b = b;
    // a grabbed xdg_popup ends in popup_done on an outside click: close the window's popups
    wl::setDismissHandler(b, [](void* user, wl::Window* owner) {
        if (State::Rec* r = static_cast<State*>(user)->find(owner)) State::closeAllPopups(*r);
    }, st.get());
    wl::setDropHandler(b, &State::dropped, st.get());
    if (const char* gtk = std::getenv("GTK_THEME")) // cheap hint; NativePlatform::systemTheme() asks gsettings
        if (const ColorScheme cs = schemeFromGtkTheme(gtk); cs != ColorScheme::Unknown) app.setSystemTheme({.scheme = cs});
    app.setEventSource({st.get(), &State::pump});
    return std::unique_ptr<WaylandPlatform>(new WaylandPlatform(std::move(st)));
}

WaylandPlatform::~WaylandPlatform() {
    // detach every shown window (restores its headless presenter) before the connection / instance go away
    while (!m->recs.empty()) m->recs.back()->win->setPresenter({});
    vkCtxDestroy(m->vk);
    wl::close(m->b);
    m->app.setEventSource({});
}

PresenterKind WaylandPlatform::show(AppWindow& w) {
    if (const State::Rec* existing = m->find(w)) return existing->kind;
    if (m->lost) return PresenterKind::Headless;
    auto* r = new State::Rec{};
    r->st = m.get();
    r->win = &w;
    r->wl = wl::createWindow(m->b, w, m->error);
    if (!r->wl) {
        delete r;
        return PresenterKind::Headless;
    }
    m->recs.push_back(r);
    if (VkCtx* c = m->vkContext()) {
        SurfaceSource s;
        s.kind = SurfaceSource::Wayland;
        s.wlDisplay = wl::display(m->b);
        s.wlSurface = wl::surface(r->wl);
        r->vk = vkWinCreate(c, s, m->opts, m->error);
    }
    r->kind = r->vk ? PresenterKind::Vulkan : PresenterKind::Software; // wl_shm (XRGB8888) is always there
    // No server-side frame (GNOME, weston): the window would have no title bar and could not be moved or resized. It
    // gets the client-side chrome instead: resize edges and, unless the app made its own, a title bar on top of the
    // root (inside the window's size).
    if (m->opts.clientDecorations && w.chrome().frame == WindowFrame::System && !wl::serverDecorated(r->wl)) {
        WindowChrome& c = w.chrome();
        UiContext& ui = w.ui();
        r->csd = true;
        c.frame = WindowFrame::Custom;
        c.resizeBorder = 6.f; // WindowDesc::resizeBorder's default
        if (!ui.alive(c.caption)) {
            r->csdBar = titleBar(w, ui.root(), w.title());
            ui.setCss(r->csdBar, "flex-shrink", "0");
            if (const NodeHandle first = ui.firstChild(ui.root()); first != r->csdBar) ui.moveTo(r->csdBar, ui.root(), first);
        }
    }
    // popups may extend past the window; the compositor keeps them on screen (xdg_positioner), so the UI does not clamp
    if (m->opts.osPopups) w.ui().setOsPopups(true, {-16384.f, -16384.f, 32768.f, 32768.f});
    if (m->clipboardUsable()) w.ui().setClipboard({&State::clipSet, &State::clipGet, m.get()});
    // a FIFO swapchain waits for the display; wl_shm frames are held back by wl_surface.frame without blocking, so
    // AppWindow::setMaxFrameRate applies to them
    w.setPresenter({r, &State::present, &State::destroy, r->vk != nullptr && m->opts.vsync});
    w.setWindowBackend({r, &State::windowRequest});
    // what the app asked for before the window existed
    if (w.minimumSize().w > 0.f || w.minimumSize().h > 0.f || w.maximumSize().w > 0.f || w.maximumSize().h > 0.f)
        State::windowRequest(r, w, WindowRequest::SizeLimits);
    if (w.requestedMode() != WindowMode::Normal) State::windowRequest(r, w, WindowRequest::Mode);
    if (!w.requestedVisible()) State::windowRequest(r, w, WindowRequest::Visible);
    return r->kind;
}

PresenterKind WaylandPlatform::kind(const AppWindow& w) const {
    const State::Rec* r = m->find(w);
    return r ? r->kind : PresenterKind::Headless;
}
std::string_view WaylandPlatform::gpuName() const { return vkCtxDeviceName(m->vk); }
bool WaylandPlatform::gpuDraws(const AppWindow& w) const {
    const State::Rec* r = m->find(w);
    return r && vkWinGpuDraws(r->vk);
}
std::string_view WaylandPlatform::lastError() const { return m->error; }
bool WaylandPlatform::setExternalTexture(const AppWindow& w, unsigned slot, const ExternalTexture& tex) {
    const State::Rec* r = m->find(w);
    return r && vkWinSetExternal(r->vk, slot, tex);
}
bool WaylandPlatform::capture(const AppWindow& w, Image& out) {
    const State::Rec* r = m->find(w);
    if (!r) return false;
    return r->vk ? vkWinReadback(r->vk, out) : wl::capture(r->wl, out);
}
void* WaylandPlatform::nativeDisplay() const { return wl::display(m->b); }
unsigned long long WaylandPlatform::nativeWindow(const AppWindow& w) const {
    const State::Rec* r = m->find(w);
    return r ? reinterpret_cast<std::uintptr_t>(wl::surface(r->wl)) : 0;
}
bool WaylandPlatform::setIcon(const AppWindow& w, std::span<const Image> sizes) {
    const State::Rec* r = m->find(w);
    if (!r) return false;
    std::vector<const Image*> v;
    for (const Image& img : sizes) v.push_back(&img);
    return wl::setIcon(m->b, r->wl, v);
}
bool WaylandPlatform::resizeWindow(const AppWindow& w, unsigned width, unsigned height) {
    const State::Rec* r = m->find(w);
    if (!r) return false;
    wl::resize(m->b, r->wl, width, height); // the client sizes its toplevel
    return true;
}
std::vector<int> WaylandPlatform::iconSizes(const AppWindow& w) const {
    const State::Rec* r = m->find(w);
    return r ? wl::iconSizes(r->wl) : std::vector<int>{}; // what was handed over (the protocol has no read-back)
}
std::size_t WaylandPlatform::popupCount(const AppWindow& w) const {
    const State::Rec* r = m->find(w);
    return r ? r->popups.size() : 0;
}
unsigned long long WaylandPlatform::popupWindow(const AppWindow& w, std::size_t i) const {
    const State::Rec* r = m->find(w);
    return r && i < r->popups.size() ? reinterpret_cast<std::uintptr_t>(r->popups[i].child) : 0;
}
Rect WaylandPlatform::popupScreenRect(const AppWindow& w, std::size_t i) const {
    const State::Rec* r = m->find(w);
    if (!r || i >= r->popups.size()) return {};
    const Rect pl = wl::childPlacement(r->popups[i].child); // where the compositor put it, window-relative
    const f32 s = r->win->scale();
    return {std::round(pl.x * s), std::round(pl.y * s), r->popups[i].pixels.w, r->popups[i].pixels.h};
}
bool WaylandPlatform::capturePopup(const AppWindow& w, std::size_t i, Image& out) {
    const State::Rec* r = m->find(w);
    return r && i < r->popups.size() && wl::captureChild(r->popups[i].child, out);
}
float WaylandPlatform::scale(const AppWindow& w) const { return m->find(w) ? w.scale() : 1.f; }
Cursor WaylandPlatform::cursor(const AppWindow& w) const {
    const State::Rec* r = m->find(w);
    return r ? wl::cursorShown(r->wl) : Cursor::Arrow;
}
bool WaylandPlatform::setClipboardText(std::string_view utf8) { return m->setText(false, utf8); }
bool WaylandPlatform::clipboardText(std::string& out) { return m->text(false, out); }
bool WaylandPlatform::setClipboard(std::vector<ClipboardItem> items) { return m->own(false, std::move(items)); }
bool WaylandPlatform::clipboardData(std::string_view mime, std::vector<u8>& out) { return m->read(false, mime, out); }
std::vector<std::string> WaylandPlatform::clipboardFormats() {
    std::vector<std::string> out;
    if (!m->clipboardUsable()) return out;
    for (std::string name : wl::selectionMimes(m->b, false)) {
        if (State::isTextMime(name) || name.starts_with("text/plain;")) name = kClipboardText;
        if (name.find('/') == std::string::npos) continue; // TARGETS, SAVE_TARGETS ... of X11 clients behind XWayland
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
    }
    return out;
}
bool WaylandPlatform::clipboardOwned() const { return !m->lost && wl::ownsSelection(m->b, false); }
bool WaylandPlatform::setPrimarySelectionText(std::string_view utf8) { return m->setText(true, utf8); }
bool WaylandPlatform::primarySelectionText(std::string& out) { return m->text(true, out); }
int WaylandPlatform::cursorThemeSize() const { return m->lost ? 0 : wl::cursorThemeSize(m->b); }
bool WaylandPlatform::clientDecorated(const AppWindow& w) const {
    const State::Rec* r = m->find(w);
    return r && r->csd;
}
bool WaylandPlatform::postInput(unsigned long long h, Vec2 at, int kind) {
    if (m->lost) return false;
    for (State::Rec* r : m->recs) {
        if (reinterpret_cast<std::uintptr_t>(wl::surface(r->wl)) == h) {
            if (kind == 3) wl::injectClose(r->wl);
            else wl::injectPointer(m->b, r->wl, at, kind);
            r->win->requestRedraw();
            return true;
        }
        for (State::Rec::Popup& p : r->popups)
            if (reinterpret_cast<std::uintptr_t>(p.child) == h && kind != 3) {
                wl::injectChildPointer(m->b, p.child, at, kind);
                return true;
            }
    }
    return false;
}
bool WaylandPlatform::postClick(unsigned long long h, Vec2 at) {
    for (State::Rec* r : m->recs) {
        if (reinterpret_cast<std::uintptr_t>(wl::surface(r->wl)) == h) {
            wl::injectPointer(m->b, r->wl, at, 0);
            wl::injectPointer(m->b, r->wl, at, 1);
            wl::injectPointer(m->b, r->wl, at, 2);
            r->win->requestRedraw();
            return true;
        }
        for (State::Rec::Popup& p : r->popups)
            if (reinterpret_cast<std::uintptr_t>(p.child) == h) { // its input path (context coordinates via its origin)
                wl::injectChildPointer(m->b, p.child, at, 0);
                wl::injectChildPointer(m->b, p.child, at, 1);
                wl::injectChildPointer(m->b, p.child, at, 2);
                return true;
            }
    }
    return false;
}
std::vector<DisplayInfo> WaylandPlatform::displays() const { return wl::displays(m->b); }

} // namespace fuse::app::detail
#endif
