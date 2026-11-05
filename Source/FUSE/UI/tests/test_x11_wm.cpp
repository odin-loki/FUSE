// X11 gates under a real window manager (plan §4.13, UI5 / UI10): the gate starts a lightweight WM on the Xvfb
// display (openbox, xfwm4, metacity, ... found at configure time or through $FUSE_TEST_WM) and drives the real pointer
// with XTest, so moves, resizes and state changes are carried out by the WM, not simulated. One source, one executable
// per gate (FUSE_X11_WM_GATE):
//   chrome       WindowFrame::Custom: the WM leaves the window undecorated (_MOTIF_WM_HINTS -> zero
//                _NET_FRAME_EXTENTS); a caption drag moves it and a bottom-right drag resizes it (_NET_WM_MOVERESIZE);
//                the FUSE max button maximises to the work area and restores (_NET_WM_STATE), a caption double click
//                does the same, min iconifies (WM_CHANGE_STATE -> WM_STATE Iconic) and _NET_ACTIVE_WINDOW brings it
//                back, close closes
//   popups       NativeOptions::osPopups on a WM-decorated window: the popup is not reparented (override-redirect),
//                sits at the menu's screen position below the title bar, a real click on an item outside the main
//                window picks it, a real click on the desktop closes the popup (pointer grab)
//   multiwindow  two decorated windows: both managed (_NET_CLIENT_LIST, frame extents), _NET_ACTIVE_WINDOW moves
//                keyboard focus between them, a real click reaches the window under the pointer, resizeWindow goes
//                through the WM, _NET_CLOSE_WINDOW closes only that window (WM_DELETE_WINDOW)
// Exit 77 without a display, XTest or a window manager to run.
#include "fuse/app/chrome.hpp"
#include "fuse/app/native.hpp"
#include "fuse/ui/widgets/widgets.hpp"
#include "x11_internal.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifndef FUSE_X11_WM_GATE
#define FUSE_X11_WM_GATE "chrome"
#endif

extern char** environ;

using namespace fuse::ui;
namespace app = fuse::app;
namespace x11 = fuse::app::x11;
namespace w = fuse::ui::widgets;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
const std::string kGate = std::string("fuse_x11_wm_") + FUSE_X11_WM_GATE;

int skip(std::string_view why) {
    std::printf("%s: skipped (%.*s)\n", kGate.c_str(), int(why.size()), why.data());
    return 77;
}
bool exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

/// The window manager process: $FUSE_TEST_WM, else the one found at configure time, else the first on PATH. A WM
/// unpacked into a private prefix (<prefix>/usr/bin/<wm>, e.g. from `apt-get download` + `dpkg -x`) gets that
/// prefix's libraries, data and config directories.
struct WindowManager {
    pid_t pid = 0;
    std::string path;
    ~WindowManager() { stop(); }
    static std::string find() {
        if (const char* e = std::getenv("FUSE_TEST_WM"); e && *e) return e;
#ifdef FUSE_TEST_WM_PATH
        if (exists(FUSE_TEST_WM_PATH)) return FUSE_TEST_WM_PATH;
#endif
        const char* path = std::getenv("PATH");
        for (const char* n : {"openbox", "xfwm4", "metacity", "marco", "icewm", "fluxbox", "kwin_x11", "mutter"}) {
            std::string dirs = path ? path : "";
            for (std::size_t at = 0; at <= dirs.size();) {
                const std::size_t end = std::min(dirs.find(':', at), dirs.size());
                const std::string p = dirs.substr(at, end - at) + "/" + n;
                if (end > at && ::access(p.c_str(), X_OK) == 0) return p;
                at = end + 1;
            }
        }
        return {};
    }
    bool start(std::string& why) {
        path = find();
        if (path.empty()) { why = "no window manager (openbox / xfwm4 / metacity / ...; or set FUSE_TEST_WM)"; return false; }
        std::vector<std::string> env;
        // <root>/usr/bin/<wm> outside /usr: the unpacked package's libraries, themes and rc files
        const std::string bin = path.substr(0, path.rfind('/'));
        const std::string usr = bin.substr(0, bin.rfind('/'));
        const std::string root = usr.substr(0, usr.rfind('/'));
        const bool privatePrefix = usr != "/usr" && usr != "/usr/local" && exists(usr + "/lib/x86_64-linux-gnu");
        for (char** e = environ; *e; ++e) {
            const std::string kv = *e;
            if (privatePrefix && (kv.rfind("LD_LIBRARY_PATH=", 0) == 0 || kv.rfind("XDG_DATA_DIRS=", 0) == 0 ||
                                  kv.rfind("XDG_CONFIG_DIRS=", 0) == 0))
                continue;
            env.push_back(kv);
        }
        if (privatePrefix) {
            const char* ld = std::getenv("LD_LIBRARY_PATH");
            env.push_back("LD_LIBRARY_PATH=" + usr + "/lib/x86_64-linux-gnu" + (ld && *ld ? std::string(":") + ld : ""));
            env.push_back("XDG_DATA_DIRS=" + usr + "/share:/usr/local/share:/usr/share");
            env.push_back("XDG_CONFIG_DIRS=" + root + "/etc/xdg:/etc/xdg");
        }
        std::vector<char*> envp;
        for (std::string& s : env) envp.push_back(s.data());
        envp.push_back(nullptr);
        std::vector<std::string> args{path};
        const std::string name = path.substr(path.rfind('/') + 1);
        if (name == "openbox") args.push_back("--sm-disable");
        if (name == "xfwm4") args.push_back("--compositor=off");
        if (name == "metacity" || name == "marco") args.push_back("--replace");
        std::vector<char*> argv;
        for (std::string& s : args) argv.push_back(s.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0); // keep the WM's chatter out
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        const int rc = posix_spawn(&pid, path.c_str(), &fa, nullptr, argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&fa);
        if (rc != 0) { pid = 0; why = "cannot start " + path; return false; }
        return true;
    }
    void stop() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            for (int i = 0; i < 100 && ::waitpid(pid, nullptr, WNOHANG) == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, WNOHANG);
            pid = 0;
        }
    }
};

