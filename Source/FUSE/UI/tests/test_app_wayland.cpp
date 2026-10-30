// fuse_app_wayland: the Wayland backend against a minimal in-process compositor built on a runtime-loaded
// libwayland-server (wl_compositor, wl_shm, xdg_wm_base, wl_seat pointer, wl_data_device_manager). Checks:
//   * registry / xdg-shell handshake: toplevel configured, title set, configure size -> AppWindow::resize
//   * wl_shm presentation: the committed buffer equals the CPU raster (format swizzle included)
//   * pointer enter / motion / button reach the UI (a button widget is clicked)
//   * drop in: a compositor-made wl_data_offer (text/uri-list) is accepted, received over a pipe and delivered
//     to AppWindow::onExternalDrop with decoded paths
//   * drag out: AppWindow::beginExternalDrag -> wl_data_device.start_drag; the compositor pulls the payload with
//     wl_data_source.send and finishes -> onExternalDragEnd(true)
//   * multi-format drag out with a pixmap: every format offered (custom MIME pulled back), the icon surface given to
//     start_drag carries a buffer of the pixmap's size
//   * cursor-shape-v1: UiContext::setCursorShape -> wp_cursor_shape_device_v1.set_shape with the enter serial
//   * keyboard + client-side key repeat: an XKB keymap (built with libxkbcommon), repeat_info, one held key types
//     several characters into a focused lineedit and stops on release
//   * text-input-v3 IME: enter -> enable + set_cursor_rectangle + commit; preedit_string/done -> composition;
//     commit_string/done -> committed text
//   * xdg_popup: showPopup sends the positioner (size, anchor rect), the popup configure size wins, popup_done
//     closes the popup window
//   * HiDPI: wp_fractional_scale_v1.preferred_scale (1.5x, 2x) -> AppWindow::setScale, a physical-size buffer
//     (540x360 for a 360x240 window) and wp_viewport.set_destination(360, 240)
//   * --vulkan: the same run with a Vulkan swapchain on the wl_surface (VK_KHR_wayland_surface, CPU-raster upload):
//     the buffers the WSI commits (wl_shm with lavapipe / swiftshader) must equal the CPU raster pixel for pixel,
//     and every other check still holds. --vulkan-gpu: the same with the ui.draw_list GPU kernel drawing (committed
//     buffers == swapchain readback). Exit 77 there when no ICD offers VK_KHR_wayland_surface.
// Exit 77 when libwayland-server / -client are not installed.
#include "fuse/app/wayland.hpp"
#include "fuse/ui/os.hpp"
#include "../App/src/wl_protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <memory>
#include <thread>
#include <unistd.h>

using namespace fuse::ui;
namespace app = fuse::app;
namespace wl = fuse::app::wl;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
struct D; struct C; struct R; struct G; struct Loop; struct ShmBuf;
using Fn = void (*)();
struct Server {
    std::unique_ptr<os::Library> so;
    D* (*create)() = nullptr;
    const char* (*addSocketAuto)(D*) = nullptr;
    Loop* (*getLoop)(D*) = nullptr;
    int (*loopDispatch)(Loop*, int) = nullptr;
    void (*flushClients)(D*) = nullptr;
    void (*destroyDisplay)(D*) = nullptr;
    int (*initShm)(D*) = nullptr;
    uint32_t (*nextSerial)(D*) = nullptr;
    G* (*globalCreate)(D*, const wl::Interface*, int, void*, void (*)(C*, void*, uint32_t, uint32_t)) = nullptr;
    R* (*resCreate)(C*, const wl::Interface*, int, uint32_t) = nullptr;
    void (*setImpl)(R*, const void*, void*, void (*)(R*)) = nullptr;
    void (*post)(R*, uint32_t, ...) = nullptr;
    void* (*userData)(R*) = nullptr;
    void (*resDestroy)(R*) = nullptr;
    C* (*resClient)(R*) = nullptr;
    int (*resVersion)(R*) = nullptr;
    ShmBuf* (*shmGet)(R*) = nullptr;
    void* (*shmData)(ShmBuf*) = nullptr;
    int32_t (*shmW)(ShmBuf*) = nullptr;
    int32_t (*shmH)(ShmBuf*) = nullptr;
    int32_t (*shmStride)(ShmBuf*) = nullptr;
    const wl::Interface *compositor, *surface, *region, *seat, *pointer, *keyboard, *output, *callback, *ddm, *device, *offer, *source;
    wl::XdgTables xdg;
    bool load() {
        so = os::loadLibrary({"libwayland-server.so.0", "libwayland-server.so"});
        if (!so) return false;
        bool all = true;
        auto f = [&](auto& fn, const char* n) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(so->symbol(n)); all = all && fn; };
        auto i = [&](const wl::Interface*& p, const char* n) { p = static_cast<const wl::Interface*>(so->symbol(n)); all = all && p; };
        f(create, "wl_display_create"); f(addSocketAuto, "wl_display_add_socket_auto"); f(getLoop, "wl_display_get_event_loop");
        f(loopDispatch, "wl_event_loop_dispatch"); f(flushClients, "wl_display_flush_clients"); f(destroyDisplay, "wl_display_destroy");
        f(initShm, "wl_display_init_shm"); f(nextSerial, "wl_display_next_serial"); f(globalCreate, "wl_global_create");
        f(resCreate, "wl_resource_create"); f(setImpl, "wl_resource_set_implementation"); f(post, "wl_resource_post_event");
        f(userData, "wl_resource_get_user_data"); f(resDestroy, "wl_resource_destroy"); f(resClient, "wl_resource_get_client");
        f(resVersion, "wl_resource_get_version"); f(shmGet, "wl_shm_buffer_get"); f(shmData, "wl_shm_buffer_get_data");
        f(shmW, "wl_shm_buffer_get_width"); f(shmH, "wl_shm_buffer_get_height"); f(shmStride, "wl_shm_buffer_get_stride");
        i(compositor, "wl_compositor_interface"); i(surface, "wl_surface_interface"); i(region, "wl_region_interface");
        i(seat, "wl_seat_interface"); i(pointer, "wl_pointer_interface"); i(keyboard, "wl_keyboard_interface"); i(output, "wl_output_interface");
        i(callback, "wl_callback_interface"); i(ddm, "wl_data_device_manager_interface"); i(device, "wl_data_device_interface");
        i(offer, "wl_data_offer_interface"); i(source, "wl_data_source_interface");
        if (all) xdg.init(surface, seat, output);
        return all;
    }
};

