// fuse_app native platform-services gate (plan §4.13): real OS windows (X11 under Xvfb; the API is platform neutral).
//   * HiDPI: NativeOptions::scale = 2 gives a window of logical size x 2 pixels, rendered at that resolution (the
//     read-back equals the CPU raster), input in window pixels lands on the logical layout, OS popup windows scale
//   * mouse cursor: what UiContext::cursor() asks for is applied to the OS window after the pump
//   * clipboard: the CLIPBOARD selection between two display connections (owner serves, the other converts)
//   * window management: position, show / hide, focus, size limits and their events
//   * displays, file dialog helper (zenity found in PATH / cancelled / unavailable -> toolkit fallback), openUrl
// Exit 77 (skip) without a display. --out=<dir>: scratch directory for the fake helper programs and screenshots.
#include "fuse/app/native.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

using namespace fuse::ui;
namespace app = fuse::app;
namespace w = fuse::ui::widgets;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
struct Events {
    std::vector<app::WindowEvent::Type> types;
    bool saw(app::WindowEvent::Type t) const {
        for (auto x : types)
            if (x == t) return true;
        return false;
    }
};
void onEvent(void* u, app::AppWindow&, const app::WindowEvent& e) { static_cast<Events*>(u)->types.push_back(e.type); }
template <class Pred>
bool pumpUntil(app::Application& a, Pred done, int steps = 300) {
    for (int i = 0; i < steps; ++i) {
        if (done()) return true;
        a.step(0.01f);
    }
    return done();
}
Vec2 centre(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }

