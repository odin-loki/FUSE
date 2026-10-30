// fuse_app_weston: the Wayland backend against a real compositor: weston (x11 backend, pixman renderer, kiosk shell)
// nested in this test's Xvfb, with the pointer driven through xdotool (XTEST -> weston's X window -> wl_pointer).
// Checks what the in-process compositor of fuse_app_wayland cannot prove: weston accepts our requests without a
// protocol error (it would drop the connection), namely
//   * the pointer cursor (cursor-shape-v1 or a libwayland-cursor surface) following an in-app widget drag
//     (Forbidden over the source, Move over a target, Default after the drop)
//   * an OS drag (wl_data_device.start_drag with the button serial, three formats and a pixmap icon surface)
//     dropped back on our own surface by weston's drag grab: every format arrives, the source sees dnd_finished.
//   * a floating dock as a wl_subsurface tool surface (osPopups): its committed pixels equal the CPU raster of its
//     popup surface, the main surface does not draw it, and a real pointer drag on its title (weston delivering
//     events to the subsurface) moves the dock and the subsurface with it.
// Exit 77 without weston / xdotool / a display, or when weston does not come up.
#include "fuse/app/wayland.hpp"
#include "fuse/ui/os.hpp"
#include "fuse/ui/widgets/widgets.hpp"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
using namespace fuse::ui;
namespace app = fuse::app;
namespace w = fuse::ui::widgets;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
void xdo(const std::string& args) { std::string c = "xdotool " + args + " >/dev/null 2>&1"; (void)!std::system(c.c_str()); }
bool exists(const std::string& p) { struct stat st{}; return ::stat(p.c_str(), &st) == 0; }
struct Weston {
    pid_t pid = -1;
    ~Weston() {
        if (pid > 0) { ::kill(pid, SIGTERM); int st = 0; ::waitpid(pid, &st, 0); }
    }
};
} // namespace