// ---- compositor state (touched only on the compositor thread, read under `mu`) ----
Server S;
D* g_dpy = nullptr;
std::mutex mu;
std::deque<std::function<void()>> cmds;
std::atomic<bool> stop{false};
R *g_surface = nullptr, *g_xsurf = nullptr, *g_top = nullptr, *g_pointer = nullptr, *g_device = nullptr, *g_pending = nullptr;
R* g_dragSource = nullptr;
R* g_dragIcon = nullptr;
R *g_keyboard = nullptr, *g_ti = nullptr, *g_surface2 = nullptr, *g_popup = nullptr, *g_popXsurf = nullptr;
int g_tiEnable = 0, g_tiDisable = 0, g_tiCommit = 0, g_tiCursorH = 0, g_posW = 0, g_posH = 0, g_anchorW = 0;
bool g_popCfg = false, g_popDestroyed = false;
std::vector<std::string> g_sourceMimes;
bool g_configured = false, g_acked = false;
std::string g_title, g_accepted, g_pulled;
int g_commits = 0, g_cfgW = 360, g_cfgH = 240;
Image g_shown;
const char* g_dropPayload = "file:///tmp/fuse%20wl/a.png\r\nfile:///tmp/b.txt\r\n";

void noop() {}
void destroyReq(C*, R* r) { S.resDestroy(r); }
Fn noops[16] = {noop, noop, noop, noop, noop, noop, noop, noop, noop, noop, noop, noop, noop, noop, noop, noop};
Fn destroyFirst[16];
R* make(C* c, const wl::Interface* i, int v, uint32_t id, const void* impl, void* data = nullptr) {
    R* r = S.resCreate(c, i, v, id);
    S.setImpl(r, impl, data, nullptr);
    return r;
}