void hidpiAndCursor(const std::string& out) {
    app::Application a{{.name = "gate_services_scale", .loadSettings = false}};
    app::NativePlatform native(a, {.vulkan = false, .osPopups = true, .scale = 2.f});
    app::AppWindow& win = a.createWindow({.title = "scale 2", .width = 320, .height = 200});
    UiContext& ui = win.ui();
    const NodeHandle btn = w::button(ui, ui.root(), "Menu", "btn");
    const NodeHandle edit = w::lineEdit(ui, ui.root(), "text", "edit");
    const NodeHandle lnk = w::link(ui, ui.root(), "link", "lnk");
    int clicks = 0;
    ui.connect(btn, SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
    const app::PresenterKind kind = native.show(win);
    CHECK(kind != app::PresenterKind::Headless);
    pumpUntil(a, [&] { return win.framesPresented() >= 2; });

    // logical size is kept, the OS window and the frame are twice that
    CHECK(native.scale(win) == 2.f && win.scale() == 2.f);
    CHECK(win.size().w == 320.f && win.size().h == 200.f);
    CHECK(win.pixelSize().w == 640.f && win.pixelSize().h == 400.f);
    Image shown;
    CHECK(native.capture(win, shown));
    CHECK(shown.width == 640 && shown.height == 400);
    if (kind == app::PresenterKind::Software && shown.width == 640) { // the server shows exactly the CPU raster
        const Image& ref = win.image();
        int maxErr = 0;
        for (std::size_t i = 0; i < ref.pixels.size() && i < shown.pixels.size(); ++i)
            for (int c = 0; c < 3; ++c)
                maxErr = std::max(maxErr, std::abs(int((shown.pixels[i] >> (8 * c)) & 0xFF) - int((ref.pixels[i] >> (8 * c)) & 0xFF)));
        std::printf("  scale 2 window read-back %dx%d, max channel error vs CPU raster %d\n", shown.width, shown.height, maxErr);
        CHECK(maxErr <= 2);
        if (!out.empty()) app::writePng(shown, out + "/native_services_scale2.png");
    }

    // a real click in window pixels reaches the widget at pixels / scale
    const Vec2 bc = centre(ui.rect(btn));
    CHECK(native.postClick(native.nativeWindow(win), {bc.x * 2.f, bc.y * 2.f}));
    CHECK(pumpUntil(a, [&] { return clicks == 1; }));
    // ... and the same pixel position read as logical units would miss it
    CHECK(ui.hitTest({bc.x * 2.f, bc.y * 2.f}) != btn);

    // resizeWindow takes logical units
    CHECK(native.resizeWindow(win, 360, 220));
    CHECK(pumpUntil(a, [&] { return win.pixelSize().w == 720.f && win.pixelSize().h == 440.f; }));
    CHECK(win.size().w == 360.f && win.size().h == 220.f);

    // cursor: the UI's request is applied to the OS window by the pump
    CHECK(native.cursor(win) == Cursor::Arrow);
    ui.pointerMove(centre(ui.rect(edit)));
    a.step(0.f);
    CHECK(native.cursor(win) == Cursor::IBeam);
    ui.pointerMove(centre(ui.rect(lnk)));
    a.step(0.f);
    CHECK(native.cursor(win) == Cursor::PointingHand);
    for (int c = 1; c < kCursorCount; ++c) { // every shape can be made (themed or from the cursor font)
        ui.setCursorOverride(Cursor(c));
        a.step(0.f);
        CHECK(native.cursor(win) == Cursor(c));
    }
    ui.setCursorOverride(Cursor::Auto);
    ui.pointerMove({-1.f, -1.f});
    a.step(0.f);
    CHECK(native.cursor(win) == Cursor::Arrow);

    // an OS popup window at scale 2: placed at origin + rect x scale, rect x scale pixels, input in its pixels
    f32 picked = -1.f;
    ui.connect(btn, SignalKind::Submitted, [&picked](UiContext&, NodeHandle, const SignalArgs& s) { picked = s.value; });
    const NodeHandle scope = w::popupMenu(ui, btn, "One\nTwo\nThree\nFour", {30.f, 40.f});
    win.requestRedraw();
    CHECK(pumpUntil(a, [&] { return native.popupCount(win) == 1; }));
    if (native.popupCount(win) == 1) {
        const Rect menu = ui.rect(ui.firstChild(scope));
        const Vec2 origin = native.screenOrigin(win);
        const Rect scr = native.popupScreenRect(win, 0);
        std::printf("  popup rect %.0f,%.0f %.0fx%.0f -> window %.0f,%.0f %.0fx%.0f\n", menu.x, menu.y, menu.w, menu.h, scr.x, scr.y, scr.w, scr.h);
        CHECK(scr.x == origin.x + std::round(menu.x * 2.f) && scr.y == origin.y + std::round(menu.y * 2.f));
        CHECK(scr.w == std::ceil(menu.w * 2.f) && scr.h == std::ceil(menu.h * 2.f));
        Image pop, ref;
        CHECK(native.capturePopup(win, 0, pop));
        const auto& ps = ui.popupSurfaces()[0];
        u32 clear = 0xFF000000u;
        for (const DrawCmd& c : ps.draw.cmds())
            if (c.op != DrawOp::Shadow && c.fill.a > 0.9f) { clear = packPremultiplied(c.fill) | 0xFF000000u; break; }
        ref.resize(int(scr.w), int(scr.h), clear);
        CpuRasterizer rz = win.rasterizer();
        rz.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
        rz.render(ps.draw, ref);
        int maxErr = 255;
        if (pop.width == ref.width && pop.height == ref.height) {
            maxErr = 0;
            for (std::size_t i = 0; i < ref.pixels.size(); ++i)
                for (int c = 0; c < 3; ++c)
                    maxErr = std::max(maxErr, std::abs(int((pop.pixels[i] >> (8 * c)) & 0xFF) - int((ref.pixels[i] >> (8 * c)) & 0xFF)));
        }
        std::printf("  scale 2 popup read-back %dx%d, max channel error %d\n", pop.width, pop.height, maxErr);
        CHECK(maxErr <= 2);
        if (!out.empty()) app::writePng(pop, out + "/native_services_popup2.png");
        NodeHandle item;
        for (NodeHandle it = ui.firstChild(ui.firstChild(scope)); ui.alive(it); it = ui.nextSibling(it))
            if (ui.text(it) == "Three") item = it;
        CHECK(ui.alive(item));
        const Vec2 ic = centre(ui.rect(item));
        CHECK(native.postClick(native.popupWindow(win, 0), {(ic.x - menu.x) * 2.f, (ic.y - menu.y) * 2.f}));
        CHECK(pumpUntil(a, [&] { return picked >= 0.f; }));
        CHECK(picked == 2.f);
    }
}

void clipboard() {
    app::Application a{{.name = "gate_services_clip_a", .loadSettings = false}};
    app::Application b{{.name = "gate_services_clip_b", .loadSettings = false}};
    app::NativePlatform na(a, {.vulkan = false}), nb(b, {.vulkan = false}); // two display connections
    app::AppWindow& wa = a.createWindow({.title = "clip a", .width = 120, .height = 80});
    app::AppWindow& wb = b.createWindow({.title = "clip b", .width = 120, .height = 80});
    na.show(wa);
    nb.show(wb);
    const std::string text = "FUSE clipboard \xC3\xA9\xE2\x9C\x93 line\nsecond";
    // the window's clipboard hooks are the system clipboard
    CHECK(wa.ui().setClipboardText(text));
    std::string got;
    CHECK(na.clipboardText(got) && got == text); // the owner reads its own copy
    // the other connection converts the selection while the owner pumps its events
    auto serve = [](app::Application& owner, std::atomic<bool>& stop) {
        while (!stop.load()) owner.step(0.005f);
    };
    {
        std::atomic<bool> stop{false};
        std::thread t(serve, std::ref(a), std::ref(stop));
        got.clear();
        const bool ok = wb.ui().clipboardText(got);
        stop = true;
        t.join();
        CHECK(ok);
        CHECK(got == text);
    }
    // ownership moves: b copies, a is told (SelectionClear) and now asks b
    CHECK(nb.setClipboardText("from b"));
    for (int i = 0; i < 20; ++i) a.step(0.005f);
    {
        std::atomic<bool> stop{false};
        std::thread t(serve, std::ref(b), std::ref(stop));
        got.clear();
        const bool ok = na.clipboardText(got);
        stop = true;
        t.join();
        CHECK(ok);
        CHECK(got == "from b");
    }
    // an empty string is a valid clipboard
    CHECK(nb.setClipboardText(""));
    CHECK(nb.clipboardText(got) && got.empty());
}

void windowManagement() {
    using T = app::WindowEvent::Type;
    app::Application a{{.name = "gate_services_wm", .loadSettings = false}};
    app::NativePlatform native(a, {.vulkan = false, .scale = 1.f});
    app::AppWindow& win = a.createWindow({.title = "wm", .width = 300, .height = 200});
    Events ev;
    win.setEventHandler({&ev, &onEvent});
    win.setMinimumSize({200.f, 120.f}); // before the window exists: applied by show()
    native.show(win);
    pumpUntil(a, [&] { return win.framesPresented() >= 1; });
    CHECK(win.visible());

    win.setTitle("retitled"); // forwarded (nothing to read back without a window manager)
    // position: client origin on screen
    win.setPosition({70.f, 50.f});
    CHECK(pumpUntil(a, [&] { return win.position().x == 70.f && win.position().y == 50.f; }));
    CHECK(ev.saw(T::Moved));
    const Vec2 o = native.screenOrigin(win);
    CHECK(o.x == 70.f && o.y == 50.f);
    // focus
    win.activate();
    CHECK(pumpUntil(a, [&] { return win.active(); }));
    CHECK(ev.saw(T::FocusGained));
    // hide / show
    win.setVisible(false);
    CHECK(pumpUntil(a, [&] { return !win.visible(); }));
    CHECK(ev.saw(T::Hidden));
    win.setVisible(true);
    CHECK(pumpUntil(a, [&] { return win.visible(); }));
    CHECK(ev.saw(T::Shown));
    // size limits: growing the minimum past the current size resizes the OS window
    win.setMinimumSize({400.f, 260.f});
    CHECK(pumpUntil(a, [&] { return win.size().w >= 400.f && win.size().h >= 260.f; }));
    win.setMaximumSize({420.f, 300.f});
    CHECK(native.resizeWindow(win, 410, 280));
    CHECK(pumpUntil(a, [&] { return win.size().w == 410.f && win.size().h == 280.f; }));
    // maximise / fullscreen / minimise are window-manager requests: without one the state honestly stays Normal
    win.maximize();
    for (int i = 0; i < 20; ++i) a.step(0.005f);
    std::printf("  maximize -> mode %d (0 = Normal: no window manager, 2 = Maximized)\n", int(win.mode()));
    CHECK(win.mode() == app::WindowMode::Normal || win.mode() == app::WindowMode::Maximized);
    win.restore();
    for (int i = 0; i < 20; ++i) a.step(0.005f);
    CHECK(win.mode() == app::WindowMode::Normal);

    // displays
    const std::vector<app::DisplayInfo> ds = native.displays();
    CHECK(!ds.empty());
    for (const app::DisplayInfo& d : ds) {
        std::printf("  display \"%s\" %.0f,%.0f %.0fx%.0f work %.0fx%.0f scale %.2f %.0f Hz%s\n", d.name.c_str(), d.bounds.x, d.bounds.y,
                    d.bounds.w, d.bounds.h, d.workArea.w, d.workArea.h, double(d.scale), double(d.refreshHz), d.primary ? " primary" : "");
        CHECK(d.bounds.w > 0.f && d.bounds.h > 0.f && d.workArea.w > 0.f && d.scale > 0.f);
    }
    const app::SystemTheme theme = native.systemTheme(); // honest Unknown on a bare X server
    std::printf("  system theme: %d (0 unknown, 1 light, 2 dark)\n", int(theme.scheme));
    CHECK(theme == a.systemTheme());

    // the closing window detaches cleanly (clipboard hooks, backend)
    win.requestClose();
    a.step(0.f);
    CHECK(a.windowCount() == 0u);
}

#if !defined(_WIN32)
struct DialogResult {
    int calls = 0;
    std::string path;
};
void writeScript(const std::string& path, const std::string& body) {
    {
        std::ofstream f(path, std::ios::trunc);
        f << "#!/bin/sh\n" << body << "\n";
    }
    ::chmod(path.c_str(), 0755);
}
void helpers(const std::string& out) {
    if (out.empty()) return;
    const std::string bin = out + "/native_services_bin";
    ::mkdir(bin.c_str(), 0755);
    for (const char* stale : {"/zenity", "/kdialog", "/xdg-open"}) ::remove((bin + stale).c_str()); // from an earlier run
    const std::string savedPath = std::getenv("PATH") ? std::getenv("PATH") : "";
    app::Application a{{.name = "gate_services_dialog", .loadSettings = false}};
    app::NativePlatform native(a, {.vulkan = false});
    app::AppWindow& win = a.createWindow({.title = "dialog", .width = 640, .height = 480});
    native.show(win);
    DialogResult res;
    const app::FileDialogCallback cb{&res, [](void* u, app::AppWindow&, std::string_view path) {
                                         ++static_cast<DialogResult*>(u)->calls;
                                         static_cast<DialogResult*>(u)->path = path;
                                     }};
    const app::FileDialogOptions opt{.title = "Open it", .directory = "/tmp", .filter = "*.png"};
    std::string path;

    // no helper in PATH: unavailable, and the helper falls back to the toolkit dialog
    ::setenv("PATH", bin.c_str(), 1);
    CHECK(native.fileDialog(&win, opt, path) == app::FileDialogStatus::Unavailable);
    native.openFileDialog(win, opt, cb);
    for (int i = 0; i < 5; ++i) a.step(0.005f);
    CHECK(res.calls == 0);
    bool toolkitOpen = false;
    for (u32 i = 0; i < 4096 && !toolkitOpen; ++i) {
        const NodeHandle n = win.ui().handleOf(i);
        toolkitOpen = win.ui().alive(n) && win.ui().hasClass(n, "filedialog") && win.ui().computed(n).visible;
    }
    CHECK(toolkitOpen);
    win.ui().keyDown(Key::Escape);
    win.ui().keyUp(Key::Escape);

    // a zenity in PATH answers: its stdout is the path, its arguments are the ones we document
    writeScript(bin + "/zenity", "printf '%s\\n' \"$@\" > \"" + bin + "/zenity.args\"\necho '/tmp/some dir/picture.png'");
    CHECK(native.fileDialog(&win, opt, path) == app::FileDialogStatus::Accepted);
    CHECK(path == "/tmp/some dir/picture.png");
    {
        std::ifstream f(bin + "/zenity.args");
        std::string line, all;
        while (std::getline(f, line)) all += line + "|";
        CHECK(all == "--file-selection|--title=Open it|--filename=/tmp/|--file-filter=Files | *.png|");
    }
    res = {};
    native.openFileDialog(win, opt, cb);
    CHECK(res.calls == 1 && res.path == "/tmp/some dir/picture.png");
    // cancelled: exit status 1, no output
    writeScript(bin + "/zenity", "exit 1");
    CHECK(native.fileDialog(&win, opt, path) == app::FileDialogStatus::Cancelled && path.empty());
    res = {};
    native.openFileDialog(win, opt, cb);
    CHECK(res.calls == 1 && res.path.empty());
    ::remove((bin + "/zenity").c_str());
    // kdialog is the second choice
    writeScript(bin + "/kdialog", "echo /home/k/file.txt");
    CHECK(native.fileDialog(&win, {.title = "Save", .save = true}, path) == app::FileDialogStatus::Accepted && path == "/home/k/file.txt");
    ::remove((bin + "/kdialog").c_str());

    // openUrl / revealFile: xdg-open with the URL / the containing folder; false when there is no xdg-open
    CHECK(!app::openUrl("https://example.org/"));
    const std::string log = bin + "/xdg.log";
    ::remove(log.c_str());
    writeScript(bin + "/xdg-open", "echo \"$1\" >> \"" + log + "\"");
    ::setenv("PATH", (bin + ":/usr/bin:/bin").c_str(), 1);
    CHECK(app::openUrl("https://example.org/a?b=c"));
    CHECK(app::revealFile("/tmp/some dir/picture.png"));
    CHECK(!app::openUrl(""));
    std::string all;
    for (int i = 0; i < 200 && all.find("/tmp/some dir") == std::string::npos; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::ifstream f(log);
        std::string line;
        all.clear();
        while (std::getline(f, line)) all += line + "|";
    }
    CHECK(all.find("https://example.org/a?b=c|") != std::string::npos);
    CHECK(all.find("/tmp/some dir|") != std::string::npos);
    ::setenv("PATH", savedPath.c_str(), 1);
}
#endif
} // namespace

int main(int argc, char** argv) {
    // this connection stays open for the whole run: a display server that resets when its last client leaves (Xvfb)
    // would otherwise restart between the sections below
    app::Application probeApp{{.name = "gate_services_probe", .argc = argc, .argv = argv, .loadSettings = false}};
    const std::string out(probeApp.args().get("out"));
    app::NativePlatform probe(probeApp, {.vulkan = false});
    if (!probe.available()) {
        std::printf("fuse_app_native_services: skipped (%.*s)\n", int(probe.lastError().size()), probe.lastError().data());
        return 77;
    }
    hidpiAndCursor(out);
    clipboard();
    windowManagement();
#if !defined(_WIN32)
    helpers(out);
#endif
    std::printf("fuse_app_native_services: %s\n", g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}