int main(int argc, char** argv) {
    if (os::env("DISPLAY").empty()) { std::printf("fuse_app_weston: skipped (no DISPLAY)\n"); return 77; }
    if (std::system("command -v weston >/dev/null 2>&1 && command -v xdotool >/dev/null 2>&1") != 0) {
        std::printf("fuse_app_weston: skipped (no weston / xdotool)\n");
        return 77;
    }
    const std::string run = os::env("XDG_RUNTIME_DIR");
    if (run.empty()) { std::printf("fuse_app_weston: skipped (no XDG_RUNTIME_DIR)\n"); return 77; }
    const std::string sock = "fuse-weston-" + std::to_string(::getpid());
    const std::string log = "--log=" + (os::env("TMPDIR").empty() ? std::string("/tmp") : os::env("TMPDIR")) + "/" + sock + ".log";
    const std::string sockArg = "--socket=" + sock;
    const char* args[] = {"weston", "--backend=x11", "--renderer=pixman", "--width=640", "--height=480", sockArg.c_str(),
                          "--idle-time=0", "--shell=kiosk", log.c_str(), nullptr};
    Weston wst;
    if (::posix_spawnp(&wst.pid, "weston", nullptr, nullptr, const_cast<char**>(args), environ) != 0) {
        std::printf("fuse_app_weston: skipped (cannot start weston)\n");
        return 77;
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!exists(run + "/" + sock) && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!exists(run + "/" + sock)) { std::printf("fuse_app_weston: skipped (weston did not come up)\n"); return 77; }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    xdo("search --name \"Weston Compositor\" windowmove 0 0");

    app::Application a{{.name = "gate_weston", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativeOptions no;
    no.vulkan = false;
    no.osPopups = true;
    app::WaylandPlatform wlp(a, sock.c_str(), no);
    if (!wlp.available()) {
        std::printf("fuse_app_weston: skipped (%.*s)\n", int(wlp.lastError().size()), wlp.lastError().data());
        return 77;
    }
    auto& win = a.createWindow({.title = "fuse weston", .width = 640, .height = 480});
    UiContext& ui = win.ui();
    const NodeHandle source = w::panel(ui, ui.root());
    ui.setCss(source, "width", "200px");
    ui.setCss(source, "height", "120px");
    const NodeHandle target = w::panel(ui, ui.root());
    ui.setCss(target, "width", "200px");
    ui.setCss(target, "height", "80px");
    u8 finished = 0xFF;
    w::setDragSource(ui, source, [](UiContext&, NodeHandle, Vec2, w::DragPayload& p) {
        p.set("text/plain", "inside"); p.actions = w::DropCopy | w::DropMove; return true;
    }, [&](UiContext&, NodeHandle, u8 act) { finished = act; });
    int targetDrops = 0;
    w::setDropTarget(ui, target, [&](UiContext&, NodeHandle, const w::DropEvent& e) -> u8 {
        if (e.phase == w::DropPhase::Drop) ++targetDrops;
        return e.phase == w::DropPhase::Leave ? 0 : u8(w::DropMove);
    });
    std::vector<app::AppWindow::ExternalDrop> drops;
    int dragEnd = -1;
    win.onExternalDrop([&](app::AppWindow&, const app::AppWindow::ExternalDrop& d) { drops.push_back(d); });
    win.onExternalDragEnd([&](bool ok) { dragEnd = ok ? 1 : 0; });
    CHECK(wlp.show(win));
    auto pump = [&](int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) { a.step(0.005f); if (!wlp.roundtrip()) break; }
    };
    for (int i = 0; i < 300 && !wlp.configured(win); ++i) pump(10);
    CHECK(wlp.configured(win));
    pump(200);
    std::printf("fuse_app_weston: window %gx%g\n", double(win.size().w), double(win.size().h));

    // ---- in-app widget drag: cursor feedback through weston ----
    xdo("mousemove 40 40"); pump(150);
    xdo("mousedown 1"); pump(80);
    xdo("mousemove 60 50"); pump(80);
    xdo("mousemove 80 60"); pump(150);
    CHECK(w::dragActive(ui));
    CHECK(ui.cursorShape() == CursorShape::Forbidden);
    const Rect tr = ui.rect(target);
    xdo("mousemove " + std::to_string(int(tr.x) + 30) + " " + std::to_string(int(tr.y) + 30)); pump(200);
    CHECK(ui.cursorShape() == CursorShape::Move);
    xdo("mouseup 1"); pump(200);
    CHECK(finished == w::DropMove && targetDrops == 1);
    CHECK(ui.cursorShape() == CursorShape::Default);
    const std::string via(wlp.cursorBackend());
    CHECK(!via.empty());
    CHECK(wlp.roundtrip()); // no protocol error from the cursor requests

    // ---- OS drag through weston's drag grab, dropped back on our surface ----
    xdo("mousemove 400 300"); pump(150);
    xdo("mousedown 1"); pump(150);
    app::AppWindow::ExternalDrag d{.mime = "text/plain", .data = "via weston"};
    d.formats = {{"application/x-fuse-test", "42"}, {"text/html", "<b>via</b>"}};
    d.image.assign(24 * 16, 0xC0204080u);
    d.imageW = 24; d.imageH = 16; d.hotX = 6; d.hotY = 4;
    CHECK(win.beginExternalDrag(std::move(d)));
    xdo("mousemove 410 310"); pump(200); // the backend starts the drag on this motion
    xdo("mousemove 450 330"); pump(200);
    xdo("mousemove 470 340"); pump(200);
    xdo("mouseup 1");
    for (int i = 0; i < 300 && (drops.empty() || dragEnd < 0); ++i) pump(10);
    CHECK(drops.size() == 1);
    if (!drops.empty()) {
        CHECK(drops[0].mime == "text/plain" && drops[0].data == "via weston");
        int found = 0;
        for (const auto& [m, v] : drops[0].formats) found += (m == "application/x-fuse-test" && v == "42") + (m == "text/html" && v == "<b>via</b>");
        CHECK(found == 2);
    }
    CHECK(dragEnd == 1);
    CHECK(wlp.roundtrip());

    // ---- floating dock = wl_subsurface tool surface ----
    {
        const NodeHandle area = w::dockArea(ui, ui.root(), "wdocks");
        const NodeHandle dock = w::dockWidget(ui, area, "Tool dock", w::DockSide::Left, "wdock");
        w::dockFloat(ui, dock, {300.f, 200.f, 220.f, 130.f});
        win.requestRedraw();
        for (int i = 0; i < 200 && wlp.toolSurfaceCount(win) != 1; ++i) pump(10);
        pump(100);
        CHECK(ui.osToolWindows() && wlp.toolSurfaceCount(win) == 1);
        Image shown, ref;
        Rect tr2{};
        CHECK(wlp.captureToolSurface(win, 0, shown, &tr2));
        CHECK(tr2.x == 300.f && tr2.y == 200.f && tr2.w == 220.f && tr2.h == 130.f);
        bool inMain = false;
        for (const DrawCmd& c : ui.drawList().cmds()) inMain |= c.node == ui.firstChild(dock).index;
        CHECK(!inMain);
        int err = 255;
        if (ui.popupSurfaces().size() == 1) {
            const auto& ps = ui.popupSurfaces()[0];
            u32 clear = 0xFF000000u;
            for (const DrawCmd& c : ps.draw.cmds())
                if (c.op != DrawOp::Shadow && c.fill.a > 0.9f) { clear = packPremultiplied(c.fill) | 0xFF000000u; break; }
            ref.resize(int(tr2.w), int(tr2.h), clear);
            CpuRasterizer rz = win.rasterizer();
            rz.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
            rz.render(ps.draw, ref);
            if (shown.width == ref.width && shown.height == ref.height) {
                err = 0;
                for (std::size_t i = 0; i < ref.pixels.size(); ++i)
                    for (int c = 0; c < 3; ++c)
                        err = std::max(err, std::abs(int((shown.pixels[i] >> (8 * c)) & 0xFF) - int((ref.pixels[i] >> (8 * c)) & 0xFF)));
            }
        }
        CHECK(err <= 2);
        // drag its title with the real pointer
        const Rect tt = ui.rect(ui.firstChild(dock));
        const int gx = int(tt.x) + 30, gy = int(tt.y + tt.h * 0.5f);
        xdo("mousemove " + std::to_string(gx) + " " + std::to_string(gy)); pump(150);
        xdo("mousedown 1"); pump(100);
        xdo("mousemove " + std::to_string(gx + 20) + " " + std::to_string(gy + 10)); pump(100);
        xdo("mousemove " + std::to_string(gx + 40) + " " + std::to_string(gy + 20)); pump(150);
        xdo("mouseup 1"); pump(200);
        const Rect moved = ui.rect(dock);
        Rect tr3{};
        Image after;
        CHECK(wlp.captureToolSurface(win, 0, after, &tr3));
        std::printf("fuse_app_weston: tool subsurface %gx%g err %d; dragged to %g,%g (subsurface %g,%g)\n", double(tr2.w), double(tr2.h), err,
                    double(moved.x), double(moved.y), double(tr3.x), double(tr3.y));
        CHECK(std::abs(moved.x - 340.f) < 1.f && std::abs(moved.y - 220.f) < 1.f && tr3.x == moved.x && tr3.y == moved.y);
        w::dockTo(ui, dock, w::DockSide::Left);
        win.requestRedraw();
        for (int i = 0; i < 100 && wlp.toolSurfaceCount(win) != 0; ++i) pump(10);
        CHECK(wlp.toolSurfaceCount(win) == 0);
        CHECK(wlp.roundtrip());
    }
    std::printf("fuse_app_weston: cursor via %s, drops=%zu formats=%zu dragEnd=%d %s\n", via.c_str(), drops.size(),
                drops.empty() ? std::size_t(0) : drops[0].formats.size(), dragEnd, g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