/// The gate's own X connection: the user's pointer (XTest) and an EWMH pager's view.
struct Desk {
    x11::Lib x;
    x11::Display* d = nullptr;
    x11::Window root = 0;
    void* xtst = nullptr;
    int (*fakeMotion)(x11::Display*, int, int, int, unsigned long) = nullptr;
    int (*fakeButton)(x11::Display*, unsigned, int, unsigned long) = nullptr;
    int (*queryTree)(x11::Display*, x11::Window, x11::Window*, x11::Window*, x11::Window**, unsigned*) = nullptr;
    ~Desk() {
        if (d) x.CloseDisplay(d);
    }
    bool open(std::string& why) {
        if (!x.load() || !(d = x.OpenDisplay(nullptr))) { why = "no X display"; return false; }
        root = x.RootWindow(d, x.DefaultScreen(d));
        queryTree = reinterpret_cast<decltype(queryTree)>(dlsym(x.so, "XQueryTree"));
        xtst = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
        if (xtst) {
            fakeMotion = reinterpret_cast<decltype(fakeMotion)>(dlsym(xtst, "XTestFakeMotionEvent"));
            fakeButton = reinterpret_cast<decltype(fakeButton)>(dlsym(xtst, "XTestFakeButtonEvent"));
        }
        if (!fakeMotion || !fakeButton || !queryTree) { why = "libXtst.so.6 (XTest) not found"; return false; }
        return true;
    }
    x11::Atom atom(const char* n) { return x.InternAtom(d, n, 0); }
    std::vector<long> longs(x11::Window win, const char* prop) {
        std::vector<unsigned char> v;
        std::vector<long> out;
        if (x.property(d, win, atom(prop), 0 /*AnyPropertyType*/, v))
            for (std::size_t i = 0; i + sizeof(long) <= v.size(); i += sizeof(long)) {
                long l = 0;
                std::memcpy(&l, v.data() + i, sizeof l);
                out.push_back(l);
            }
        return out;
    }
    bool wmRunning() {
        const auto v = longs(root, "_NET_SUPPORTING_WM_CHECK");
        return !v.empty() && v[0] != 0;
    }
    x11::Window parent(x11::Window win) {
        x11::Window r = 0, p = 0, *kids = nullptr;
        unsigned n = 0;
        if (!queryTree(d, win, &r, &p, &kids, &n)) return 0;
        if (kids) x.Free(kids);
        return p;
    }
    void move(int px, int py) {
        fakeMotion(d, -1, px, py, 0);
        x.Sync(d, 0);
    }
    void button(bool down) {
        fakeButton(d, 1, down ? 1 : 0, 0);
        x.Sync(d, 0);
    }
    /// EWMH request from a pager / taskbar (source 2) about `win`
    void ewmh(x11::Window win, const char* type, long l0 = 0, long l1 = 0, long l2 = 0) {
        x.clientMessage(d, root, win, atom(type), l0, l1, l2);
        x.Sync(d, 0);
    }
};