// wl_surface
struct Attach { int w = 0, h = 0, dx = 0; };
std::map<R*, Attach> g_attached; // every non-toplevel surface's last attach (drag icons are attached before start_drag)
void sAttach(C*, R* r, R* buffer, int32_t dx, int32_t) {
    if (r != g_surface && r != g_surface2 && buffer) {
        if (ShmBuf* b = S.shmGet(buffer)) { std::lock_guard lk(mu); g_attached[r] = {S.shmW(b), S.shmH(b), dx}; }
        return;
    }
    g_pending = buffer;
}
void sFrame(C* c, R* r, uint32_t id) { R* cb = make(c, S.callback, 1, id, noops); S.post(cb, 0, 0u); S.resDestroy(cb); (void)r; }
void sCommit(C*, R* r) {
    ++g_commits;
    if (g_pending) {
        if (ShmBuf* b = S.shmGet(g_pending)) {
            std::lock_guard lk(mu);
            const int w = S.shmW(b), h = S.shmH(b), st = S.shmStride(b);
            g_shown.resize(w, h, 0);
            const auto* src = static_cast<const uint8_t*>(S.shmData(b));
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    uint32_t p;
                    std::memcpy(&p, src + y * st + x * 4, 4); // XRGB -> 0xAABBGGRR
                    g_shown.pixels[std::size_t(y * w + x)] = 0xFF000000u | ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
                }
        }
        S.post(g_pending, 0); // release
        g_pending = nullptr;
    }
    if (g_popup && !g_popCfg && r == g_surface2) {
        g_popCfg = true;
        S.post(g_popup, 0, 10, 30, 100, 70);
        S.post(g_popXsurf, 0, S.nextSerial(g_dpy));
    }
    if (g_top && !g_configured && r == g_surface) {
        g_configured = true;
        wl::Array states{0, 0, nullptr};
        S.post(g_top, 0, g_cfgW, g_cfgH, &states);
        S.post(g_xsurf, 0, S.nextSerial(g_dpy));
    }
}
Fn surfaceImpl[11];
void cCreateSurface(C* c, R* r, uint32_t id) {
    R* s = make(c, S.surface, S.resVersion(r), id, surfaceImpl);
    if (!g_surface) g_surface = s; // the popup's surface is picked up by get_xdg_surface
}
void cCreateRegion(C* c, R*, uint32_t id) { make(c, S.region, 1, id, destroyFirst); }
Fn compositorImpl[2] = {reinterpret_cast<Fn>(&cCreateSurface), reinterpret_cast<Fn>(&cCreateRegion)};
void bindCompositor(C* c, void*, uint32_t v, uint32_t id) { make(c, S.compositor, int(v), id, compositorImpl); }
// xdg
void topTitle(C*, R*, const char* t) { std::lock_guard lk(mu); g_title = t; }
Fn topImpl[14];
void xsGetToplevel(C* c, R*, uint32_t id) { g_top = make(c, &S.xdg.toplevel, 1, id, topImpl); }
void xsAck(C*, R*, uint32_t) { std::lock_guard lk(mu); g_acked = true; }
Fn xsurfImpl[5];
void wmGetXdgSurface(C* c, R*, uint32_t id, R* surf) {
    R* x = make(c, &S.xdg.surface, 1, id, xsurfImpl);
    (surf == g_surface ? g_xsurf : g_popXsurf) = x;
    if (surf != g_surface) g_surface2 = surf;
}
void posSize(C*, R*, int32_t w, int32_t h) { g_posW = w; g_posH = h; }
void posAnchor(C*, R*, int32_t, int32_t, int32_t w, int32_t) { g_anchorW = w; }
Fn posImpl[7] = {reinterpret_cast<Fn>(&destroyReq), reinterpret_cast<Fn>(&posSize), reinterpret_cast<Fn>(&posAnchor), noop, noop, noop, noop};
void wmCreatePositioner(C* c, R*, uint32_t id) { make(c, &S.xdg.positioner, 1, id, posImpl); }
void popDestroy(C*, R* r) { g_popDestroyed = true; g_popup = nullptr; S.resDestroy(r); }
Fn popImpl[2] = {reinterpret_cast<Fn>(&popDestroy), noop};
void xsGetPopup(C* c, R*, uint32_t id, R*, R*) { g_popup = make(c, &S.xdg.popup, 1, id, popImpl); }
Fn wmImpl[4] = {reinterpret_cast<Fn>(&destroyReq), reinterpret_cast<Fn>(&wmCreatePositioner), reinterpret_cast<Fn>(&wmGetXdgSurface), noop};
void bindWm(C* c, void*, uint32_t, uint32_t id) { make(c, &S.xdg.wmBase, 1, id, wmImpl); }
// seat
std::string g_keymapText; // XKB keymap (libxkbcommon "us" default), sent with wl_keyboard.keymap
void seatGetPointer(C* c, R* r, uint32_t id) { g_pointer = make(c, S.pointer, S.resVersion(r), id, noops); }
void seatGetKeyboard(C* c, R* r, uint32_t id) {
    g_keyboard = make(c, S.keyboard, S.resVersion(r), id, noops);
    if (auto mem = os::createSharedBuffer(g_keymapText.size() + 1)) {
        std::memcpy(mem->data, g_keymapText.c_str(), g_keymapText.size() + 1);
        S.post(g_keyboard, 0, 1u, mem->fd, uint32_t(g_keymapText.size() + 1)); // xkb_v1; the fd is dup'ed
    }
    S.post(g_keyboard, 5, 40, 60); // repeat_info: 40 Hz after 60 ms
}
Fn seatImpl[4] = {reinterpret_cast<Fn>(&seatGetPointer), reinterpret_cast<Fn>(&seatGetKeyboard), noop, noop};
void bindSeat(C* c, void*, uint32_t v, uint32_t id) {
    R* r = make(c, S.seat, int(v), id, seatImpl);
    S.post(r, 0, g_keymapText.empty() ? 1u : 3u); // capabilities: pointer (+ keyboard when a keymap exists)
}
// text-input-v3
wl::TextInputTables TI;
// viewporter + fractional-scale-v1
wl::ScaleTables SC;
R *g_frac = nullptr;
int g_destW = 0, g_destH = 0;
void vpDest(C*, R* r, int32_t w, int32_t h) { if (S.userData(r) == g_surface) { std::lock_guard lk(mu); g_destW = w; g_destH = h; } }
Fn vpImpl[3] = {reinterpret_cast<Fn>(&destroyReq), noop, reinterpret_cast<Fn>(&vpDest)};
void vpGet(C* c, R*, uint32_t id, R* surf) { make(c, &SC.viewport, 1, id, vpImpl, surf); }
Fn vpMgrImpl[2] = {reinterpret_cast<Fn>(&destroyReq), reinterpret_cast<Fn>(&vpGet)};
void bindViewporter(C* c, void*, uint32_t, uint32_t id) { make(c, &SC.viewporter, 1, id, vpMgrImpl); }
void fracGet(C* c, R*, uint32_t id, R* surf) {
    R* f = make(c, &SC.frac, 1, id, destroyFirst);
    if (surf == g_surface) g_frac = f;
}
Fn fracMgrImpl[2] = {reinterpret_cast<Fn>(&destroyReq), reinterpret_cast<Fn>(&fracGet)};
void bindFracMgr(C* c, void*, uint32_t, uint32_t id) { make(c, &SC.fracManager, 1, id, fracMgrImpl); }
void tiEnable(C*, R*) { ++g_tiEnable; }
void tiDisable(C*, R*) { ++g_tiDisable; }
void tiCursor(C*, R*, int32_t, int32_t, int32_t, int32_t h) { g_tiCursorH = h; }
void tiCommitReq(C*, R*) { ++g_tiCommit; }
Fn tiImpl[8] = {reinterpret_cast<Fn>(&destroyReq), reinterpret_cast<Fn>(&tiEnable), reinterpret_cast<Fn>(&tiDisable),
                noop, noop, noop, reinterpret_cast<Fn>(&tiCursor), reinterpret_cast<Fn>(&tiCommitReq)};
