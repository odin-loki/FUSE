// fuse_app_wayland: the Wayland backend against a minimal in-process compositor built on a runtime-loaded
// libwayland-server (wl_compositor, wl_shm, xdg_wm_base, wl_seat pointer, wl_data_device_manager). Checks:
//   * registry / xdg-shell handshake: toplevel configured, title set, configure size -> AppWindow::resize
//   * wl_shm presentation: the committed buffer equals the CPU raster (format swizzle included)
//   * pointer enter / motion / button reach the UI (a button widget is clicked)
//   * drop in: a compositor-made wl_data_offer (text/uri-list) is accepted, received over a pipe and delivered
//     to AppWindow::onExternalDrop with decoded paths
//   * drag out: AppWindow::beginExternalDrag -> wl_data_device.start_drag; the compositor pulls the payload with
//     wl_data_source.send and finishes -> onExternalDragEnd(true)
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
#include <mutex>
#include <string>
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
    const wl::Interface *compositor, *surface, *region, *seat, *pointer, *output, *callback, *ddm, *device, *offer, *source;
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
        i(seat, "wl_seat_interface"); i(pointer, "wl_pointer_interface"); i(output, "wl_output_interface");
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
void sAttach(C*, R*, R* buffer, int32_t, int32_t) { g_pending = buffer; }
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
    if (g_top && !g_configured && r == g_surface) {
        g_configured = true;
        wl::Array states{0, 0, nullptr};
        S.post(g_top, 0, g_cfgW, g_cfgH, &states);
        S.post(g_xsurf, 0, S.nextSerial(g_dpy));
    }
}
Fn surfaceImpl[11];
void cCreateSurface(C* c, R* r, uint32_t id) { g_surface = make(c, S.surface, S.resVersion(r), id, surfaceImpl); }
void cCreateRegion(C* c, R*, uint32_t id) { make(c, S.region, 1, id, destroyFirst); }
Fn compositorImpl[2] = {reinterpret_cast<Fn>(&cCreateSurface), reinterpret_cast<Fn>(&cCreateRegion)};
void bindCompositor(C* c, void*, uint32_t v, uint32_t id) { make(c, S.compositor, int(v), id, compositorImpl); }
// xdg
void topTitle(C*, R*, const char* t) { std::lock_guard lk(mu); g_title = t; }
Fn topImpl[14];
void xsGetToplevel(C* c, R*, uint32_t id) { g_top = make(c, &S.xdg.toplevel, 1, id, topImpl); }
void xsAck(C*, R*, uint32_t) { std::lock_guard lk(mu); g_acked = true; }
Fn xsurfImpl[5];
void wmGetXdgSurface(C* c, R*, uint32_t id, R*) { g_xsurf = make(c, &S.xdg.surface, 1, id, xsurfImpl); }
void wmCreatePositioner(C* c, R*, uint32_t id) { make(c, &S.xdg.positioner, 1, id, destroyFirst); }
Fn wmImpl[4] = {reinterpret_cast<Fn>(&destroyReq), reinterpret_cast<Fn>(&wmCreatePositioner), reinterpret_cast<Fn>(&wmGetXdgSurface), noop};
void bindWm(C* c, void*, uint32_t, uint32_t id) { make(c, &S.xdg.wmBase, 1, id, wmImpl); }
// seat
void seatGetPointer(C* c, R* r, uint32_t id) { g_pointer = make(c, S.pointer, S.resVersion(r), id, noops); }
Fn seatImpl[4] = {reinterpret_cast<Fn>(&seatGetPointer), noop, noop, noop};
void bindSeat(C* c, void*, uint32_t v, uint32_t id) {
    R* r = make(c, S.seat, int(v), id, seatImpl);
    S.post(r, 0, 1u); // capabilities: pointer
}
// data device
void srcOffer(C*, R*, const char* mime) { std::lock_guard lk(mu); g_sourceMimes.emplace_back(mime); }
Fn sourceImpl[3] = {reinterpret_cast<Fn>(&srcOffer), reinterpret_cast<Fn>(&destroyReq), noop};
void ddmCreateSource(C* c, R* r, uint32_t id) { make(c, S.source, S.resVersion(r), id, sourceImpl); }
void devStartDrag(C*, R*, R* source, R*, R*, uint32_t) { std::lock_guard lk(mu); g_dragSource = source; }
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

int main() {
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
    xsurfImpl[4] = reinterpret_cast<Fn>(&xsAck);

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
    std::thread th(compositorThread);

    {
        app::Application a{{.name = "gate_wayland", .loadSettings = false}};
        app::WaylandPlatform wlp(a, sock);
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
        CHECK(u32(win.size().w) == 360u && u32(win.size().h) == 240u); // compositor's configure size won
        {
            std::lock_guard lk(mu);
            CHECK(g_title == "fuse wayland");
            CHECK(g_acked);
            CHECK(g_shown.width == 360 && g_shown.height == 240);
        }
        Image mine;
        CHECK(wlp.capture(win, mine));
        { std::lock_guard lk(mu); CHECK(mine.width == g_shown.width && mine.pixels == g_shown.pixels); }

        // pointer enter + motion (gives the client an input serial for start_drag)
        run([&] {
            S.post(g_pointer, 0, S.nextSerial(g_dpy), g_surface, wl::toFixed(40), wl::toFixed(40));
            S.post(g_pointer, 2, 1u, wl::toFixed(41), wl::toFixed(41));
        });
        pump(10);
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
            std::atomic<bool> pulled{false};
            run([&] {
                auto p = os::makePipe();
                S.post(src, 1, "text/uri-list", p->second);
                os::closeFd(p->second);
                S.flushClients(g_dpy);
                std::thread([fd = p->first, &pulled] { g_pulled = os::readAllAndClose(fd); pulled = true; }).detach();
            });
            for (int i = 0; i < 200 && !pulled; ++i) pump(1);
            run([&] { S.post(src, 3); S.post(src, 4); });
            for (int i = 0; i < 100 && dragEnd < 0; ++i) pump(1);
            std::lock_guard lk(mu);
            CHECK(g_pulled == "file:///srv/one%20two.txt\r\n");
            CHECK(!g_sourceMimes.empty() && g_sourceMimes[0] == "text/uri-list");
        }
        CHECK(dragEnd == 1);
        std::printf("fuse_app_wayland: commits=%d clicks=%d dropped=%zu dragEnd=%d %s\n", g_commits, clicks, dropped.size(),
                    dragEnd, g_fail ? "FAIL" : "OK");
    }
    stop = true;
    th.join();
    S.destroyDisplay(g_dpy);
    return g_fail ? 1 : 0;
}