void pump(app::Application& a, int n = 10) {
    for (int i = 0; i < n; ++i) a.step(0.005f);
}
bool waitFor(app::Application& a, const std::function<bool()>& pred, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred() && std::chrono::steady_clock::now() < end) a.step(0.01f);
    return pred();
}
Vec2 center(const UiContext& ui, NodeHandle n) {
    const Rect r = ui.rect(n);
    return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
}
/// A real click (XTest) at screen point p, pumping the app between press and release.
void click(Desk& desk, app::Application& a, Vec2 p) {
    desk.move(int(p.x), int(p.y));
    pump(a, 4);
    desk.button(true);
    pump(a, 4);
    desk.button(false);
    pump(a, 6);
}
/// A real drag (XTest): press at `from`, move in steps by `by`, release.
void drag(Desk& desk, app::Application& a, Vec2 from, Vec2 by) {
    desk.move(int(from.x), int(from.y));
    pump(a, 4);
    desk.button(true);
    pump(a, 10); // the app sees the press and hands the drag to the WM
    for (int i = 1; i <= 10; ++i) {
        desk.move(int(from.x + by.x * f32(i) / 10.f), int(from.y + by.y * f32(i) / 10.f));
        pump(a, 2);
    }
    pump(a, 4);
    desk.button(false);
    pump(a, 10);
}
bool near(f32 a, f32 b, f32 tol = 1.f) { return std::abs(a - b) <= tol; }

