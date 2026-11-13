// fuse_app_xdnd: X11 system drag and drop end to end under Xvfb. A window drags file paths out (XDND source) and
// the drop lands back in a FUSE window (XDND target): Enter / Position / Status / Drop / SelectionRequest /
// SelectionNotify / Finished all go through the X server. The pointer is driven with xdotool (XTest).
// Exit 77 (skip) without a display or xdotool.
#include "fuse/app/native.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace fuse::ui;
namespace app = fuse::app;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void xdo(const std::string& args) { std::string c = "xdotool " + args + " >/dev/null 2>&1"; (void)!std::system(c.c_str()); }

int main(int argc, char** argv) {
    if (std::system("command -v xdotool >/dev/null 2>&1") != 0) { std::printf("fuse_app_xdnd: skipped (no xdotool)\n"); return 77; }
    app::Application a{{.name = "gate_xdnd", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform native(a);
    if (!native.available()) { std::printf("fuse_app_xdnd: skipped (no display)\n"); return 77; }
    auto& win = a.createWindow({.title = "xdnd", .width = 300, .height = 200});
    std::vector<std::string> got;
    std::string mime;
    int ended = -1;
    win.onExternalDrop([&](app::AppWindow&, const app::AppWindow::ExternalDrop& d) { got = d.paths; mime = d.mime; });
    win.onExternalDragEnd([&](bool ok) { ended = ok ? 1 : 0; });
    native.show(win);
    for (int i = 0; i < 400 && win.framesPresented() < 1; ++i) a.step(0.01f);
    for (int i = 0; i < 20; ++i) a.step(0.005f);
    const Vec2 o = native.screenOrigin(win);
    auto at = [&](int dx, int dy) { return std::to_string(int(o.x) + dx) + " " + std::to_string(int(o.y) + dy); };
    auto pump = [&](int n) { for (int i = 0; i < n; ++i) a.step(0.005f); };
    xdo("mousemove " + at(40, 40)); pump(10);
    xdo("mousedown 1"); pump(10);
    CHECK(win.beginExternalDrag({.paths = {"/tmp/fuse drag/a.png", "/tmp/b.txt"}}));
    xdo("mousemove " + at(60, 50)); pump(20); // first motion starts the drag (grab + selection owner)
    xdo("mousemove " + at(120, 90)); pump(20);
    xdo("mousemove " + at(150, 100)); pump(20);
    xdo("mouseup 1");
    for (int i = 0; i < 200 && ended < 0; ++i) a.step(0.005f);
    CHECK(ended == 1);
    CHECK(mime == "text/uri-list");
    CHECK(got.size() == 2 && got[0] == "/tmp/fuse drag/a.png" && got[1] == "/tmp/b.txt");
    std::printf("fuse_app_xdnd: drop paths=%zu ended=%d %s\n", got.size(), ended, g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
