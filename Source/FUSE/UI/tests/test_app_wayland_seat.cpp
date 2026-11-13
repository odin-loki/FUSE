// fuse_app Wayland backend, the seat (fuse_app_wayland_seat, Linux / BSD): NativePlatform's Wayland backend against a
// minimal compositor that this test runs in-process, on its own thread, with a runtime-loaded libwayland-server
// (wl_compositor, wl_shm, wl_output, xdg_wm_base, xdg-decoration, wl_seat with pointer + keyboard + touch,
// wl_data_device_manager, wp_viewporter, wp_fractional_scale_v1, wp_cursor_shape_v1, zwp_text_input_v3). The
// compositor speaks the real wire protocol through the backend's own hand-written interface tables, so every request
// and event below is marshalled and demarshalled by libwayland. Weston's headless backend has no seat; this is where
// real wl_pointer / wl_keyboard / wl_touch events are checked:
//   * handshake: forced backend (NativeOptions::backend), title and app id, the committed wl_shm buffer equals the CPU
//     raster, server-side decoration answered by the compositor
//   * wl_pointer enter / motion / button click a button; wp_cursor_shape_v1 gets "text" over a line edit with the
//     enter serial; scrolling: one wheel detent (axis_discrete) = one notch, finger scrolling follows in pixels
//   * wl_keyboard: an xkb keymap (made with libxkbcommon), text with and without Shift, a key without text, key repeat
//     at the compositor's rate while held, stopped by the release
//   * wl_touch: a tap clicks, two fingers start the two-finger gesture, a drag pans the scroll area, cancel ends it
//   * wp_fractional_scale_v1 preferred_scale 1.5 -> AppWindow scale 1.5, a 1.5x buffer, wp_viewport destination = the
//     logical size
//   * clipboard: our text is offered (wl_data_source) and sent on request; another client's selection is received
//   * a drop of text/uri-list from another client reaches UiContext::dropExternal as paths
//   * zwp_text_input_v3: enabled with the caret rectangle while the line edit has focus; pre-edit -> composition,
//     commit -> text
//   * xdg_toplevel.configure (size + maximized + activated) -> AppWindow size / mode / active; close -> closing
//   * the compositor going away: the windows are asked to close, the loop does not spin or hang
// Exit 77 when libwayland-server, libwayland-client or libxkbcommon are not installed.
#include "../App/src/wayland_internal.hpp"
#include "fuse/app/chrome.hpp"
#include "fuse/app/native.hpp"
#include "fuse/ui/widgets/widgets.hpp"
#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace fuse::ui;
namespace app = fuse::app;
namespace wl = fuse::app::wl;
namespace wg = fuse::ui::widgets;