void tiMgrGet(C* c, R*, uint32_t id, R*) { g_ti = make(c, &TI.input, 1, id, tiImpl); }
Fn tiMgrImpl[2] = {reinterpret_cast<Fn>(&destroyReq), reinterpret_cast<Fn>(&tiMgrGet)};
void bindTiMgr(C* c, void*, uint32_t, uint32_t id) { make(c, &TI.manager, 1, id, tiMgrImpl); }
bool makeKeymap() {
    auto so = os::loadLibrary({"libxkbcommon.so.0", "libxkbcommon.so"});
    if (!so) return false;
    auto ctxNew = reinterpret_cast<void* (*)(int)>(so->symbol("xkb_context_new"));
    auto fromNames = reinterpret_cast<void* (*)(void*, const void*, int)>(so->symbol("xkb_keymap_new_from_names"));
    auto asString = reinterpret_cast<char* (*)(void*, int)>(so->symbol("xkb_keymap_get_as_string"));
    if (!ctxNew || !fromNames || !asString) return false;
    void* ctx = ctxNew(0);
    void* km = ctx ? fromNames(ctx, nullptr, 0) : nullptr;
    char* text = km ? asString(km, 1) : nullptr;
    if (!text) return false;
    g_keymapText = text;
    std::free(text); // keymap / context leak: test process
    return true;
}
// cursor-shape-v1
wl::CursorShapeTables CS;
std::vector<uint32_t> g_shapes;
uint32_t g_enterSerial = 0, g_shapeSerialBad = 0;
void shapeSet(C*, R*, uint32_t serial, uint32_t shape) {
    std::lock_guard lk(mu);
    g_shapes.push_back(shape);
    if (serial != g_enterSerial) ++g_shapeSerialBad;
}
Fn shapeDevImpl[2] = {reinterpret_cast<Fn>(&destroyReq), reinterpret_cast<Fn>(&shapeSet)};
void shapeGetPointer(C* c, R*, uint32_t id, R*) { make(c, &CS.device, 1, id, shapeDevImpl); }
Fn shapeMgrImpl[2] = {reinterpret_cast<Fn>(&destroyReq), reinterpret_cast<Fn>(&shapeGetPointer)};
void bindShapeMgr(C* c, void*, uint32_t, uint32_t id) { make(c, &CS.manager, 1, id, shapeMgrImpl); }
// data device
void srcOffer(C*, R*, const char* mime) { std::lock_guard lk(mu); g_sourceMimes.emplace_back(mime); }
Fn sourceImpl[3] = {reinterpret_cast<Fn>(&srcOffer), reinterpret_cast<Fn>(&destroyReq), noop};
void ddmCreateSource(C* c, R* r, uint32_t id) { make(c, S.source, S.resVersion(r), id, sourceImpl); }
void devStartDrag(C*, R*, R* source, R*, R* icon, uint32_t) { std::lock_guard lk(mu); g_dragSource = source; g_dragIcon = icon; }
Fn deviceImpl[3] = {reinterpret_cast<Fn>(&devStartDrag), noop, reinterpret_cast<Fn>(&destroyReq)};
void ddmGetDevice(C* c, R* r, uint32_t id, R*) { g_device = make(c, S.device, S.resVersion(r), id, deviceImpl); }
Fn ddmImpl[2] = {reinterpret_cast<Fn>(&ddmCreateSource), reinterpret_cast<Fn>(&ddmGetDevice)};
void bindDdm(C* c, void*, uint32_t v, uint32_t id) { make(c, S.ddm, int(v), id, ddmImpl); }
void offerAccept(C*, R*, uint32_t, const char* mime) { std::lock_guard lk(mu); g_accepted = mime ? mime : ""; }
void offerReceive(C*, R*, const char*, int32_t fd) { os::writeAllAndClose(fd, g_dropPayload); }
Fn offerImpl[5] = {reinterpret_cast<Fn>(&offerAccept), reinterpret_cast<Fn>(&offerReceive), reinterpret_cast<Fn>(&destroyReq), noop, noop};

