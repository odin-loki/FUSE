// fuse_app_x11_widget_dnd: a widget drag (widgets::setDragSource) on a real X server (Xvfb): the in-app drag shows
// the drag cursor (read back with XFixesGetCursorImage), and leaving the window hands it to XDND
// (setExternalDragHandler -> AppWindow::beginExternalDrag); a second FUSE window receives the drop with every
// payload format; the source's DragFinished gets Copy and the cursor returns to the default. Pointer via xdotool.
// Exit 77 without a display, xdotool or libXfixes.
#include "fuse/app/native.hpp"
#include "fuse/ui/os.hpp"
#include "fuse/ui/widgets/widgets.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

using namespace fuse::ui;
namespace app = fuse::app;
namespace w = fuse::ui::widgets;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
void xdo(const std::string& args) { std::string c = "xdotool " + args + " >/dev/null 2>&1"; (void)!std::system(c.c_str()); }
struct CursorImage { short x, y; unsigned short width, height, xhot, yhot; unsigned long serial; unsigned long* pixels; unsigned long atom; const char* name; };
struct Fixes {
    std::unique_ptr<os::Library> x11, xf;
    void* (*open)(const char*) = nullptr;
    CursorImage* (*get)(void*) = nullptr;
    int (*xfree)(void*) = nullptr;
    void* dpy = nullptr;
    bool load() {
        x11 = os::loadLibrary({"libX11.so.6"});
        xf = os::loadLibrary({"libXfixes.so.3"});
        if (!x11 || !xf) return false;
        open = reinterpret_cast<decltype(open)>(x11->symbol("XOpenDisplay"));
        xfree = reinterpret_cast<decltype(xfree)>(x11->symbol("XFree"));
        get = reinterpret_cast<decltype(get)>(xf->symbol("XFixesGetCursorImage"));
        return open && get && xfree && (dpy = open(nullptr));
    }
    /// pixel hash of the current cursor (the serial changes on every XDefineCursor; the image tells the shape)
    unsigned long long shape() {
        CursorImage* c = get(dpy);
        if (!c) return 0;
        unsigned long long h = 1469598103934665603ull ^ ((static_cast<unsigned long long>(c->width) << 16 | c->height));
        for (int i = 0; i < c->width * c->height; ++i) h = (h ^ (c->pixels[i] & 0xFFFFFFFFu)) * 1099511628211ull;
        xfree(c);
        return h;
    }
};
} // namespace

int main(int argc, char** argv) {
    if (std::system("command -v xdotool >/dev/null 2>&1") != 0) { std::printf("fuse_app_x11_widget_dnd: skipped (no xdotool)\n"); return 77; }
    Fixes fx;
    if (!fx.load()) { std::printf("fuse_app_x11_widget_dnd: skipped (no display / libXfixes)\n"); return 77; }
    app::Application a{{.name = "gate_x11_widget_dnd", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform native(a);
    if (!native.available()) { std::printf("fuse_app_x11_widget_dnd: skipped (no display)\n"); return 77; }
    auto& src = a.createWindow({.title = "wdnd-src", .width = 300, .height = 200});
    auto& dst = a.createWindow({.title = "wdnd-dst", .width = 300, .height = 200});
    UiContext& ui = src.ui();
    const NodeHandle panel = w::panel(ui, ui.root());
    ui.setCss(panel, "width", "200px");
    ui.setCss(panel, "height", "120px");
    const NodeHandle target = w::panel(ui, ui.root()); // in-window target: Move accepted
    ui.setCss(target, "width", "200px");
    ui.setCss(target, "height", "40px");
    u8 finished = 0xFF;
    w::setDragSource(ui, panel, [](UiContext&, NodeHandle, Vec2, w::DragPayload& p) {
        p.set("text/plain", "hello x11"); p.set("application/x-fuse-test", "42"); p.actions = w::DropCopy | w::DropMove; return true;
    }, [&](UiContext&, NodeHandle, u8 act) { finished = act; });
    w::setDropTarget(ui, target, [](UiContext&, NodeHandle, const w::DropEvent& e) -> u8 { return e.phase == w::DropPhase::Leave ? 0 : u8(w::DropMove); });
    std::vector<app::AppWindow::ExternalDrop> drops;
    dst.onExternalDrop([&](app::AppWindow&, const app::AppWindow::ExternalDrop& d) { drops.push_back(d); });
    native.show(src);
    native.show(dst);
    for (int i = 0; i < 400 && (src.framesPresented() < 1 || dst.framesPresented() < 1); ++i) a.step(0.01f);
    xdo("search --name wdnd-dst windowmove 500 300");
    auto pump = [&](int n) { for (int i = 0; i < n; ++i) a.step(0.005f); };
    pump(40);
    const Vec2 o = native.screenOrigin(src), od = native.screenOrigin(dst);
    CHECK(od.x >= 400.f);
    auto at = [](Vec2 b, int dx, int dy) { return std::to_string(int(b.x) + dx) + " " + std::to_string(int(b.y) + dy); };
    xdo("mousemove " + at(o, 40, 40)); pump(10);
    const unsigned long long idle = fx.shape();
    xdo("mousedown 1"); pump(10);
    xdo("mousemove " + at(o, 60, 50)); pump(10);
    xdo("mousemove " + at(o, 80, 60)); pump(20);
    CHECK(w::dragActive(ui) && !w::externalDragActive(ui));
    CHECK(ui.cursorShape() == CursorShape::Forbidden); // over the source itself: no drop target -> refused
    const unsigned long long inDrag = fx.shape();
    std::printf("cursor idle=%llx drag=%llx shape=%d\n", idle, inDrag, int(ui.cursorShape()));
    // over the in-window target: Move
    const Rect tr = ui.rect(target);
    xdo("mousemove " + at(o, int(tr.x + 20.f), int(tr.y + 20.f))); pump(20);
    CHECK(ui.cursorShape() == CursorShape::Move);
    const unsigned long long moveShape = fx.shape();
    CHECK(moveShape != idle);
    // out of the window, over the other FUSE window: XDND
    xdo("mousemove " + at(o, 350, 100)); pump(20);
    CHECK(w::externalDragActive(ui));
    xdo("mousemove " + at(od, 100, 100)); pump(20);
    xdo("mousemove " + at(od, 120, 110)); pump(20);
    xdo("mouseup 1");
    for (int i = 0; i < 400 && finished == 0xFF; ++i) a.step(0.005f);
    pump(20);
    CHECK(finished == w::DropCopy);
    CHECK(!w::dragActive(ui) && ui.cursorShape() == CursorShape::Default);
    CHECK(drops.size() == 1);
    if (!drops.empty()) {
        CHECK(drops[0].mime == "text/plain" && drops[0].data == "hello x11");
        bool custom = false;
        for (const auto& [m, d] : drops[0].formats) custom |= m == "application/x-fuse-test" && d == "42";
        CHECK(custom);
    }
    xdo("mousemove " + at(o, 40, 40)); pump(20);
    CHECK(fx.shape() == idle);
    std::printf("fuse_app_x11_widget_dnd: drops=%zu finished=%d %s\n", drops.size(), int(finished), g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