namespace {
// ---- libwayland-server (runtime-loaded) ----------------------------------------------------------------------------
struct D; struct C; struct R; struct Loop; struct ShmBuffer; struct Iface;
struct Array { std::size_t size, alloc; void* data; }; // wl_array
using Fn = void (*)();
template <class F>
Fn fn(F f) { return reinterpret_cast<Fn>(f); }
struct Server {
    void* so = nullptr;
    D* (*create)() = nullptr;
    const char* (*addSocketAuto)(D*) = nullptr;
    Loop* (*getLoop)(D*) = nullptr;
    int (*loopDispatch)(Loop*, int) = nullptr;
    void (*flushClients)(D*) = nullptr;
    void (*destroyClients)(D*) = nullptr;
    void (*destroyDisplay)(D*) = nullptr;
    int (*initShm)(D*) = nullptr;
    uint32_t (*nextSerial)(D*) = nullptr;
    void* (*globalCreate)(D*, const Iface*, int, void*, void (*)(C*, void*, uint32_t, uint32_t)) = nullptr;
    R* (*resCreate)(C*, const Iface*, int, uint32_t) = nullptr;
    void (*setImpl)(R*, const void*, void*, void (*)(R*)) = nullptr;
    void (*post)(R*, uint32_t, ...) = nullptr;
    void* (*userData)(R*) = nullptr;
    void (*resDestroy)(R*) = nullptr;
    int (*resVersion)(R*) = nullptr;
    ShmBuffer* (*shmGet)(R*) = nullptr;
    void* (*shmData)(ShmBuffer*) = nullptr;
    int32_t (*shmW)(ShmBuffer*) = nullptr;
    int32_t (*shmH)(ShmBuffer*) = nullptr;
    int32_t (*shmStride)(ShmBuffer*) = nullptr;
    const Iface *compositor, *surface, *region, *seat, *pointer, *keyboard, *touch, *output, *callback, *ddm, *device, *offer, *source;
    bool load() {
        so = dlopen("libwayland-server.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!so) so = dlopen("libwayland-server.so", RTLD_NOW | RTLD_LOCAL);
        if (!so) return false;
        bool all = true;
        auto f = [&](auto& p, const char* n) { p = reinterpret_cast<std::remove_reference_t<decltype(p)>>(dlsym(so, n)); all = all && p; };
        auto i = [&](const Iface*& p, const char* n) { p = static_cast<const Iface*>(dlsym(so, n)); all = all && p; };
        f(create, "wl_display_create"); f(addSocketAuto, "wl_display_add_socket_auto"); f(getLoop, "wl_display_get_event_loop");
        f(loopDispatch, "wl_event_loop_dispatch"); f(flushClients, "wl_display_flush_clients");
        f(destroyClients, "wl_display_destroy_clients"); f(destroyDisplay, "wl_display_destroy");
        f(initShm, "wl_display_init_shm"); f(nextSerial, "wl_display_next_serial"); f(globalCreate, "wl_global_create");
        f(resCreate, "wl_resource_create"); f(setImpl, "wl_resource_set_implementation"); f(post, "wl_resource_post_event");
        f(userData, "wl_resource_get_user_data"); f(resDestroy, "wl_resource_destroy"); f(resVersion, "wl_resource_get_version");
        f(shmGet, "wl_shm_buffer_get"); f(shmData, "wl_shm_buffer_get_data"); f(shmW, "wl_shm_buffer_get_width");
        f(shmH, "wl_shm_buffer_get_height"); f(shmStride, "wl_shm_buffer_get_stride");
        i(compositor, "wl_compositor_interface"); i(surface, "wl_surface_interface"); i(region, "wl_region_interface");
        i(seat, "wl_seat_interface"); i(pointer, "wl_pointer_interface"); i(keyboard, "wl_keyboard_interface");
        i(touch, "wl_touch_interface"); i(output, "wl_output_interface"); i(callback, "wl_callback_interface");
        i(ddm, "wl_data_device_manager_interface"); i(device, "wl_data_device_interface"); i(offer, "wl_data_offer_interface");
        i(source, "wl_data_source_interface");
        return all;
    }
};
Server S;
const Iface* ext(const char* name) { return static_cast<const Iface*>(wl::protocolInterface(name)); } // the backend's tables

// ---- compositor state: written on the compositor thread, read by the test under `mu` ---------------------------------
D* g_dpy = nullptr;
std::mutex mu;
std::deque<std::function<void()>> cmds;
std::atomic<bool> stop{false};
struct State {
    R *surface = nullptr, *xsurf = nullptr, *top = nullptr, *pointer = nullptr, *keyboard = nullptr, *touch = nullptr, *device = nullptr,
      *deco = nullptr, *frac = nullptr, *ti = nullptr, *selection = nullptr, *pendingBuffer = nullptr;
    C* client = nullptr;
    std::vector<R*> frames; ///< wl_surface.frame callbacks of the toplevel, answered at the next commit
    bool configured = false, acked = false;
    std::string title, appId, keymap;
    int commits = 0, decoMode = 0, destW = 0, destH = 0, setCursorNull = 0;
    Image shown;
    std::vector<uint32_t> shapes;
    uint32_t enterSerial = 0, shapeSerialBad = 0, selectionSets = 0;
    std::vector<std::string> sourceMimes;
    std::string pasted;       ///< what our selection source sent when asked
    std::string accepted;     ///< the MIME type the client accepted for the drag
    std::string received;     ///< the MIME type the client asked the offer for
    int tiEnable = 0, tiDisable = 0, tiCommit = 0, tiCursorH = 0, maxW = 0, minW = 0, maximizeAsked = 0;
    // client-side chrome: what xdg_toplevel.move / resize carried, and how the decoration manager answers
    int moves = 0, resizes = 0, decoGets = 0;
    uint32_t moveSerial = 0, resizeSerial = 0, resizeEdge = 0, decoAnswer = 2 /*server_side*/;
    // zwp_primary_selection_v1
    R* primarySource = nullptr;
    int primarySets = 0;
    std::vector<std::string> primaryMimes;
} g;
const char* g_offerPayload = "";

void noop() {}
Fn g_noops[16] = {noop, noop, noop, noop, noop, noop, noop, noop, noop, noop, noop, noop, noop, noop, noop, noop};
void destroyReq(C*, R* r) { S.resDestroy(r); }
R* make(C* c, const Iface* i, int v, uint32_t id, const void* impl, void* data = nullptr) {
    R* r = S.resCreate(c, i, v, id);
    S.setImpl(r, impl, data, nullptr);
    return r;
}
void writeAllAsync(int fd, std::string data) {
    std::thread([fd, d = std::move(data)] {
        std::size_t at = 0;
        while (at < d.size()) {
            const ssize_t n = ::write(fd, d.data() + at, d.size() - at);
            if (n <= 0) break;
            at += std::size_t(n);
        }
        ::close(fd);
    }).detach();
}

// wl_surface
void sAttach(C*, R* r, R* buffer, int32_t, int32_t) { if (r == g.surface) g.pendingBuffer = buffer; }
void sFrame(C* c, R* r, uint32_t id) {
    R* cb = make(c, S.callback, 1, id, g_noops);
    if (r == g.surface) g.frames.push_back(cb);
    else { S.post(cb, 0, 0u); S.resDestroy(cb); }
}
void sCommit(C*, R* r) {
    if (r != g.surface) return;
    std::lock_guard lk(mu);
    ++g.commits;
    if (g.pendingBuffer) {
        if (ShmBuffer* b = S.shmGet(g.pendingBuffer)) {
            const int w = S.shmW(b), h = S.shmH(b), st = S.shmStride(b);
            g.shown.resize(w, h, 0);
            const auto* src = static_cast<const uint8_t*>(S.shmData(b));
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    uint32_t p;
                    std::memcpy(&p, src + y * st + x * 4, 4); // XRGB8888 -> 0xAABBGGRR
                    g.shown.pixels[std::size_t(y * w + x)] = 0xFF000000u | ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
                }
        }
        S.post(g.pendingBuffer, 0); // wl_buffer.release
        g.pendingBuffer = nullptr;
    }
    for (R* cb : g.frames) { // "shown": the frame callbacks of this commit
        S.post(cb, 0, 0u);
        S.resDestroy(cb);
    }
    g.frames.clear();
    if (g.top && !g.configured) { // the initial commit: first configure (the client picks the size)
        g.configured = true;
        Array states{0, 0, nullptr};
        S.post(g.top, 0, 0, 0, &states);
        S.post(g.xsurf, 0, S.nextSerial(g_dpy));
    }
}
Fn surfaceImpl[11];
void cCreateSurface(C* c, R* r, uint32_t id) {
    R* s = make(c, S.surface, S.resVersion(r), id, surfaceImpl);
    if (!g.surface) { g.surface = s; g.client = c; } // later surfaces: the cursor
}
Fn regionImpl[3] = {fn(&destroyReq), noop, noop};
void cCreateRegion(C* c, R*, uint32_t id) { make(c, S.region, 1, id, regionImpl); }
Fn compositorImpl[2] = {fn(&cCreateSurface), fn(&cCreateRegion)};
void bindCompositor(C* c, void*, uint32_t v, uint32_t id) { make(c, S.compositor, int(v), id, compositorImpl); }

// xdg-shell
void topTitle(C*, R*, const char* t) { std::lock_guard lk(mu); g.title = t; }
void topAppId(C*, R*, const char* t) { std::lock_guard lk(mu); g.appId = t; }
void topMax(C*, R*, int32_t w, int32_t) { std::lock_guard lk(mu); g.maxW = w; }
void topMin(C*, R*, int32_t w, int32_t) { std::lock_guard lk(mu); g.minW = w; }
void topMaximize(C*, R*) { std::lock_guard lk(mu); ++g.maximizeAsked; }
void topMove(C*, R*, R*, uint32_t serial) { std::lock_guard lk(mu); ++g.moves; g.moveSerial = serial; }
void topResize(C*, R*, R*, uint32_t serial, uint32_t edge) { std::lock_guard lk(mu); ++g.resizes; g.resizeSerial = serial; g.resizeEdge = edge; }
Fn topImpl[14];
void xsGetToplevel(C* c, R*, uint32_t id) { g.top = make(c, ext("xdg_toplevel"), 1, id, topImpl); }
void xsAck(C*, R*, uint32_t) { std::lock_guard lk(mu); g.acked = true; }
Fn xsurfImpl[5] = {fn(&destroyReq), fn(&xsGetToplevel), noop, noop, fn(&xsAck)};
void wmGetXdgSurface(C* c, R*, uint32_t id, R*) { g.xsurf = make(c, ext("xdg_surface"), 1, id, xsurfImpl); }
Fn positionerImpl[7] = {fn(&destroyReq), noop, noop, noop, noop, noop, noop};
void wmCreatePositioner(C* c, R*, uint32_t id) { make(c, ext("xdg_positioner"), 1, id, positionerImpl); }
Fn wmImpl[4] = {fn(&destroyReq), fn(&wmCreatePositioner), fn(&wmGetXdgSurface), noop};
void bindWm(C* c, void*, uint32_t, uint32_t id) { make(c, ext("xdg_wm_base"), 1, id, wmImpl); }
// xdg-decoration: the compositor draws the frame
void decoSetMode(C*, R* r, uint32_t mode) {
    uint32_t answer;
    { std::lock_guard lk(mu); g.decoMode = int(mode); answer = g.decoAnswer; }
    S.post(r, 0, answer); // configure: server_side (2) unless the test plays a compositor without frames (1)
}
Fn decoImpl[3] = {fn(&destroyReq), fn(&decoSetMode), noop};
void decoGet(C* c, R*, uint32_t id, R*) {
    { std::lock_guard lk(mu); ++g.decoGets; }
    g.deco = make(c, ext("zxdg_toplevel_decoration_v1"), 1, id, decoImpl);
}
Fn decoMgrImpl[2] = {fn(&destroyReq), fn(&decoGet)};
void bindDecoMgr(C* c, void*, uint32_t, uint32_t id) { make(c, ext("zxdg_decoration_manager_v1"), 1, id, decoMgrImpl); }

// wl_output: 800x600, scale 1
void bindOutput(C* c, void*, uint32_t v, uint32_t id) {
    R* o = make(c, S.output, int(v), id, g_noops);
    S.post(o, 0, 0, 0, 300, 200, 0, "FUSE", "test output", 0); // geometry
    S.post(o, 1, 3u, 800, 600, 60000);                          // mode: current | preferred
    S.post(o, 3, 1);                                            // scale
    S.post(o, 2);                                               // done
}

// wl_seat
void ptrSetCursor(C*, R*, uint32_t, R* surface, int32_t, int32_t) { std::lock_guard lk(mu); g.setCursorNull += surface == nullptr; }
Fn pointerImpl[2] = {fn(&ptrSetCursor), fn(&destroyReq)};
Fn releaseImpl[1] = {fn(&destroyReq)};
void seatGetPointer(C* c, R* r, uint32_t id) { g.pointer = make(c, S.pointer, S.resVersion(r), id, pointerImpl); }
void seatGetKeyboard(C* c, R* r, uint32_t id) {
    g.keyboard = make(c, S.keyboard, S.resVersion(r), id, releaseImpl);
    const std::size_t size = g.keymap.size() + 1;
    char name[] = "/tmp/fuse-wl-keymap-XXXXXX";
    const int fd = ::mkstemp(name);
    if (fd >= 0) {
        ::unlink(name);
        if (::write(fd, g.keymap.c_str(), size) == ssize_t(size)) S.post(g.keyboard, 0, 1u, fd, uint32_t(size)); // xkb_v1; the fd is dup'ed
        ::close(fd);
    }
    S.post(g.keyboard, 5, 25, 150); // repeat_info: 25 Hz after 150 ms
}
void seatGetTouch(C* c, R* r, uint32_t id) { g.touch = make(c, S.touch, S.resVersion(r), id, releaseImpl); }
Fn seatImpl[4] = {fn(&seatGetPointer), fn(&seatGetKeyboard), fn(&seatGetTouch), fn(&destroyReq)};
void bindSeat(C* c, void*, uint32_t v, uint32_t id) {
    R* r = make(c, S.seat, int(v), id, seatImpl);
    S.post(r, 0, 7u); // capabilities: pointer | keyboard | touch
}
// wp_cursor_shape_v1
void shapeSet(C*, R*, uint32_t serial, uint32_t shape) {
    std::lock_guard lk(mu);
    g.shapes.push_back(shape);
    if (serial != g.enterSerial) ++g.shapeSerialBad;
}
Fn shapeDevImpl[2] = {fn(&destroyReq), fn(&shapeSet)};
void shapeGetPointer(C* c, R*, uint32_t id, R*) { make(c, ext("wp_cursor_shape_device_v1"), 1, id, shapeDevImpl); }
Fn shapeMgrImpl[3] = {fn(&destroyReq), fn(&shapeGetPointer), noop};
void bindShapeMgr(C* c, void*, uint32_t, uint32_t id) { make(c, ext("wp_cursor_shape_manager_v1"), 1, id, shapeMgrImpl); }
// wp_viewporter + wp_fractional_scale_v1
void vpDest(C*, R* r, int32_t w, int32_t h) {
    if (S.userData(r) != g.surface) return;
    std::lock_guard lk(mu);
    g.destW = w;
    g.destH = h;
}
Fn vpImpl[3] = {fn(&destroyReq), noop, fn(&vpDest)};
void vpGet(C* c, R*, uint32_t id, R* surf) { make(c, ext("wp_viewport"), 1, id, vpImpl, surf); }
Fn vpMgrImpl[2] = {fn(&destroyReq), fn(&vpGet)};
void bindViewporter(C* c, void*, uint32_t, uint32_t id) { make(c, ext("wp_viewporter"), 1, id, vpMgrImpl); }
Fn fracImpl[1] = {fn(&destroyReq)};
void fracGet(C* c, R*, uint32_t id, R* surf) {
    R* f = make(c, ext("wp_fractional_scale_v1"), 1, id, fracImpl);
    if (surf == g.surface) g.frac = f;
}
Fn fracMgrImpl[2] = {fn(&destroyReq), fn(&fracGet)};
void bindFracMgr(C* c, void*, uint32_t, uint32_t id) { make(c, ext("wp_fractional_scale_manager_v1"), 1, id, fracMgrImpl); }
// zwp_text_input_v3
void tiEnable(C*, R*) { std::lock_guard lk(mu); ++g.tiEnable; }
void tiDisable(C*, R*) { std::lock_guard lk(mu); ++g.tiDisable; }
void tiCursor(C*, R*, int32_t, int32_t, int32_t, int32_t h) { std::lock_guard lk(mu); g.tiCursorH = h; }
void tiCommitReq(C*, R*) { std::lock_guard lk(mu); ++g.tiCommit; }
Fn tiImpl[8] = {fn(&destroyReq), fn(&tiEnable), fn(&tiDisable), noop, noop, noop, fn(&tiCursor), fn(&tiCommitReq)};
void tiMgrGet(C* c, R*, uint32_t id, R*) { g.ti = make(c, ext("zwp_text_input_v3"), 1, id, tiImpl); }
Fn tiMgrImpl[2] = {fn(&destroyReq), fn(&tiMgrGet)};
void bindTiMgr(C* c, void*, uint32_t, uint32_t id) { make(c, ext("zwp_text_input_manager_v3"), 1, id, tiMgrImpl); }
// wl_data_device_manager
void srcOffer(C*, R*, const char* mime) { std::lock_guard lk(mu); g.sourceMimes.emplace_back(mime); }
Fn sourceImpl[3] = {fn(&srcOffer), fn(&destroyReq), noop};
void ddmCreateSource(C* c, R* r, uint32_t id) { make(c, S.source, S.resVersion(r), id, sourceImpl); }
void devSetSelection(C*, R*, R* source, uint32_t) {
    std::lock_guard lk(mu);
    g.selection = source;
    ++g.selectionSets;
}
Fn deviceImpl[3] = {noop, fn(&devSetSelection), fn(&destroyReq)};
void ddmGetDevice(C* c, R* r, uint32_t id, R*) { g.device = make(c, S.device, S.resVersion(r), id, deviceImpl); }
Fn ddmImpl[2] = {fn(&ddmCreateSource), fn(&ddmGetDevice)};
void bindDdm(C* c, void*, uint32_t v, uint32_t id) { make(c, S.ddm, int(v), id, ddmImpl); }
// zwp_primary_selection_v1
void psSrcOffer(C*, R*, const char* mime) { std::lock_guard lk(mu); g.primaryMimes.emplace_back(mime); }
Fn psSourceImpl[2] = {fn(&psSrcOffer), fn(&destroyReq)};
void psCreateSource(C* c, R*, uint32_t id) { make(c, ext("zwp_primary_selection_source_v1"), 1, id, psSourceImpl); }
void psSetSelection(C*, R*, R* source, uint32_t) {
    std::lock_guard lk(mu);
    g.primarySource = source;
    ++g.primarySets;
}
Fn psDeviceImpl[2] = {fn(&psSetSelection), fn(&destroyReq)};
void psGetDevice(C* c, R*, uint32_t id, R*) { make(c, ext("zwp_primary_selection_device_v1"), 1, id, psDeviceImpl); }
Fn psMgrImpl[3] = {fn(&psCreateSource), fn(&psGetDevice), fn(&destroyReq)};
void bindPsMgr(C* c, void*, uint32_t, uint32_t id) { make(c, ext("zwp_primary_selection_device_manager_v1"), 1, id, psMgrImpl); }
void offerAccept(C*, R*, uint32_t, const char* mime) { std::lock_guard lk(mu); g.accepted = mime ? mime : ""; }
void offerReceive(C*, R*, const char* mime, int32_t fd) {
    { std::lock_guard lk(mu); g.received = mime; }
    writeAllAsync(fd, g_offerPayload);
}
Fn offerImpl[5] = {fn(&offerAccept), fn(&offerReceive), fn(&destroyReq), noop, noop};

void compositorThread() {
    Loop* loop = S.getLoop(g_dpy);
    while (!stop) {
        S.loopDispatch(loop, 2);
        std::deque<std::function<void()>> todo;
        { std::lock_guard lk(mu); todo.swap(cmds); }
        for (auto& f : todo) f();
        S.flushClients(g_dpy);
    }
}
/// Runs `f` on the compositor thread and waits for it.
void run(std::function<void()> f) {
    std::promise<void> done;
    { std::lock_guard lk(mu); cmds.push_back([&] { f(); done.set_value(); }); }
    done.get_future().wait();
}
int32_t fx(f32 v) { return int32_t(std::lround(v * 256.f)); } // wl_fixed_t
uint32_t serial() { return S.nextSerial(g_dpy); }

bool makeKeymap() {
    void* so = dlopen("libxkbcommon.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!so) so = dlopen("libxkbcommon.so", RTLD_NOW | RTLD_LOCAL);
    if (!so) return false;
    auto ctxNew = reinterpret_cast<void* (*)(int)>(dlsym(so, "xkb_context_new"));
    auto fromNames = reinterpret_cast<void* (*)(void*, const void*, int)>(dlsym(so, "xkb_keymap_new_from_names"));
    auto asString = reinterpret_cast<char* (*)(void*, int)>(dlsym(so, "xkb_keymap_get_as_string"));
    if (!ctxNew || !fromNames || !asString) return false;
    struct Names { const char *rules, *model, *layout, *variant, *options; } names{"evdev", "pc105", "us", "", ""};
    void* ctx = ctxNew(0);
    void* km = ctx ? fromNames(ctx, &names, 0) : nullptr;
    char* text = km ? asString(km, 1) : nullptr;
    if (!text) return false;
    g.keymap = text;
    std::free(text); // keymap / context are left to the process exit
    return true;
}

// ---- the client side ------------------------------------------------------------------------------------------------
void settle(app::Application& a, int n = 30) {
    for (int i = 0; i < n; ++i) a.step(0.004f);
}
template <class F>
bool waitFor(app::Application& a, F done) {
    for (int i = 0; i < 750 && !done(); ++i) a.step(0.004f);
    return done();
}
template <class T>
T locked(T State::* field) {
    std::lock_guard lk(mu);
    return g.*field;
}
Vec2 centre(UiContext& ui, NodeHandle n) {
    const Rect r = ui.rect(n);
    return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
}
std::size_t differing(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height) return std::size_t(-1);
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) n += (a.pixels[i] & 0xFFFFFFu) != (b.pixels[i] & 0xFFFFFFu);
    return n;
}
int g_clicks = 0;
std::string g_dropText;
u32 g_dropType = 0;
int g_wheels = 0;
} // namespace