/// Starts the WM and waits until it manages the screen. Returns 0, or the skip code.
int startWm(Desk& desk, WindowManager& wm) {
    std::string why;
    if (!desk.open(why)) return skip(why);
    if (desk.wmRunning()) return skip("another window manager already runs on this display");
    if (!wm.start(why)) return skip(why);
    for (int i = 0; i < 500 && !desk.wmRunning(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!desk.wmRunning()) return skip(wm.path + " did not start (no _NET_SUPPORTING_WM_CHECK within 10 s)");
    // the WM sets _NET_SUPPORTING_WM_CHECK before it finishes its start-up scan; a window mapped in that gap can be
    // left unmanaged on a loaded machine, so give it a moment to settle
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::printf("  window manager: %s\n", wm.path.c_str());
    return 0;
}

// ---- chrome ----------------------------------------------------------------------------------------------------------
int gateChrome(int argc, char** argv) {
    Desk desk;
    WindowManager wm;
    if (const int r = startWm(desk, wm)) return r;
    constexpr u32 kW = 480, kH = 300;
    app::Application a{{.name = "gate_wm_chrome", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform np(a, {.vulkan = false, .dpiAware = false});
    if (!np.available()) return skip(np.lastError());
    auto& win = a.createWindow({.title = "fuse_x11_wm chrome", .width = kW, .height = kH, .frame = app::WindowFrame::Custom});
    UiContext& ui = win.ui();
    ui.setCss(ui.root(), "padding", "0px");
    ui.setCss(ui.root(), "gap", "0px");
    app::titleBar(win, ui.root(), "Custom chrome");
    const NodeHandle body = w::panel(ui, ui.root(), "body");
    ui.setCss(body, "flex-grow", "1");
    np.show(win);
    waitFor(a, [&] { return win.framesPresented() >= 1; });
    const x11::Window xw = x11::Window(np.nativeWindow(win));
    // managed (reparented into the WM's frame) and undecorated
    CHECK(waitFor(a, [&] { return desk.parent(xw) != desk.root && desk.longs(xw, "_NET_FRAME_EXTENTS").size() == 4; }, 10000));
    const auto ext = desk.longs(xw, "_NET_FRAME_EXTENTS");
    std::printf("  _NET_FRAME_EXTENTS %ld %ld %ld %ld\n", ext.size() == 4 ? ext[0] : -1, ext.size() == 4 ? ext[1] : -1,
                ext.size() == 4 ? ext[2] : -1, ext.size() == 4 ? ext[3] : -1);
    CHECK(ext.size() == 4 && ext[0] == 0 && ext[1] == 0 && ext[2] == 0 && ext[3] == 0);
    pump(a, 20);

    // move: drag the caption
    const Vec2 o0 = np.screenOrigin(win);
    drag(desk, a, {o0.x + 200.f, o0.y + 16.f}, {120.f, 70.f});
    CHECK(waitFor(a, [&] { const Vec2 o = np.screenOrigin(win); return near(o.x, o0.x + 120.f) && near(o.y, o0.y + 70.f); }, 2000));
    const Vec2 o1 = np.screenOrigin(win);
    std::printf("  caption drag: origin %.0f,%.0f -> %.0f,%.0f\n", o0.x, o0.y, o1.x, o1.y);
    // resize: drag the bottom-right corner
    drag(desk, a, {o1.x + f32(kW) - 2.f, o1.y + f32(kH) - 2.f}, {60.f, 40.f});
    CHECK(waitFor(a, [&] { return near(f32(win.pixelSize().w), f32(kW + 60), 2.f) && near(f32(win.pixelSize().h), f32(kH + 40), 2.f); }, 2000));
    const auto sized = win.pixelSize();
    std::printf("  corner drag: %ux%u -> %ux%u (origin %.0f,%.0f)\n", kW, kH, sized.w, sized.h, np.screenOrigin(win).x, np.screenOrigin(win).y);
    CHECK(near(np.screenOrigin(win).x, o1.x) && near(np.screenOrigin(win).y, o1.y)); // the top-left stays
    pump(a, 20);

    // maximise / restore through the FUSE button: the WM applies _NET_WM_STATE
    const int sw = desk.x.DisplayWidth(desk.d, desk.x.DefaultScreen(desk.d)), sh = desk.x.DisplayHeight(desk.d, desk.x.DefaultScreen(desk.d));
    auto screenOf = [&](NodeHandle n) { const Vec2 o = np.screenOrigin(win), c = center(ui, n); return Vec2{o.x + c.x, o.y + c.y}; };
    click(desk, a, screenOf(ui.find("titlebar.max")));
    CHECK(waitFor(a, [&] { return win.chrome().maximized && win.pixelSize().w == u32(sw) && win.pixelSize().h == u32(sh); }));
    std::printf("  maximised: chrome %d, %ux%u (screen %dx%d)\n", win.chrome().maximized, win.pixelSize().w, win.pixelSize().h, sw, sh);
    pump(a, 20);
    click(desk, a, screenOf(ui.find("titlebar.max")));
    CHECK(waitFor(a, [&] { return !win.chrome().maximized && win.pixelSize().w == sized.w && win.pixelSize().h == sized.h; }));
    std::printf("  restored: %ux%u\n", win.pixelSize().w, win.pixelSize().h);
    pump(a, 20);
    // a double click on the caption toggles maximise too
    {
        const Vec2 cap = {np.screenOrigin(win).x + 200.f, np.screenOrigin(win).y + 16.f};
        click(desk, a, cap);
        click(desk, a, cap);
        CHECK(waitFor(a, [&] { return win.chrome().maximized; }));
        pump(a, 20);
        const Vec2 cap2 = {np.screenOrigin(win).x + 200.f, np.screenOrigin(win).y + 16.f};
        click(desk, a, cap2);
        click(desk, a, cap2);
        CHECK(waitFor(a, [&] { return !win.chrome().maximized && win.pixelSize().w == sized.w; }));
        std::printf("  caption double click: maximise + restore ok\n");
        pump(a, 20);
    }
    // minimise: WM_CHANGE_STATE -> the WM iconifies (WM_STATE Iconic); _NET_ACTIVE_WINDOW restores
    click(desk, a, screenOf(ui.find("titlebar.min")));
    auto wmState = [&] { const auto v = desk.longs(xw, "WM_STATE"); return v.empty() ? -1L : v[0]; };
    CHECK(waitFor(a, [&] { return wmState() == 3; }));
    std::printf("  minimised: WM_STATE %ld\n", wmState());
    desk.ewmh(xw, "_NET_ACTIVE_WINDOW", 2);
    CHECK(waitFor(a, [&] { return wmState() == 1; }));
    std::printf("  activated: WM_STATE %ld\n", wmState());
    pump(a, 20);
    // close
    click(desk, a, screenOf(ui.find("titlebar.close")));
    CHECK(waitFor(a, [&] { return a.windowCount() == 0; }));
    return 0;
}

// ---- popups ----------------------------------------------------------------------------------------------------------
int gatePopups(int argc, char** argv) {
    Desk desk;
    WindowManager wm;
    if (const int r = startWm(desk, wm)) return r;
    app::Application a{{.name = "gate_wm_popups", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform np(a, {.vulkan = false, .osPopups = true, .dpiAware = false});
    if (!np.available()) return skip(np.lastError());
    auto& win = a.createWindow({.title = "fuse_x11_wm popups", .width = 320, .height = 160});
    UiContext& ui = win.ui();
    const NodeHandle btn = w::button(ui, ui.root(), "File", "file");
    np.show(win);
    waitFor(a, [&] { return win.framesPresented() >= 1; });
    const x11::Window xw = x11::Window(np.nativeWindow(win));
    CHECK(waitFor(a, [&] { const auto e = desk.longs(xw, "_NET_FRAME_EXTENTS"); return e.size() == 4 && e[2] > 0; }));
    const auto ext = desk.longs(xw, "_NET_FRAME_EXTENTS");
    pump(a, 20);
    f32 picked = -1.f;
    ui.connect(btn, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& sa) { picked = sa.value; });
    std::string items;
    for (int i = 1; i <= 12; ++i) items += (i > 1 ? "\n" : "") + std::string("Item ") + std::to_string(i);
    NodeHandle scope;
    auto open = [&] {
        scope = w::popupMenu(ui, btn, items, {40.f, 60.f});
        win.requestRedraw();
        return waitFor(a, [&] { return np.popupCount(win) == 1; }, 2000);
    };

    CHECK(open());
    pump(a, 20);
    const x11::Window pw = x11::Window(np.popupWindow(win, 0));
    const Vec2 origin = np.screenOrigin(win);
    const Rect scr = np.popupScreenRect(win, 0);
    const Rect menu = ui.rect(ui.firstChild(scope));
    int px = 0, py = 0;
    x11::Window child = 0;
    desk.x.TranslateCoordinates(desk.d, pw, desk.root, 0, 0, &px, &py, &child);
    std::printf("  decorated window: frame top %ld, client origin %.0f,%.0f; popup %.0f,%.0f %.0fx%.0f (server says %d,%d)\n",
                ext.size() == 4 ? ext[2] : -1L, origin.x, origin.y, scr.x, scr.y, scr.w, scr.h, px, py);
    CHECK(desk.parent(pw) == desk.root);                     // override-redirect: not reparented
    CHECK(desk.longs(pw, "_NET_FRAME_EXTENTS").empty());      // nor decorated
    CHECK(near(scr.x, origin.x + menu.x) && near(scr.y, origin.y + menu.y)); // below the WM's title bar
    CHECK(px == int(scr.x) && py == int(scr.y));
    CHECK(origin.y >= f32(ext.size() == 4 ? ext[2] : 0));

    // a real click on "Item 11" (outside the main window)
    NodeHandle item;
    const NodeHandle panel = ui.popupSurfaces()[0].node;
    for (NodeHandle it = ui.firstChild(panel); ui.alive(it); it = ui.nextSibling(it))
        if (ui.text(it) == "Item 11") item = it;
    CHECK(ui.alive(item));
    if (ui.alive(item)) {
        const Rect ir = ui.rect(item), pr = ui.rect(panel);
        click(desk, a, {scr.x + ir.x + ir.w * 0.5f - pr.x, scr.y + ir.y + ir.h * 0.5f - pr.y});
        CHECK(waitFor(a, [&] { return picked == 10.f && np.popupCount(win) == 0; }, 2000));
        std::printf("  real click on Item 11: picked %.0f, popups %zu\n", picked, np.popupCount(win));
    }
    // a real click on the desktop closes the popup without picking
    picked = -1.f;
    CHECK(open());
    pump(a, 20);
    click(desk, a, {1200.f, 700.f});
    CHECK(waitFor(a, [&] { return np.popupCount(win) == 0; }, 2000));
    CHECK(picked < 0.f);
    std::printf("  desktop click: popups %zu, picked %.0f\n", np.popupCount(win), picked);
    win.requestClose();
    a.step(0.f);
    return 0;
}

// ---- multiwindow -----------------------------------------------------------------------------------------------------
int gateMultiwindow(int argc, char** argv) {
    Desk desk;
    WindowManager wm;
    if (const int r = startWm(desk, wm)) return r;
    app::Application a{{.name = "gate_wm_multiwindow", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform np(a, {.vulkan = false, .dpiAware = false});
    if (!np.available()) return skip(np.lastError());
    auto& wa = a.createWindow({.title = "fuse_x11_wm A", .width = 300, .height = 200});
    auto& wb = a.createWindow({.title = "fuse_x11_wm B", .width = 300, .height = 200});
    int clicksA = 0, clicksB = 0;
    const NodeHandle ba = w::button(wa.ui(), wa.ui().root(), "A", "a");
    const NodeHandle bb = w::button(wb.ui(), wb.ui().root(), "B", "b");
    wa.ui().connect(ba, SignalKind::Clicked, [&](UiContext&, NodeHandle, const SignalArgs&) { ++clicksA; });
    wb.ui().connect(bb, SignalKind::Clicked, [&](UiContext&, NodeHandle, const SignalArgs&) { ++clicksB; });
    np.show(wa);
    np.show(wb);
    waitFor(a, [&] { return wa.framesPresented() >= 1 && wb.framesPresented() >= 1; });
    const x11::Window xa = x11::Window(np.nativeWindow(wa)), xb = x11::Window(np.nativeWindow(wb));
    auto managed = [&](x11::Window xw) {
        bool listed = false;
        for (long c : desk.longs(desk.root, "_NET_CLIENT_LIST")) listed |= x11::Window(c) == xw;
        const auto e = desk.longs(xw, "_NET_FRAME_EXTENTS");
        return listed && e.size() == 4 && e[2] > 0 && desk.parent(xw) != desk.root;
    };
    CHECK(waitFor(a, [&] { return managed(xa) && managed(xb); }));
    pump(a, 20);
    const Vec2 oa = np.screenOrigin(wa), ob = np.screenOrigin(wb);
    std::printf("  A at %.0f,%.0f, B at %.0f,%.0f (placed by the WM)\n", oa.x, oa.y, ob.x, ob.y);

    // keyboard focus follows _NET_ACTIVE_WINDOW
    auto activate = [&](x11::Window xw) { desk.ewmh(xw, "_NET_ACTIVE_WINDOW", 2); };
    activate(xa);
    CHECK(waitFor(a, [&] { return np.windowFocused(wa) && !np.windowFocused(wb); }));
    activate(xb);
    CHECK(waitFor(a, [&] { return np.windowFocused(wb) && !np.windowFocused(wa); }));
    auto active = [&] { const auto v = desk.longs(desk.root, "_NET_ACTIVE_WINDOW"); return v.empty() ? x11::Window(0) : x11::Window(v[0]); };
    CHECK(waitFor(a, [&] { return active() == xb; }, 1000));
    const auto act = desk.longs(desk.root, "_NET_ACTIVE_WINDOW");
    std::printf("  focus: A -> B ok (_NET_ACTIVE_WINDOW 0x%lx)\n", act.empty() ? 0ul : (unsigned long)act[0]);
    // a real click lands in the window under the pointer (raise A first: B may overlap it)
    activate(xa);
    waitFor(a, [&] { return np.windowFocused(wa); });
    pump(a, 20);
    const Vec2 ca = center(wa.ui(), ba);
    click(desk, a, {np.screenOrigin(wa).x + ca.x, np.screenOrigin(wa).y + ca.y});
    CHECK(waitFor(a, [&] { return clicksA == 1; }, 2000));
    CHECK(clicksB == 0);
    std::printf("  real click: A %d, B %d\n", clicksA, clicksB);
    // resizeWindow goes through the WM (SubstructureRedirect) and comes back as ConfigureNotify
    CHECK(np.resizeWindow(wa, 400, 260));
    CHECK(waitFor(a, [&] { return wa.pixelSize().w == 400u && wa.pixelSize().h == 260u; }));
    // the WM's close (_NET_CLOSE_WINDOW -> WM_DELETE_WINDOW) closes B only
    desk.ewmh(xb, "_NET_CLOSE_WINDOW");
    CHECK(waitFor(a, [&] { return a.windowCount() == 1; }));
    CHECK(a.windowCount() == 1 && np.nativeWindow(wa) == xa);
    std::printf("  _NET_CLOSE_WINDOW B: %u window(s) left\n", a.windowCount());
    wa.requestClose();
    a.step(0.f);
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    const std::string g = FUSE_X11_WM_GATE;
    int r = 0;
    if (g == "chrome") r = gateChrome(argc, argv);
    else if (g == "popups") r = gatePopups(argc, argv);
    else if (g == "multiwindow") r = gateMultiwindow(argc, argv);
    if (g_fail) {
        std::printf("%s: FAILED (%d checks)\n", kGate.c_str(), g_fail);
        return 1;
    }
    if (r == 77) return 77;
    std::printf("%s: OK\n", kGate.c_str());
    return r;
}