void compositorThread() {
    Loop* loop = S.getLoop(g_dpy);
    while (!stop) {
        S.loopDispatch(loop, 5);
        std::deque<std::function<void()>> todo;
        { std::lock_guard lk(mu); todo.swap(cmds); }
        for (auto& f : todo) f();
        S.flushClients(g_dpy);
    }
}
void run(std::function<void()> f) { std::lock_guard lk(mu); cmds.push_back(std::move(f)); }
} // namespace

int main(int argc, char** argv) {
    const bool gpuDraw = argc > 1 && std::strcmp(argv[1], "--vulkan-gpu") == 0;
    const bool vulkan = gpuDraw || (argc > 1 && std::strcmp(argv[1], "--vulkan") == 0);
    if (!S.load()) { std::printf("fuse_app_wayland: skipped (no libwayland-server)\n"); return 77; }
    for (int i = 0; i < 16; ++i) destroyFirst[i] = noop;
    destroyFirst[0] = reinterpret_cast<Fn>(&destroyReq);
    for (auto& f : surfaceImpl) f = noop;
    surfaceImpl[0] = reinterpret_cast<Fn>(&destroyReq); surfaceImpl[1] = reinterpret_cast<Fn>(&sAttach);
    surfaceImpl[3] = reinterpret_cast<Fn>(&sFrame); surfaceImpl[6] = reinterpret_cast<Fn>(&sCommit);
    for (auto& f : topImpl) f = noop;
    topImpl[0] = reinterpret_cast<Fn>(&destroyReq); topImpl[2] = reinterpret_cast<Fn>(&topTitle);
    for (auto& f : xsurfImpl) f = noop;
    xsurfImpl[0] = reinterpret_cast<Fn>(&destroyReq); xsurfImpl[1] = reinterpret_cast<Fn>(&xsGetToplevel);
    xsurfImpl[2] = reinterpret_cast<Fn>(&xsGetPopup);
    xsurfImpl[4] = reinterpret_cast<Fn>(&xsAck);
    TI.init(S.surface, S.seat);
    SC.init(S.surface);
    CS.init(S.pointer);
    const bool haveKeymap = makeKeymap();
    if (!haveKeymap) std::printf("fuse_app_wayland: no libxkbcommon, keyboard checks skipped\n");

    const std::string runtime = os::runtimeDir();
    if (os::env("XDG_RUNTIME_DIR").empty()) ::setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
    g_dpy = S.create();
    const char* sock = S.addSocketAuto(g_dpy);
    if (!sock) { std::printf("fuse_app_wayland: skipped (cannot create socket)\n"); return 77; }
    S.initShm(g_dpy);
    S.globalCreate(g_dpy, S.compositor, 4, nullptr, &bindCompositor);
    S.globalCreate(g_dpy, &S.xdg.wmBase, 1, nullptr, &bindWm);
    S.globalCreate(g_dpy, S.seat, 5, nullptr, &bindSeat);
    S.globalCreate(g_dpy, S.ddm, 3, nullptr, &bindDdm);
    S.globalCreate(g_dpy, &TI.manager, 1, nullptr, &bindTiMgr);
    S.globalCreate(g_dpy, &SC.viewporter, 1, nullptr, &bindViewporter);
    S.globalCreate(g_dpy, &SC.fracManager, 1, nullptr, &bindFracMgr);
    S.globalCreate(g_dpy, &CS.manager, 1, nullptr, &bindShapeMgr);
    std::thread th(compositorThread);

    {
        app::Application a{{.name = "gate_wayland", .loadSettings = false}};
        app::NativeOptions no;
        no.vulkan = vulkan;
        no.vsync = false;
        no.drawMode = gpuDraw ? app::VkDrawMode::GpuDraw : app::VkDrawMode::CpuUpload;
        app::WaylandPlatform wlp(a, sock, no);
        CHECK(wlp.available());
        if (!wlp.available()) std::fprintf(stderr, "error: %.*s\n", int(wlp.lastError().size()), wlp.lastError().data());
        auto& win = a.createWindow({.title = "fuse wayland", .width = 300, .height = 200});
        std::vector<std::string> dropped;
        int dragEnd = -1;
        win.onExternalDrop([&](app::AppWindow&, const app::AppWindow::ExternalDrop& d) { dropped = d.paths; });
        win.onExternalDragEnd([&](bool ok) { dragEnd = ok ? 1 : 0; });
        CHECK(wlp.show(win));
        auto pump = [&](int n) { for (int i = 0; i < n; ++i) { a.step(0.005f); wlp.roundtrip(); } };
        pump(20);
        CHECK(wlp.configured(win));
        if (vulkan && !wlp.gpuPresents(win)) {
            std::printf("fuse_app_wayland --vulkan: skipped (%.*s)\n", int(wlp.lastError().size()), wlp.lastError().data());
            stop = true;
            th.join();
            return 77;
        }
        if (gpuDraw && !wlp.gpuDraws(win)) {
            std::printf("fuse_app_wayland --vulkan-gpu: skipped (no ui.draw_list kernel)\n");
            stop = true;
            th.join();
            return 77;
        }
        if (vulkan)
            std::printf("fuse_app_wayland: Vulkan present (%s) on %.*s\n", gpuDraw ? "GPU draw" : "CPU upload",
                        int(wlp.gpuName().size()), wlp.gpuName().data());
        CHECK(u32(win.size().w) == 360u && u32(win.size().h) == 240u); // compositor's configure size won
        {
            std::lock_guard lk(mu);
            CHECK(g_title == "fuse wayland");
        }
        win.setTitle("renamed \xE2\x9C\x93"); // live retitle -> xdg_toplevel.set_title
        for (int i = 0; i < 50; ++i) { pump(1); std::lock_guard lk(mu); if (g_title == win.title()) break; }
        {
            std::lock_guard lk(mu);
            CHECK(g_title == "renamed \xE2\x9C\x93");
            CHECK(g_acked);
            CHECK(g_shown.width == 360 && g_shown.height == 240);
        }
        Image mine;
        CHECK(wlp.capture(win, mine));
        { std::lock_guard lk(mu); CHECK(mine.width == g_shown.width && mine.pixels == g_shown.pixels); }

        // pointer enter + motion (gives the client an input serial for start_drag)
        run([&] {
            { std::lock_guard lk(mu); g_enterSerial = S.nextSerial(g_dpy); }
            S.post(g_pointer, 0, g_enterSerial, g_surface, wl::toFixed(40), wl::toFixed(40));
            S.post(g_pointer, 2, 1u, wl::toFixed(41), wl::toFixed(41));
        });
        pump(10);
        // cursor-shape-v1: the enter sets the default shape, a UI change sends the new one
        { std::lock_guard lk(mu); CHECK(!g_shapes.empty() && g_shapes.front() == 1u); }
        win.ui().setCursorShape(CursorShape::Copy);
        win.requestRedraw();
        for (int i = 0; i < 50; ++i) { pump(1); std::lock_guard lk(mu); if (!g_shapes.empty() && g_shapes.back() == 12u) break; }
        win.ui().setCursorShape(CursorShape::Default);
        win.requestRedraw();
        for (int i = 0; i < 50; ++i) { pump(1); std::lock_guard lk(mu); if (!g_shapes.empty() && g_shapes.back() == 1u && g_shapes.size() >= 3) break; }
        {
            std::lock_guard lk(mu);
            CHECK(g_shapes.size() >= 3 && g_shapes[g_shapes.size() - 2] == 12u && g_shapes.back() == 1u);
            CHECK(g_shapeSerialBad == 0);
        }
        const int clicks = 0;
        // drop in
        run([&] {
            C* c = S.resClient(g_device);
            R* o = make(c, S.offer, S.resVersion(g_device), 0, offerImpl);
            S.post(g_device, 0, o);
            S.post(o, 0, "text/uri-list");
            S.post(g_device, 1, S.nextSerial(g_dpy), g_surface, wl::toFixed(50), wl::toFixed(60), o);
            S.post(g_device, 4);
        });
        for (int i = 0; i < 100 && dropped.empty(); ++i) pump(1);
        { std::lock_guard lk(mu); CHECK(g_accepted == "text/uri-list"); }
        CHECK(dropped.size() == 2 && dropped[0] == "/tmp/fuse wl/a.png" && dropped[1] == "/tmp/b.txt");

        // drag out
        CHECK(win.beginExternalDrag({.paths = {"/srv/one two.txt"}}));
        run([&] {
            S.post(g_pointer, 3, S.nextSerial(g_dpy), 4u, 0x110u, 1u);
            S.post(g_pointer, 2, 5u, wl::toFixed(20), wl::toFixed(20));
        });
        R* src = nullptr;
        for (int i = 0; i < 100 && !src; ++i) { pump(1); std::lock_guard lk(mu); src = g_dragSource; }
        CHECK(src != nullptr);
        if (src) {
            // The reader owns its state through a shared_ptr and is joined when done: it used to be detached with a
            // reference to a stack flag and wrote g_pulled unlocked, so when the 200-pump wait ran out under ctest -jN
            // load it wrote into a dead frame (SegFault seen at -j32). Wait on a wall-clock deadline instead.
            struct Pull { std::atomic<bool> done{false}; std::string data; };
            auto pull = std::make_shared<Pull>();
            std::thread reader;
            run([&] {
                auto p = os::makePipe();
                S.post(src, 1, "text/uri-list", p->second);
                os::closeFd(p->second);
                S.flushClients(g_dpy);
                reader = std::thread([fd = p->first, pull] { pull->data = os::readAllAndClose(fd); pull->done = true; });
            });
            const auto pullEnd = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (!pull->done && std::chrono::steady_clock::now() < pullEnd) pump(1);
            if (pull->done) { reader.join(); std::lock_guard lk(mu); g_pulled = pull->data; }
            else reader.detach(); // owns `pull`; the CHECK below reports the failure
            run([&] { S.post(src, 3); S.post(src, 4); });
            for (int i = 0; i < 100 && dragEnd < 0; ++i) pump(1);
            std::lock_guard lk(mu);
            CHECK(g_pulled == "file:///srv/one%20two.txt\r\n");
            CHECK(!g_sourceMimes.empty() && g_sourceMimes[0] == "text/uri-list");
        }
        CHECK(dragEnd == 1);

        // multi-format drag out with a pixmap (16x8, hotspot 4,2)
        {
            { std::lock_guard lk(mu); g_dragSource = nullptr; g_dragIcon = nullptr; g_sourceMimes.clear(); }
            dragEnd = -1;
            app::AppWindow::ExternalDrag md{.mime = "text/plain", .data = "hi"};
            md.formats = {{"application/x-fuse-test", "42"}};
            md.image.assign(16 * 8, 0xFF3366CCu);
            md.imageW = 16; md.imageH = 8; md.hotX = 4; md.hotY = 2;
            CHECK(win.beginExternalDrag(std::move(md)));
            run([&] { S.post(g_pointer, 2, 6u, wl::toFixed(22), wl::toFixed(22)); });
            R* s2 = nullptr;
            for (int i = 0; i < 100 && !s2; ++i) { pump(1); std::lock_guard lk(mu); s2 = g_dragSource; }
            CHECK(s2 != nullptr);
            {
                std::lock_guard lk(mu);
                bool custom = false, text = false;
                for (const auto& mm : g_sourceMimes) { custom |= mm == "application/x-fuse-test"; text |= mm == "text/plain;charset=utf-8"; }
                CHECK(custom && text);
                const Attach ic = g_dragIcon && g_attached.count(g_dragIcon) ? g_attached[g_dragIcon] : Attach{};
                CHECK(g_dragIcon != nullptr && ic.w == 16 && ic.h == 8 && ic.dx == -4);
            }
            if (s2) {
                struct Pull { std::atomic<bool> done{false}; std::string data; };
                auto pull = std::make_shared<Pull>();
                std::thread reader;
                run([&] {
                    auto p = os::makePipe();
                    S.post(s2, 1, "application/x-fuse-test", p->second);
                    os::closeFd(p->second);
                    S.flushClients(g_dpy);
                    reader = std::thread([fd = p->first, pull] { pull->data = os::readAllAndClose(fd); pull->done = true; });
                });
                const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                while (!pull->done && std::chrono::steady_clock::now() < end) pump(1);
                if (pull->done) reader.join(); else reader.detach();
                CHECK(pull->done && pull->data == "42");
                run([&] { S.post(s2, 3); S.post(s2, 4); });
                for (int i = 0; i < 100 && dragEnd < 0; ++i) pump(1);
            }
            CHECK(dragEnd == 1);
        }

        // ---- keyboard: client-side repeat into a focused lineedit ----
        CHECK(win.loadText(R"({"fuseui":1,"root":{"class":"lineedit","id":"e","text":""}})").has_value());
        UiContext& ui = win.ui();
        const NodeHandle edit = ui.find("e");
        ui.setFocus(edit);
        win.requestRedraw();
        pump(5);
        CHECK(ui.wantsTextInput());
        std::size_t typed = 0;
        if (haveKeymap) {
            run([&] {
                wl::Array keys{0, 0, nullptr};
                S.post(g_keyboard, 1, S.nextSerial(g_dpy), g_surface, &keys);
                S.post(g_keyboard, 3, S.nextSerial(g_dpy), 10u, 30u, 1u); // KEY_A pressed
            });
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(250)) pump(1);
            run([&] { S.post(g_keyboard, 3, S.nextSerial(g_dpy), 300u, 30u, 0u); }); // released
            pump(5);
            typed = ui.text(edit).size();
            CHECK(typed >= 4); // 1 + repeats every 25 ms after 60 ms
            CHECK(ui.text(edit).find_first_not_of('a') == std::string_view::npos);
            const auto t1 = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - t1 < std::chrono::milliseconds(100)) pump(1);
            CHECK(ui.text(edit).size() == typed); // stopped on release
        }

        // ---- text-input-v3 ----
        run([&] { S.post(g_ti, 0, g_surface); }); // enter
        for (int i = 0; i < 50 && !g_tiCommit; ++i) { win.requestRedraw(); pump(1); }
        { std::lock_guard lk(mu); CHECK(g_tiEnable == 1 && g_tiCommit >= 1 && g_tiCursorH > 0); }
        run([&] { S.post(g_ti, 2, "ni", 2, 2); S.post(g_ti, 5, 1u); });
        for (int i = 0; i < 50 && ui.composition().empty(); ++i) pump(1);
        CHECK(ui.composition() == "ni" && ui.compositionCaret() == 2);
        run([&] { S.post(g_ti, 3, "\xE4\xBD\xA0"); S.post(g_ti, 5, 2u); });
        for (int i = 0; i < 50 && !ui.composition().empty(); ++i) pump(1);
        CHECK(ui.composition().empty());
        CHECK(ui.text(edit).ends_with("\xE4\xBD\xA0"));
        ui.setFocus(NodeHandle{});
        for (int i = 0; i < 50 && !g_tiDisable; ++i) { win.requestRedraw(); pump(1); }
        { std::lock_guard lk(mu); CHECK(g_tiDisable == 1); }

        // ---- HiDPI: fractional scale ----
        CHECK(g_frac != nullptr && win.scale() == 1.f);
        { std::lock_guard lk(mu); CHECK(g_destW == 360 && g_destH == 240); }
        for (const uint32_t s120 : {180u, 240u}) {
            run([&, s120] { S.post(g_frac, 0, s120); });
            const int want = 360 * int(s120) / 120;
            for (int i = 0; i < 100; ++i) {
                win.requestRedraw();
                pump(1);
                std::lock_guard lk(mu);
                if (g_shown.width == want) break;
            }
            CHECK(win.scale() == f32(s120) / 120.f && u32(win.size().w) == 360u);
            Image hi;
            CHECK(wlp.capture(win, hi));
            std::lock_guard lk(mu);
            CHECK(g_shown.width == want && g_shown.height == 240 * int(s120) / 120);
            CHECK(g_destW == 360 && g_destH == 240);
            CHECK(hi.width == g_shown.width && hi.pixels == g_shown.pixels);
        }
        std::printf("fuse_app_wayland: hidpi %dx%d -> viewport %dx%d\n", g_shown.width, g_shown.height, g_destW, g_destH);
        run([&] { S.post(g_frac, 0, 120u); });
        for (int i = 0; i < 100 && win.scale() != 1.f; ++i) pump(1);
        CHECK(win.scale() == 1.f);

        // ---- xdg_popup ----
        auto& pop = a.createWindow({.title = "menu", .width = 120, .height = 80});
        CHECK(wlp.showPopup(pop, win, {10, 10, 40, 20}));
        for (int i = 0; i < 50 && !wlp.configured(pop); ++i) pump(1);
        CHECK(wlp.configured(pop));
        { std::lock_guard lk(mu); CHECK(g_posW == 120 && g_posH == 80 && g_anchorW == 40); }
        pump(5);
        CHECK(u32(pop.size().w) == 100u && u32(pop.size().h) == 70u);
        Image popShown;
        CHECK(wlp.capture(pop, popShown) && popShown.width == 100);
        const u32 before = a.windowCount();
        run([&] { if (g_popup) S.post(g_popup, 1); }); // popup_done (outside click)
        for (int i = 0; i < 50 && a.windowCount() == before; ++i) pump(1);
        CHECK(a.windowCount() == before - 1);
        for (int i = 0; i < 50 && !g_popDestroyed; ++i) pump(1);
        CHECK(g_popDestroyed);
        std::printf("fuse_app_wayland: typed=%zu ime=%d popup=%dx%d\n", typed, g_tiCommit, g_posW, g_posH);
        std::printf("fuse_app_wayland: commits=%d clicks=%d dropped=%zu dragEnd=%d %s\n", g_commits, clicks, dropped.size(),
                    dragEnd, g_fail ? "FAIL" : "OK");
    }
    stop = true;
    th.join();
    S.destroyDisplay(g_dpy);
    return g_fail ? 1 : 0;
}