int main() {
    std::string err;
    if (!wl::loadLibrary(err)) {
        std::printf("fuse_app_wayland_seat: skipped (%s)\n", err.c_str());
        return 77;
    }
    if (!S.load()) {
        std::printf("fuse_app_wayland_seat: skipped (libwayland-server.so.0 is not installed)\n");
        return 77;
    }
    if (!makeKeymap()) {
        std::printf("fuse_app_wayland_seat: skipped (libxkbcommon.so.0 is not installed)\n");
        return 77;
    }
    for (auto& f : surfaceImpl) f = noop;
    surfaceImpl[0] = fn(&destroyReq); surfaceImpl[1] = fn(&sAttach); surfaceImpl[3] = fn(&sFrame); surfaceImpl[6] = fn(&sCommit);
    for (auto& f : topImpl) f = noop;
    topImpl[0] = fn(&destroyReq); topImpl[2] = fn(&topTitle); topImpl[3] = fn(&topAppId); topImpl[7] = fn(&topMax);
    topImpl[8] = fn(&topMin); topImpl[9] = fn(&topMaximize); topImpl[5] = fn(&topMove); topImpl[6] = fn(&topResize);

    // a private runtime directory for the socket
    const char* tmp = std::getenv("TMPDIR");
    std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/fuse-wl-seat-XXXXXX";
    if (!::mkdtemp(dir.data())) {
        std::printf("fuse_app_wayland_seat: skipped (no temporary directory)\n");
        return 77;
    }
    ::chmod(dir.c_str(), 0700);
    ::setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
    g_dpy = S.create();
    const char* sock = g_dpy ? S.addSocketAuto(g_dpy) : nullptr;
    if (!sock) {
        std::printf("fuse_app_wayland_seat: skipped (cannot create a Wayland socket in %s)\n", dir.c_str());
        return 77;
    }
    const std::string socket = sock;
    S.initShm(g_dpy);
    S.globalCreate(g_dpy, S.compositor, 4, nullptr, &bindCompositor);
    S.globalCreate(g_dpy, S.output, 2, nullptr, &bindOutput);
    S.globalCreate(g_dpy, ext("xdg_wm_base"), 1, nullptr, &bindWm);
    S.globalCreate(g_dpy, ext("zxdg_decoration_manager_v1"), 1, nullptr, &bindDecoMgr);
    S.globalCreate(g_dpy, S.seat, 5, nullptr, &bindSeat);
    S.globalCreate(g_dpy, S.ddm, 3, nullptr, &bindDdm);
    S.globalCreate(g_dpy, ext("wp_viewporter"), 1, nullptr, &bindViewporter);
    S.globalCreate(g_dpy, ext("wp_fractional_scale_manager_v1"), 1, nullptr, &bindFracMgr);
    S.globalCreate(g_dpy, ext("wp_cursor_shape_manager_v1"), 1, nullptr, &bindShapeMgr);
    S.globalCreate(g_dpy, ext("zwp_text_input_manager_v3"), 1, nullptr, &bindTiMgr);
    S.globalCreate(g_dpy, ext("zwp_primary_selection_device_manager_v1"), 1, nullptr, &bindPsMgr);
    std::thread th(compositorThread);

    ::setenv("WAYLAND_DISPLAY", socket.c_str(), 1);
    ::setenv("FUSE_UI_BACKEND", "x11", 1); // the option beats the environment
    ::unsetenv("FUSE_UI_SCALE");
    {
        app::Application a{{.name = "gate_wayland_seat", .loadSettings = false}};
        app::NativePlatform native(a, {.vulkan = false, .backend = app::NativeBackend::Wayland});
        UI_CHECK(native.available() && native.backend() == app::NativeBackend::Wayland);
        if (native.backend() != app::NativeBackend::Wayland) {
            std::fprintf(stderr, "no Wayland backend: %.*s\n", int(native.lastError().size()), native.lastError().data());
            stop = true;
            th.join();
            return 1;
        }
        wl::Backend* b = wl::current();
        auto& w = a.createWindow({.title = "seat gate", .width = 360, .height = 240});
        UiContext& ui = w.ui();
        ui.setCss(ui.root(), "flex-direction", "column");
        const NodeHandle btn = wg::button(ui, ui.root(), "Press", "b");
        const NodeHandle edit = wg::lineEdit(ui, ui.root(), "", "e");
        const NodeHandle area = wg::scrollArea(ui, ui.root(), "s");
        ui.setCss(area, "height", "120px");
        for (int i = 0; i < 30; ++i) ui.setCss(wg::label(ui, area, "row " + std::to_string(i)), "flex-shrink", "0");
        for (const NodeHandle n : {btn, edit, area}) ui.setCss(n, "flex-shrink", "0");
        ui.connect(btn, SignalKind::Clicked, [](UiContext&, NodeHandle, const SignalArgs&) { ++g_clicks; });
        ui.setDropTypes(area, {kDragFiles});
        ui.connect(area, SignalKind::Event, [](UiContext&, NodeHandle, const SignalArgs& s) {
            if (s.event && s.event->type == EventType::Drop && s.event->drag) {
                g_dropText = s.event->drag->text;
                g_dropType = s.event->drag->type;
            }
            if (s.event && s.event->type == EventType::Wheel) ++g_wheels;
        });
        w.setMaximumSize({1000.f, 0.f}); // asked before the window exists: sent with show()
        UI_CHECK(native.show(w) == app::PresenterKind::Software);
        wl::Window* win = wl::findWindow(b, w);
        UI_CHECK(win != nullptr);
        UI_CHECK(waitFor(a, [&] { return w.framesPresented() >= 1 && locked(&State::commits) >= 2; }));
        settle(a);

        // ---- handshake ----
        UI_CHECK(locked(&State::acked));
        UI_CHECK(locked(&State::title) == "seat gate" && locked(&State::appId) == "gate_wayland_seat");
        UI_CHECK(locked(&State::maxW) == 1000);
        UI_CHECK(w.size().w == 360.f && w.size().h == 240.f); // configure (0, 0): the client's size
        {
            const Image ref = w.rasterize({});
            std::lock_guard lk(mu);
            UI_CHECK(g.shown.width == 360 && g.shown.height == 240);
            const std::size_t d = differing(g.shown, ref);
            std::printf("  buffer seen by the compositor: %dx%d, %zu pixels differ from the CPU raster\n", g.shown.width, g.shown.height, d);
            UI_CHECK(d == 0);
        }
        UI_CHECK(locked(&State::decoMode) == 2 && wl::serverDecorated(win)); // asked for and got a server-side frame
        // ... so the window keeps its System frame: no client-side title bar is added
        UI_CHECK(w.chrome().frame == app::WindowFrame::System && !ui.alive(ui.find("titlebar")));
        // what this backend does not have answers "nothing" (no X11 code behind it)
        UI_CHECK(native.toolWindowCount(w) == 0 && native.toolWindow(w, 0) == 0 && native.toolWindowScreenRect(w, 0).w == 0.f);
        Image none;
        UI_CHECK(!native.captureToolWindow(w, 0, none) && !native.externalDragActive());
        UI_CHECK(native.screenOrigin(w).x == 0.f && native.cursorThemeSize() == 0); // cursor-shape: the compositor's cursors
        const auto disp = native.displays();
        UI_CHECK(disp.size() == 1 && disp[0].bounds.w == 800.f && disp[0].refreshHz == 60.f && disp[0].name == "FUSE test output");
        UI_CHECK(wl::hasGlobal(b, "wp_cursor_shape_manager_v1") && !wl::hasGlobal(b, "xdg_toplevel_icon_manager_v1"));
        Image icon;
        icon.resize(16, 16, 0xFF0000FFu);
        UI_CHECK(!native.setIcon(w, {&icon, 1}) && native.iconSizes(w).empty()); // this compositor has no icon protocol

        // ---- pointer ----
        const Vec2 pb = centre(ui, btn);
        run([&] {
            g.enterSerial = serial();
            S.post(g.pointer, 0, g.enterSerial, g.surface, fx(pb.x), fx(pb.y)); // enter
            S.post(g.pointer, 5);                                                // frame
            S.post(g.pointer, 3, serial(), 10u, 0x110u, 1u);                     // left down
            S.post(g.pointer, 5);
            S.post(g.pointer, 3, serial(), 20u, 0x110u, 0u);                     // left up
            S.post(g.pointer, 5);
        });
        UI_CHECK(waitFor(a, [&] { return g_clicks == 1; }));
        const Vec2 pe = centre(ui, edit);
        run([&] {
            S.post(g.pointer, 2, 30u, fx(pe.x), fx(pe.y)); // motion onto the line edit
            S.post(g.pointer, 5);
        });
        UI_CHECK(waitFor(a, [&] { std::lock_guard lk(mu); return !g.shapes.empty() && g.shapes.back() == 9u; })); // "text"
        UI_CHECK(native.cursor(w) == Cursor::IBeam && wl::cursorShapeSent(win) == 9u);
        UI_CHECK(locked(&State::shapeSerialBad) == 0u);
        ui.setCursorOverride(Cursor::Hidden); // no cursor image: wl_pointer.set_cursor with no surface
        w.requestRedraw();
        UI_CHECK(waitFor(a, [&] { return locked(&State::setCursorNull) == 1; }));
        ui.setCursorOverride(Cursor::Auto);
        // scrolling over the scroll area: one detent down (15 units, 1 step), then two fingers 30 px further
        const Vec2 ps = centre(ui, area);
        run([&] {
            S.post(g.pointer, 2, 40u, fx(ps.x), fx(ps.y));
            S.post(g.pointer, 5);
            S.post(g.pointer, 6, 0u);            // axis_source: wheel
            S.post(g.pointer, 8, 0u, 1);         // axis_discrete: vertical, one step
            S.post(g.pointer, 4, 41u, 0u, fx(15.f));
            S.post(g.pointer, 5);
        });
        UI_CHECK(waitFor(a, [&] { return ui.scroll(area).y == 40.f; })); // a notch scrolls 40 px
        UI_CHECK(g_wheels == 0 || g_wheels == 1);
        run([&] {
            S.post(g.pointer, 6, 1u);            // axis_source: finger
            S.post(g.pointer, 4, 42u, 0u, fx(30.f));
            S.post(g.pointer, 5);
        });
        UI_CHECK(waitFor(a, [&] { return ui.scroll(area).y == 70.f; })); // follows the fingers in pixels
        run([&] {
            S.post(g.pointer, 6, 0u);
            S.post(g.pointer, 8, 0u, -1);
            S.post(g.pointer, 4, 43u, 0u, fx(-15.f));
            S.post(g.pointer, 5);
        });
        UI_CHECK(waitFor(a, [&] { return ui.scroll(area).y == 30.f; }));

        // ---- keyboard ----
        run([&] {
            S.post(g.pointer, 2, 50u, fx(pe.x), fx(pe.y));
            S.post(g.pointer, 5);
            S.post(g.pointer, 3, serial(), 51u, 0x110u, 1u); // click the line edit: focus
            S.post(g.pointer, 5);
            S.post(g.pointer, 3, serial(), 52u, 0x110u, 0u);
            S.post(g.pointer, 5);
            Array keys{0, 0, nullptr};
            S.post(g.keyboard, 1, serial(), g.surface, &keys); // enter
            S.post(g.keyboard, 4, serial(), 0u, 0u, 0u, 0u);   // modifiers: none
            S.post(g.keyboard, 3, serial(), 60u, 30u, 1u);     // KEY_A down
            S.post(g.keyboard, 3, serial(), 61u, 30u, 0u);     // up
        });
        UI_CHECK(waitFor(a, [&] { return ui.text(edit) == "a"; }));
        UI_CHECK(wl::keyboardFocus(win));
        run([&] {
            S.post(g.keyboard, 4, serial(), 1u, 0u, 0u, 0u); // Shift held
            S.post(g.keyboard, 3, serial(), 62u, 48u, 1u);   // KEY_B
            S.post(g.keyboard, 3, serial(), 63u, 48u, 0u);
            S.post(g.keyboard, 4, serial(), 0u, 0u, 0u, 0u);
        });
        UI_CHECK(waitFor(a, [&] { return ui.text(edit) == "aB"; }));
        run([&] {
            S.post(g.keyboard, 3, serial(), 64u, 14u, 1u); // Backspace: a key, no text
            S.post(g.keyboard, 3, serial(), 65u, 14u, 0u);
        });
        UI_CHECK(waitFor(a, [&] { return ui.text(edit) == "a"; }));
        // repeat: 'x' held ~400 ms at 25 Hz after 150 ms -> about 6 more; none after the release
        run([&] { S.post(g.keyboard, 3, serial(), 70u, 45u, 1u); });
        {
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(400)) a.step(0.01f);
        }
        run([&] { S.post(g.keyboard, 3, serial(), 71u, 45u, 0u); });
        settle(a, 10);
        const std::size_t held = ui.text(edit).size();
        std::printf("  key repeat: 'x' typed %zu times in 400 ms (25 Hz after 150 ms)\n", held - 1);
        UI_CHECK(held - 1 >= 3 && held - 1 <= 14); // 7 on an idle machine; a stalled loop catches up a few at a time
        {
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(150)) a.step(0.01f);
        }
        UI_CHECK(ui.text(edit).size() == held);

        // ---- text-input-v3: the line edit has focus ----
        run([&] { S.post(g.ti, 0, g.surface); }); // enter
        UI_CHECK(waitFor(a, [&] { return locked(&State::tiEnable) == 1 && locked(&State::tiCommit) >= 1; }));
        UI_CHECK(locked(&State::tiCursorH) > 0); // the caret rectangle (where the candidate window goes)
        run([&] {
            S.post(g.ti, 2, "ni", 2, 2); // preedit_string
            S.post(g.ti, 5, 1u);         // done
        });
        UI_CHECK(waitFor(a, [&] { return ui.composition() == "ni"; }));
        UI_CHECK(ui.compositionCaret() == 2);
        const std::string beforeIme(ui.text(edit));
        run([&] {
            S.post(g.ti, 3, "\xE4\xBD\xA0"); // commit_string
            S.post(g.ti, 5, 2u);
        });
        UI_CHECK(waitFor(a, [&] { return ui.text(edit) == beforeIme + "\xE4\xBD\xA0"; }));
        UI_CHECK(ui.composition().empty());

        // ---- touch: a tap on the button, then a drag that pans the scroll area ----
        run([&] {
            S.post(g.touch, 0, serial(), 100u, g.surface, 7, fx(pb.x), fx(pb.y)); // down
            S.post(g.touch, 3);                                                    // frame
            S.post(g.touch, 1, serial(), 140u, 7);                                 // up
            S.post(g.touch, 3);
        });
        UI_CHECK(waitFor(a, [&] { return g_clicks == 2; }));
        // two fingers: the pointer ids reach the core's two-finger gesture (pinch / rotate) path
        run([&] {
            S.post(g.touch, 0, serial(), 150u, g.surface, 1, fx(ps.x - 20.f), fx(ps.y));
            S.post(g.touch, 0, serial(), 151u, g.surface, 2, fx(ps.x + 20.f), fx(ps.y));
            S.post(g.touch, 3);
        });
        UI_CHECK(waitFor(a, [&] { return ui.touchCount() == 2 && ui.gestureActive(); }));
        run([&] {
            S.post(g.touch, 1, serial(), 160u, 1);
            S.post(g.touch, 1, serial(), 161u, 2);
            S.post(g.touch, 3);
        });
        UI_CHECK(waitFor(a, [&] { return ui.touchCount() == 0 && !ui.gestureActive(); }));
        UI_CHECK(g_clicks == 2); // a two-finger touch is not a click
        const f32 y0 = ui.scroll(area).y;
        run([&] {
            S.post(g.touch, 0, serial(), 200u, g.surface, 8, fx(ps.x), fx(ps.y));
            S.post(g.touch, 3);
            for (int i = 1; i <= 10; ++i) {
                S.post(g.touch, 2, uint32_t(200 + i * 16), 8, fx(ps.x), fx(ps.y - f32(i) * 5.f)); // 50 px up
                S.post(g.touch, 3);
            }
        });
        UI_CHECK(waitFor(a, [&] { return ui.touchPanning(); }));
        UI_CHECK(ui.scroll(area).y > y0 + 20.f); // the content followed the finger (minus the slop)
        run([&] {
            S.post(g.touch, 4); // cancel: the compositor took the gesture
        });
        UI_CHECK(waitFor(a, [&] { return !ui.touchPanning(); }));
        UI_CHECK(!ui.flinging() && ui.touchCount() == 0 && g_clicks == 2);

        // ---- clipboard ----
        UI_CHECK(native.setClipboardText("copied by fuse"));
        UI_CHECK(waitFor(a, [&] { return locked(&State::selectionSets) == 1u; }));
        {
            std::lock_guard lk(mu);
            UI_CHECK(std::find(g.sourceMimes.begin(), g.sourceMimes.end(), "text/plain;charset=utf-8") != g.sourceMimes.end());
            UI_CHECK(std::find(g.sourceMimes.begin(), g.sourceMimes.end(), "UTF8_STRING") != g.sourceMimes.end());
        }
        std::string own;
        UI_CHECK(native.clipboardText(own) && own == "copied by fuse"); // our own selection: no round trip
        UI_CHECK(ui.setClipboardText("from the widget") && native.clipboardText(own) && own == "from the widget"); // the window's hooks
        {
            int fds[2];
            UI_CHECK(::pipe(fds) == 0);
            run([&] { // another client pastes: the compositor asks our source
                S.post(g.selection, 1, "text/plain;charset=utf-8", fds[1]);
                ::close(fds[1]);
            });
            std::string got;
            ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL) | O_NONBLOCK);
            UI_CHECK(waitFor(a, [&] {
                char buf[256];
                for (;;) {
                    const ssize_t n = ::read(fds[0], buf, sizeof buf);
                    if (n > 0) got.append(buf, std::size_t(n));
                    else return n == 0; // EOF: the source wrote everything and closed
                }
            }));
            ::close(fds[0]);
            UI_CHECK(got == "from the widget");
        }
        // another client copies: our source is cancelled, the new offer is read through a pipe
        g_offerPayload = "from another client";
        run([&] {
            S.post(g.selection, 2); // cancelled
            R* offer = make(g.client, S.offer, 3, 0, offerImpl);
            S.post(g.device, 0, offer);                       // data_offer
            S.post(offer, 0, "text/html");
            S.post(offer, 0, "text/plain;charset=utf-8");
            S.post(g.device, 5, offer);                       // selection
        });
        settle(a);
        std::string other;
        UI_CHECK(native.clipboardText(other));
        UI_CHECK(other == "from another client" && locked(&State::received) == "text/plain;charset=utf-8");

        // ---- clipboard formats (NativePlatform::setClipboard / clipboardFormats / clipboardData) ----
        {
            UI_CHECK(!native.clipboardOwned());
            const std::vector<std::string> theirs = native.clipboardFormats();
            UI_CHECK(theirs.size() == 2 && theirs[0] == "text/html" && theirs[1] == app::kClipboardText);
            g_offerPayload = "<b>bold</b>";
            std::vector<u8> html;
            UI_CHECK(native.clipboardData("text/html", html) && std::string(html.begin(), html.end()) == "<b>bold</b>");
            UI_CHECK(locked(&State::received) == "text/html");
            UI_CHECK(!native.clipboardData("image/png", html)); // not offered

            static int made = 0;
            std::vector<app::ClipboardItem> items(3);
            items[0].mime = app::kClipboardText;
            items[0].text = "three formats";
            items[1].mime = "application/x-fuse-gate";
            items[1].provide = [](void*, app::ClipboardItem& it) { // lazy: made when first pasted
                ++made;
                it.bytes = {1, 2, 3, 250};
                return true;
            };
            items[2].mime = app::kClipboardImage;
            items[2].image.resize(3, 2, 0xFF2040C0u);
            items[2].image.pixels[4] = 0x80FFFFFFu;
            const Image sent = items[2].image;
            const u32 setsBefore = locked(&State::selectionSets);
            UI_CHECK(native.setClipboard(std::move(items)));
            UI_CHECK(waitFor(a, [&] { return locked(&State::selectionSets) == setsBefore + 1u; }));
            {
                std::lock_guard lk(mu);
                UI_CHECK(std::find(g.sourceMimes.begin(), g.sourceMimes.end(), "image/png") != g.sourceMimes.end());
                UI_CHECK(std::find(g.sourceMimes.begin(), g.sourceMimes.end(), "application/x-fuse-gate") != g.sourceMimes.end());
            }
            UI_CHECK(native.clipboardOwned());
            const std::vector<std::string> ours = native.clipboardFormats();
            UI_CHECK(ours.size() == 3 && ours[0] == app::kClipboardText && ours[1] == "application/x-fuse-gate" && ours[2] == app::kClipboardImage);
            std::string text;
            UI_CHECK(native.clipboardText(text) && text == "three formats" && made == 0);
            std::vector<u8> bytes;
            UI_CHECK(native.clipboardData("application/x-fuse-gate", bytes) && bytes == std::vector<u8>({1, 2, 3, 250}) && made == 1);
            UI_CHECK(native.clipboardData("application/x-fuse-gate", bytes) && made == 1); // made once
            Image back;
            UI_CHECK(native.clipboardImage(back) && back.width == 3 && back.height == 2 && back.pixels == sent.pixels);
            // another client pastes the image: the compositor asks our source for image/png
            int fds[2];
            UI_CHECK(::pipe(fds) == 0);
            run([&] {
                S.post(g.selection, 1, "image/png", fds[1]);
                ::close(fds[1]);
            });
            std::vector<u8> png;
            ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL) | O_NONBLOCK);
            UI_CHECK(waitFor(a, [&] {
                u8 buf[256];
                for (;;) {
                    const ssize_t n = ::read(fds[0], buf, sizeof buf);
                    if (n > 0) png.insert(png.end(), buf, buf + n);
                    else return n == 0;
                }
            }));
            ::close(fds[0]);
            Image decoded;
            UI_CHECK(png.size() > 8 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
            UI_CHECK(app::decodePng(png, decoded) && decoded.pixels == sent.pixels);
            UI_CHECK(native.setClipboardImage(sent) && native.clipboardFormats() == std::vector<std::string>{std::string(app::kClipboardImage)});
        }
        // ---- primary selection (zwp_primary_selection_v1) ----
        {
            UI_CHECK(wl::hasDataDevice(b, true));
            std::string sel;
            UI_CHECK(!native.primarySelectionText(sel)); // nobody owns it yet
            UI_CHECK(native.setPrimarySelectionText("selected text"));
            UI_CHECK(waitFor(a, [&] { return locked(&State::primarySets) == 1; }));
            {
                std::lock_guard lk(mu);
                UI_CHECK(std::find(g.primaryMimes.begin(), g.primaryMimes.end(), "text/plain;charset=utf-8") != g.primaryMimes.end());
            }
            UI_CHECK(native.primarySelectionText(sel) && sel == "selected text");
            UI_CHECK(native.clipboardOwned() && native.clipboardFormats().size() == 1); // the clipboard is another selection
            int fds[2];
            UI_CHECK(::pipe(fds) == 0);
            run([&] { // a middle click in another client
                S.post(g.primarySource, 0, "UTF8_STRING", fds[1]);
                ::close(fds[1]);
            });
            std::string got;
            ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL) | O_NONBLOCK);
            UI_CHECK(waitFor(a, [&] {
                char buf[256];
                for (;;) {
                    const ssize_t n = ::read(fds[0], buf, sizeof buf);
                    if (n > 0) got.append(buf, std::size_t(n));
                    else return n == 0;
                }
            }));
            ::close(fds[0]);
            UI_CHECK(got == "selected text");
        }
        // ---- synthetic input (postPointerMove / postPointerButton): delivered in-process, in device pixels ----
        {
            const int before = g_clicks;
            const unsigned long long h = native.nativeWindow(w);
            const Vec2 at{pb.x * w.scale(), pb.y * w.scale()};
            UI_CHECK(native.postPointerMove(h, at) && native.postPointerButton(h, at, true) && native.postPointerButton(h, at, false));
            UI_CHECK(waitFor(a, [&] { return g_clicks == before + 1; }));
            UI_CHECK(!native.postPointerMove(12345ull, at) && !native.postClose(12345ull)); // not a window of ours
            g_clicks = before; // the sections below count from here
        }

        // ---- drop from another client onto the scroll area ----
        g_offerPayload = "file:///tmp/fuse%20wl/a.png\r\nfile:///tmp/b.txt\r\n";
        run([&] {
            R* offer = make(g.client, S.offer, 3, 0, offerImpl);
            S.post(g.device, 0, offer);
            S.post(offer, 0, "text/uri-list");
            S.post(g.device, 1, serial(), g.surface, fx(ps.x), fx(ps.y), offer); // enter
            S.post(g.device, 3, 300u, fx(ps.x), fx(ps.y + 4.f));                 // motion
        });
        UI_CHECK(waitFor(a, [&] { return locked(&State::accepted) == "text/uri-list"; }));
        UI_CHECK(wl::dragOver(win));
        UI_CHECK(ui.states(area) & kStateDragOver); // hover feedback while the drag is over the window
        run([&] { S.post(g.device, 4); }); // drop
        UI_CHECK(waitFor(a, [&] { return !g_dropText.empty(); }));
        UI_CHECK(g_dropText == "/tmp/fuse wl/a.png\n/tmp/b.txt" && g_dropType == kDragFiles);
        UI_CHECK(!wl::dragOver(win) && !(ui.states(area) & kStateDragOver));

        // ---- fractional scale: the compositor prefers 1.5 ----
        run([&] { S.post(g.frac, 0, 180u); }); // preferred_scale, in 120ths
        UI_CHECK(waitFor(a, [&] { std::lock_guard lk(mu); return w.scale() == 1.5f && g.shown.width == 540; }));
        settle(a);
        {
            const Image ref = w.rasterize({});
            std::lock_guard lk(mu);
            UI_CHECK(g.shown.width == 540 && g.shown.height == 360 && g.destW == 360 && g.destH == 240);
            UI_CHECK(differing(g.shown, ref) == 0);
        }
        UI_CHECK(wl::fractional(win) && wl::bufferScale(win) == 1 && native.scale(w) == 1.5f);
        // pointer positions stay logical at 1.5x
        run([&] {
            S.post(g.pointer, 2, 400u, fx(pb.x), fx(pb.y));
            S.post(g.pointer, 5);
            S.post(g.pointer, 3, serial(), 401u, 0x110u, 1u);
            S.post(g.pointer, 5);
            S.post(g.pointer, 3, serial(), 402u, 0x110u, 0u);
            S.post(g.pointer, 5);
        });
        UI_CHECK(waitFor(a, [&] { return g_clicks == 3; }));

        // ---- window state from the compositor ----
        w.maximize();
        UI_CHECK(waitFor(a, [&] { return locked(&State::maximizeAsked) == 1; }));
        UI_CHECK(w.mode() == app::WindowMode::Normal); // a request: the state follows the configure
        run([&] {
            uint32_t st[2] = {1u /*maximized*/, 4u /*activated*/};
            Array states{sizeof st, sizeof st, st};
            S.post(g.top, 0, 800, 560, &states);
            S.post(g.xsurf, 0, serial());
        });
        UI_CHECK(waitFor(a, [&] { std::lock_guard lk(mu); return w.mode() == app::WindowMode::Maximized && g.shown.width == 1200; }));
        UI_CHECK(w.size().w == 800.f && w.size().h == 560.f && w.active());
        run([&] {
            Array states{0, 0, nullptr};
            S.post(g.top, 0, 0, 0, &states); // restored, not active, "your size"
            S.post(g.xsurf, 0, serial());
        });
        UI_CHECK(waitFor(a, [&] { return w.mode() == app::WindowMode::Normal && !w.active(); }));
        UI_CHECK(w.size().w == 360.f && w.size().h == 240.f); // (0, 0) = the client's choice: the size from before
        w.activate();                  // cannot: no xdg-activation token
        UI_CHECK(!w.active());

        // ---- client-side chrome: a compositor that does not draw frames (xdg-decoration answers "client side") ----
        R* firstSurface = nullptr;
        run([&] {
            firstSurface = g.surface;
            S.post(g.pointer, 1, serial(), g.surface); // leave the first window
            S.post(g.pointer, 5);
            g.surface = nullptr; // the compositor follows the next toplevel from here
            g.top = g.xsurf = g.deco = nullptr;
            g.configured = false;
            g.decoAnswer = 1u;
            g.shapes.clear();
        });
        {
            const int getsBefore = locked(&State::decoGets);
            auto& w2 = a.createWindow({.title = "bare", .width = 300, .height = 200});
            UiContext& u2 = w2.ui();
            u2.setCss(u2.root(), "flex-direction", "column");
            const NodeHandle body = wg::button(u2, u2.root(), "Body", "body");
            UI_CHECK(native.show(w2) == app::PresenterKind::Software);
            wl::Window* win2 = wl::findWindow(b, w2);
            UI_CHECK(win2 != nullptr && !wl::serverDecorated(win2));
            UI_CHECK(locked(&State::decoGets) == getsBefore + 1 && locked(&State::decoMode) == 2); // asked; the answer was "client side"
            // the fallback: Custom frame, a title bar on top of the root, the title in it
            const NodeHandle bar = u2.find("titlebar");
            UI_CHECK(w2.chrome().frame == app::WindowFrame::Custom && w2.chrome().resizeBorder == 6.f);
            UI_CHECK(u2.alive(bar) && u2.firstChild(u2.root()) == bar && w2.chrome().caption == bar);
            UI_CHECK(u2.text(u2.find("titlebar.title")) == "bare");
            w2.setTitle("renamed");
            UI_CHECK(u2.text(u2.find("titlebar.title")) == "renamed");
            UI_CHECK(waitFor(a, [&] { return locked(&State::title) == "renamed" && w2.framesPresented() >= 1; }));
            settle(a);
            UI_CHECK(u2.rect(bar).y == 0.f && u2.rect(bar).h == 32.f && u2.rect(body).y >= 32.f);
            const Vec2 cap{120.f, 16.f};
            UI_CHECK(app::chromeHitTest(w2, cap) == app::HitArea::Caption);
            UI_CHECK(app::chromeHitTest(w2, {2.f, 100.f}) == app::HitArea::Left);
            // an edge: the resize cursor, and a press is handed to the compositor as xdg_toplevel.resize(left)
            uint32_t press = 0;
            run([&] {
                S.post(g.pointer, 0, serial(), g.surface, fx(2.f), fx(100.f)); // enter on the left edge
                S.post(g.pointer, 5);
            });
            UI_CHECK(waitFor(a, [&] { return native.cursor(w2) == Cursor::ResizeH; }));
            run([&] {
                press = serial();
                S.post(g.pointer, 3, press, 500u, 0x110u, 1u);
                S.post(g.pointer, 5);
            });
            UI_CHECK(waitFor(a, [&] { return locked(&State::resizes) == 1; }));
            UI_CHECK(locked(&State::resizeEdge) == 4u && locked(&State::resizeSerial) == press && wl::chromeResizes(win2) == 1u);
            run([&] { // the compositor owns the pointer during the drag: the client sees it leave and come back
                S.post(g.pointer, 1, serial(), g.surface);
                S.post(g.pointer, 5);
                S.post(g.pointer, 0, serial(), g.surface, fx(298.f), fx(198.f)); // bottom-right corner
                S.post(g.pointer, 5);
                press = serial();
                S.post(g.pointer, 3, press, 510u, 0x110u, 1u);
                S.post(g.pointer, 5);
            });
            UI_CHECK(waitFor(a, [&] { return locked(&State::resizes) == 2; }));
            UI_CHECK(locked(&State::resizeEdge) == 10u && locked(&State::resizeSerial) == press);
            UI_CHECK(native.cursor(w2) == Cursor::ResizeNWSE);
            // the caption: xdg_toplevel.move with the serial of the press; the UI does not see the press
            run([&] {
                S.post(g.pointer, 1, serial(), g.surface);
                S.post(g.pointer, 5);
                S.post(g.pointer, 0, serial(), g.surface, fx(cap.x), fx(cap.y));
                S.post(g.pointer, 5);
                press = serial();
                S.post(g.pointer, 3, press, 520u, 0x110u, 1u);
                S.post(g.pointer, 5);
            });
            UI_CHECK(waitFor(a, [&] { return locked(&State::moves) == 1; }));
            UI_CHECK(locked(&State::moveSerial) == press && wl::chromeMoves(win2) == 1u && !u2.alive(u2.pressed()));
            UI_CHECK(native.cursor(w2) == Cursor::Arrow);
            // a second caption press right after it is a double click: maximise is asked for, no second move
            const int maxBefore = locked(&State::maximizeAsked);
            run([&] {
                S.post(g.pointer, 3, serial(), 530u, 0x110u, 0u);
                S.post(g.pointer, 5);
                S.post(g.pointer, 3, serial(), 560u, 0x110u, 1u);
                S.post(g.pointer, 5);
                S.post(g.pointer, 3, serial(), 570u, 0x110u, 0u);
                S.post(g.pointer, 5);
            });
            UI_CHECK(waitFor(a, [&] { return locked(&State::maximizeAsked) == maxBefore + 1; }));
            UI_CHECK(locked(&State::moves) == 1);
            // the body still gets its clicks; the title bar's close button closes the window
            int bodyClicks = 0;
            u2.connect(body, SignalKind::Clicked, [&bodyClicks](UiContext&, NodeHandle, const SignalArgs&) { ++bodyClicks; });
            const Vec2 pbody = centre(u2, body), pclose = centre(u2, u2.find("titlebar.close"));
            UI_CHECK(app::chromeHitTest(w2, pclose) == app::HitArea::Close);
            run([&] {
                S.post(g.pointer, 2, 600u, fx(pbody.x), fx(pbody.y));
                S.post(g.pointer, 5);
                S.post(g.pointer, 3, serial(), 601u, 0x110u, 1u);
                S.post(g.pointer, 5);
                S.post(g.pointer, 3, serial(), 602u, 0x110u, 0u);
                S.post(g.pointer, 5);
            });
            UI_CHECK(waitFor(a, [&] { return bodyClicks == 1; }));
            UI_CHECK(a.windowCount() == 2);
            run([&] {
                S.post(g.pointer, 2, 610u, fx(pclose.x), fx(pclose.y));
                S.post(g.pointer, 5);
                S.post(g.pointer, 3, serial(), 611u, 0x110u, 1u);
                S.post(g.pointer, 5);
                S.post(g.pointer, 3, serial(), 612u, 0x110u, 0u);
                S.post(g.pointer, 5);
            });
            UI_CHECK(waitFor(a, [&] { return a.windowCount() == 1; }));
        }
        // a window that asks for its own frame: no decoration object at all, and nothing is added to its UI
        run([&] {
            g.surface = nullptr;
            g.top = g.xsurf = nullptr;
            g.configured = false;
        });
        {
            const int getsBefore = locked(&State::decoGets);
            auto& w3 = a.createWindow({.title = "own frame", .width = 200, .height = 120, .frame = app::WindowFrame::Borderless});
            UI_CHECK(native.show(w3) == app::PresenterKind::Software);
            UI_CHECK(locked(&State::decoGets) == getsBefore && !wl::serverDecorated(wl::findWindow(b, w3)));
            UI_CHECK(w3.chrome().frame == app::WindowFrame::Borderless && !w3.ui().alive(w3.ui().find("titlebar")));
            UI_CHECK(waitFor(a, [&] { return w3.framesPresented() >= 1; }));
            UI_CHECK(native.postClose(native.nativeWindow(w3))); // what the compositor's close button sends
            UI_CHECK(waitFor(a, [&] { return a.windowCount() == 1; }));
        }
        (void)firstSurface;

        // ---- the compositor goes away: the window is asked to close, the loop keeps running ----
        stop = true;
        th.join();
        S.destroyClients(g_dpy);
        S.destroyDisplay(g_dpy);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 200 && a.windowCount() > 0; ++i) a.step(0.005f);
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  compositor gone: %u windows left after %.2f s (%.*s)\n", a.windowCount(), took, int(native.lastError().size()),
                    native.lastError().data());
        UI_CHECK(a.windowCount() == 0);
        UI_CHECK(!native.lastError().empty());
        auto& again = a.createWindow({.title = "after", .width = 64, .height = 48});
        UI_CHECK(native.show(again) == app::PresenterKind::Headless); // no window system any more
    }
    const std::string rm = "rm -rf '" + dir + "'";
    [[maybe_unused]] int rc = std::system(rm.c_str());
    std::printf("fuse_app_wayland_seat: %s\n", fuse::ui::test::failures() ? "FAIL" : "pass");
    return fuse::ui::test::failures() ? 1 : 0;
}
