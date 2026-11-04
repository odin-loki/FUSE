// Custom window chrome gate (plan §4.13 Windows): WindowFrame::Custom + the FUSE UI title bar.
//   * chromeHitTest (headless): caption, the four edges and corners, min / max / close buttons, client area, an
//     interactive widget inside the caption (client), System frames (always client); the close button closes
//   * on a real Win32 window: client rect == window rect (WM_NCCALCSIZE removed the OS frame), WM_NCHITTEST at screen
//     points answers HTCAPTION / HTLEFT / HTBOTTOMRIGHT / HTMINBUTTON / HTMAXBUTTON / HTCLOSE / HTCLIENT ...,
//     WM_NCMOUSEMOVE hovers the FUSE button, non-client clicks on the buttons maximise / restore / minimise / close
//   * on a real X11 window: _MOTIF_WM_HINTS removes the decorations, presses on the caption / edges / corners send
//     _NET_WM_MOVERESIZE with the chromeHitTest direction (the gate watches the root like a window manager), client
//     presses do not, edges show resize cursors, the FUSE buttons send _NET_WM_STATE (the gate applies it as the WM
//     would: maximised state + resize) and WM_CHANGE_STATE, close closes
// Exit 77 without a display. --out=<dir> writes window_chrome.png.
#include "fuse/app/chrome.hpp"
#include "fuse/app/native.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include "x11_internal.hpp"

#include <cstring>
#endif

using namespace fuse::ui;
namespace app = fuse::app;
namespace w = fuse::ui::widgets;
using app::HitArea;
#if !defined(_WIN32)
namespace x11 = fuse::app::x11;
#endif

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
constexpr u32 kW = 480, kH = 300;
struct Probe { const char* what; Vec2 p; HitArea want; };

void build(app::AppWindow& win, NodeHandle& inCaption) {
    UiContext& ui = win.ui();
    ui.setCss(ui.root(), "padding", "0px");
    ui.setCss(ui.root(), "gap", "0px");
    const NodeHandle bar = app::titleBar(win, ui.root(), "Custom chrome");
    inCaption = w::toolButton(ui, bar, "?", "help"); // interactive widget inside the caption: stays client
    ui.moveTo(inCaption, bar, ui.find("titlebar.min"));
    const NodeHandle body = w::panel(ui, ui.root(), "body");
    ui.setCss(body, "flex-grow", "1");
    w::button(ui, body, "Body button", "bodybtn");
}
Vec2 center(const UiContext& ui, NodeHandle n) {
    const Rect r = ui.rect(n);
    return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
}
std::vector<Probe> probes(const UiContext& ui, NodeHandle inCaption) {
    return {{"caption", {200.f, 16.f}, HitArea::Caption},
            {"title label", {30.f, 16.f}, HitArea::Caption},
            {"left edge", {2.f, 150.f}, HitArea::Left},
            {"right edge", {f32(kW) - 2.f, 150.f}, HitArea::Right},
            {"top edge", {200.f, 1.f}, HitArea::Top},
            {"bottom edge", {200.f, f32(kH) - 2.f}, HitArea::Bottom},
            {"top-left", {1.f, 1.f}, HitArea::TopLeft},
            {"top-right", {f32(kW) - 1.f, 3.f}, HitArea::TopRight},
            {"bottom-left", {3.f, f32(kH) - 1.f}, HitArea::BottomLeft},
            {"bottom-right", {f32(kW) - 2.f, f32(kH) - 2.f}, HitArea::BottomRight},
            {"minimize", center(ui, ui.find("titlebar.min")), HitArea::Minimize},
            {"maximize", center(ui, ui.find("titlebar.max")), HitArea::Maximize},
            {"close", {center(ui, ui.find("titlebar.close")).x, 20.f}, HitArea::Close},
            {"button in caption", center(ui, inCaption), HitArea::Client},
            {"body", {240.f, 200.f}, HitArea::Client},
            {"body button", center(ui, ui.find("bodybtn")), HitArea::Client}};
}

int headless() {
    app::Application a{{.name = "gate_chrome", .loadSettings = false}};
    auto& win = a.createWindow({.title = "chrome", .width = kW, .height = kH, .frame = app::WindowFrame::Custom});
    NodeHandle inCaption;
    build(win, inCaption);
    a.step(0.f);
    for (const Probe& p : probes(win.ui(), inCaption)) {
        const HitArea got = app::chromeHitTest(win, p.p);
        if (got != p.want)
            std::fprintf(stderr, "  headless %s at %.0f,%.0f: %.*s, want %.*s\n", p.what, p.p.x, p.p.y, int(app::hitAreaName(got).size()),
                         app::hitAreaName(got).data(), int(app::hitAreaName(p.want).size()), app::hitAreaName(p.want).data());
        CHECK(got == p.want);
    }
    // maximised: no resize edges
    win.chrome().maximized = true;
    CHECK(app::chromeHitTest(win, {2.f, 150.f}) == HitArea::Client);
    CHECK(app::chromeHitTest(win, {200.f, 1.f}) == HitArea::Caption);
    win.chrome().maximized = false;
    // system frame: always client
    auto& sys = a.createWindow({.title = "system", .width = 200, .height = 100});
    CHECK(app::chromeHitTest(sys, {1.f, 1.f}) == HitArea::Client);
    // borderless: caption drags, no edges
    auto& bl = a.createWindow({.title = "borderless", .width = kW, .height = kH, .frame = app::WindowFrame::Borderless});
    NodeHandle c2;
    build(bl, c2);
    a.step(0.f);
    CHECK(app::chromeHitTest(bl, {2.f, 150.f}) == HitArea::Client);
    CHECK(app::chromeHitTest(bl, {1.f, 1.f}) == HitArea::Caption);
    // without a backend hook the FUSE close button closes the window
    win.click(center(win.ui(), win.ui().find("titlebar.close")));
    CHECK(win.closing());
    std::printf("  headless hit tests ok\n");
    return 0;
}

#if defined(_WIN32)
LRESULT expectedHt(HitArea a) {
    switch (a) {
    case HitArea::Caption: return HTCAPTION;
    case HitArea::Left: return HTLEFT;
    case HitArea::Right: return HTRIGHT;
    case HitArea::Top: return HTTOP;
    case HitArea::Bottom: return HTBOTTOM;
    case HitArea::TopLeft: return HTTOPLEFT;
    case HitArea::TopRight: return HTTOPRIGHT;
    case HitArea::BottomLeft: return HTBOTTOMLEFT;
    case HitArea::BottomRight: return HTBOTTOMRIGHT;
    case HitArea::Minimize: return HTMINBUTTON;
    case HitArea::Maximize: return HTMAXBUTTON;
    case HitArea::Close: return HTCLOSE;
    default: return HTCLIENT;
    }
}
LPARAM screenLp(HWND hwnd, Vec2 p) {
    POINT pt{LONG(p.x), LONG(p.y)};
    ClientToScreen(hwnd, &pt);
    return MAKELPARAM(pt.x, pt.y);
}
void ncClick(HWND hwnd, LRESULT ht, Vec2 p) {
    SendMessageW(hwnd, WM_NCLBUTTONDOWN, WPARAM(ht), screenLp(hwnd, p));
    SendMessageW(hwnd, WM_NCLBUTTONUP, WPARAM(ht), screenLp(hwnd, p));
}
#endif

#if !defined(_WIN32)
// The gate's own X connection watches the root for the EWMH requests a window manager would receive and answers the
// maximise request like one (_NET_WM_STATE property + resize).
struct WmWatch {
    x11::Lib x;
    x11::Display* d = nullptr;
    x11::Window root = 0;
    ~WmWatch() {
        if (d) x.CloseDisplay(d);
    }
    bool open() {
        if (!x.load() || !(d = x.OpenDisplay(nullptr))) return false;
        root = x.RootWindow(d, x.DefaultScreen(d));
        x.SelectInput(d, root, x11::kSubstructureNotifyMask); // ClientMessages sent to the root with that mask
        x.Sync(d, 0);
        return true;
    }
    x11::Atom atom(const char* n) { return x.InternAtom(d, n, 0); }
    void drop() {
        x.Sync(d, 0);
        while (x.Pending(d)) {
            x11::Event ev{};
            x.NextEvent(d, &ev);
        }
    }
    /// The next ClientMessage of `type` about `win` (pumping the app meanwhile); false after a timeout.
    bool next(app::Application& a, x11::Window win, x11::Atom type, x11::ClientEv& out, int ms = 1500) {
        for (int t = 0; t < ms / 5; ++t) {
            a.step(0.005f);
            x.Sync(d, 0);
            while (x.Pending(d)) {
                x11::Event ev{};
                x.NextEvent(d, &ev);
                if (ev.type == x11::ClientMessage && ev.client.window == win && ev.client.message_type == type) {
                    out = ev.client;
                    return true;
                }
            }
        }
        return false;
    }
};
int dirOf(HitArea a) {
    switch (a) {
    case HitArea::TopLeft: return 0;
    case HitArea::Top: return 1;
    case HitArea::TopRight: return 2;
    case HitArea::Right: return 3;
    case HitArea::BottomRight: return 4;
    case HitArea::Bottom: return 5;
    case HitArea::BottomLeft: return 6;
    case HitArea::Left: return 7;
    case HitArea::Caption: return 8;
    default: return -1;
    }
}
#endif

int native(const std::string& out) {
#if !defined(_WIN32)
    WmWatch wm;
    if (!wm.open()) {
        std::printf("fuse_app_window_chrome: native part skipped (no X display)\n");
        return 77;
    }
    app::Application a{{.name = "gate_chrome_native", .loadSettings = false}};
    app::NativePlatform np(a, {.dpiAware = false});
    if (!np.available()) return 77;
    auto& win = a.createWindow({.title = "fuse_app custom chrome", .width = kW, .height = kH, .frame = app::WindowFrame::Custom});
    NodeHandle inCaption;
    build(win, inCaption);
    np.show(win);
    for (int i = 0; i < 400 && win.framesPresented() < 1; ++i) a.step(0.01f);
    for (int i = 0; i < 10; ++i) a.step(0.005f);
    const x11::Window xw = x11::Window(np.nativeWindow(win));
    CHECK(xw != 0);
    if (!xw) return 1;
    // the WM is asked not to decorate the window: _MOTIF_WM_HINTS { flags & decorations, decorations = 0 }
    std::vector<unsigned char> mh;
    const x11::Atom motif = wm.atom("_MOTIF_WM_HINTS");
    const bool hasHints = wm.x.property(wm.d, xw, motif, motif, mh) && mh.size() >= 3 * sizeof(long);
    long hints[3] = {0, 0, -1};
    if (hasHints) std::memcpy(hints, mh.data(), sizeof hints);
    std::printf("  _MOTIF_WM_HINTS: %s (flags %ld, decorations %ld), window %ux%u\n", hasHints ? "set" : "missing", hints[0], hints[2],
                win.pixelSize().w, win.pixelSize().h);
    CHECK(hasHints && (hints[0] & 2) && hints[2] == 0);
    CHECK(win.pixelSize().w == kW && win.pixelSize().h == kH);

    // presses on the caption / edges become _NET_WM_MOVERESIZE requests (direction from chromeHitTest), at the press'
    // screen position; presses on client area do not
    const x11::Atom moveResize = wm.atom("_NET_WM_MOVERESIZE");
    const Vec2 origin = np.screenOrigin(win);
    int ok = 0, n = 0;
    for (const Probe& p : probes(win.ui(), inCaption)) {
        if (p.want == HitArea::Minimize || p.want == HitArea::Maximize || p.want == HitArea::Close) continue;
        wm.drop();
        ++n;
        CHECK(np.postClick(xw, p.p));
        x11::ClientEv m{};
        const bool got = wm.next(a, xw, moveResize, m, dirOf(p.want) < 0 ? 150 : 1500);
        const int want = dirOf(p.want);
        bool good = want < 0 ? !got
                             : got && m.l[2] == want && m.l[0] == long(origin.x + p.p.x) && m.l[1] == long(origin.y + p.p.y) && m.l[3] == 1;
        if (good) ++ok;
        else std::fprintf(stderr, "  %s: %s direction %ld, want %d\n", p.what, got ? "moveresize" : "no moveresize", got ? m.l[2] : -1L, want);
    }
    std::printf("  _NET_WM_MOVERESIZE: %d / %d probes match\n", ok, n);
    CHECK(ok == n);
    // resize cursor over an edge (no WM frame draws one)
    np.postPointerMove(xw, {2.f, 150.f});
    for (int i = 0; i < 20; ++i) a.step(0.005f);
    CHECK(np.appliedCursor(win) == CursorShape::ResizeEW);
    np.postPointerMove(xw, {240.f, 200.f});
    for (int i = 0; i < 20; ++i) a.step(0.005f);
    CHECK(np.appliedCursor(win) == CursorShape::Arrow);
    if (!out.empty()) {
        for (int i = 0; i < 5; ++i) { win.requestRedraw(); a.step(0.005f); }
        Image img;
        if (np.capture(win, img)) app::writePng(img, out + "/window_chrome.png");
    }

    // maximise / restore through the FUSE buttons: _NET_WM_STATE requests; the "WM" applies them
    UiContext& ui = win.ui();
    const x11::Atom state = wm.atom("_NET_WM_STATE"), maxV = wm.atom("_NET_WM_STATE_MAXIMIZED_VERT"),
                    maxH = wm.atom("_NET_WM_STATE_MAXIMIZED_HORZ");
    wm.drop();
    np.postClick(xw, center(ui, ui.find("titlebar.max")));
    x11::ClientEv m{};
    CHECK(wm.next(a, xw, state, m) && m.l[0] == 1 && x11::Atom(m.l[1]) == maxV && x11::Atom(m.l[2]) == maxH);
    const long both[2] = {long(maxV), long(maxH)};
    wm.x.ChangeProperty(wm.d, xw, state, x11::kXA_ATOM, 32, x11::kPropModeReplace, reinterpret_cast<const unsigned char*>(both), 2);
    wm.x.ResizeWindow(wm.d, xw, 1000, 700);
    wm.x.Sync(wm.d, 0);
    for (int i = 0; i < 200 && !(win.chrome().maximized && win.pixelSize().w == 1000); ++i) a.step(0.005f);
    std::printf("  maximised: chrome %d, window %ux%u\n", win.chrome().maximized, win.pixelSize().w, win.pixelSize().h);
    CHECK(win.chrome().maximized && win.pixelSize().w == 1000 && win.pixelSize().h == 700);
    CHECK(app::chromeHitTest(win, {2.f, 150.f}) == HitArea::Client); // no resize edges while maximised
    for (int i = 0; i < 10; ++i) a.step(0.005f);
    wm.drop();
    np.postClick(xw, center(ui, ui.find("titlebar.max")));
    CHECK(wm.next(a, xw, state, m) && m.l[0] == 0);
    wm.x.DeleteProperty(wm.d, xw, state);
    wm.x.ResizeWindow(wm.d, xw, kW, kH);
    wm.x.Sync(wm.d, 0);
    for (int i = 0; i < 200 && (win.chrome().maximized || win.pixelSize().w != kW); ++i) a.step(0.005f);
    CHECK(!win.chrome().maximized && win.pixelSize().w == kW);
    for (int i = 0; i < 10; ++i) a.step(0.005f);
    // minimise: WM_CHANGE_STATE IconicState (what XIconifyWindow sends)
    wm.drop();
    np.postClick(xw, center(ui, ui.find("titlebar.min")));
    CHECK(wm.next(a, xw, wm.atom("WM_CHANGE_STATE"), m) && m.l[0] == 3);
    for (int i = 0; i < 10; ++i) a.step(0.005f);
    // close
    np.postClick(xw, center(ui, ui.find("titlebar.close")));
    for (int i = 0; i < 200 && a.windowCount() > 0; ++i) a.step(0.005f);
    CHECK(a.windowCount() == 0);
    std::printf("  max / restore / min / close via the FUSE title bar ok\n");
    return 0;
#else
    app::Application a{{.name = "gate_chrome_native", .loadSettings = false}};
    app::NativePlatform np(a);
    auto& win = a.createWindow({.title = "fuse_app custom chrome", .width = kW, .height = kH, .frame = app::WindowFrame::Custom});
    NodeHandle inCaption;
    build(win, inCaption);
    np.show(win);
    for (int i = 0; i < 400 && win.framesPresented() < 1; ++i) a.step(0.01f);
    for (int i = 0; i < 10; ++i) a.step(0.005f);
    HWND hwnd = reinterpret_cast<HWND>(np.nativeWindow(win));
    CHECK(hwnd != nullptr);
    if (!hwnd) return 1;
    RECT wr, cr;
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    std::printf("  window %ldx%ld, client %ldx%ld, UI %.0fx%.0f\n", wr.right - wr.left, wr.bottom - wr.top, cr.right, cr.bottom,
                win.size().w, win.size().h);
    CHECK(wr.right - wr.left == LONG(kW) && wr.bottom - wr.top == LONG(kH));
    CHECK(cr.right == LONG(kW) && cr.bottom == LONG(kH));
    int ok = 0, n = 0;
    for (const Probe& p : probes(win.ui(), inCaption)) {
        const LRESULT got = SendMessageW(hwnd, WM_NCHITTEST, 0, screenLp(hwnd, p.p));
        ++n;
        if (got == expectedHt(p.want)) ++ok;
        else std::fprintf(stderr, "  WM_NCHITTEST %s: %lld, want %lld\n", p.what, static_cast<long long>(got), static_cast<long long>(expectedHt(p.want)));
    }
    std::printf("  WM_NCHITTEST: %d / %d probes match\n", ok, n);
    CHECK(ok == n);

    UiContext& ui = win.ui();
    const NodeHandle maxBtn = ui.find("titlebar.max"), minBtn = ui.find("titlebar.min");
    SendMessageW(hwnd, WM_NCMOUSEMOVE, HTMAXBUTTON, screenLp(hwnd, center(ui, maxBtn)));
    CHECK(ui.hovered() == maxBtn);
    if (!out.empty()) {
        for (int i = 0; i < 5; ++i) { win.requestRedraw(); a.step(0.005f); }
        Image img;
        if (np.capture(win, img)) app::writePng(img, out + "/window_chrome.png");
    }
    // maximise / restore / minimise through non-client clicks on the FUSE buttons
    ncClick(hwnd, HTMAXBUTTON, center(ui, maxBtn));
    for (int i = 0; i < 10; ++i) a.step(0.005f);
    const bool zoomed = IsZoomed(hwnd) != 0;
    CHECK(zoomed && win.chrome().maximized);
    if (zoomed) {
        RECT mon{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &mon, 0);
        GetClientRect(hwnd, &cr);
        std::printf("  maximised: client %ldx%ld (work area %ldx%ld), UI %.0fx%.0f\n", cr.right, cr.bottom, mon.right - mon.left,
                    mon.bottom - mon.top, win.size().w, win.size().h);
        CHECK(app::chromeHitTest(win, {2.f, 150.f}) == HitArea::Client); // no resize edges while maximised
    }
    ncClick(hwnd, HTMAXBUTTON, center(ui, maxBtn));
    for (int i = 0; i < 10; ++i) a.step(0.005f);
    CHECK(!IsZoomed(hwnd) && !win.chrome().maximized);
    ncClick(hwnd, HTMINBUTTON, center(ui, minBtn));
    for (int i = 0; i < 10; ++i) a.step(0.005f);
    CHECK(IsIconic(hwnd));
    ShowWindow(hwnd, SW_RESTORE);
    for (int i = 0; i < 10; ++i) a.step(0.005f);
    // a press that starts on another button does not fire on release
    SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTMINBUTTON, 0);
    SendMessageW(hwnd, WM_NCLBUTTONUP, HTCLOSE, 0);
    for (int i = 0; i < 5; ++i) a.step(0.005f);
    CHECK(a.windowCount() == 1);
    ncClick(hwnd, HTCLOSE, center(ui, ui.find("titlebar.close")));
    for (int i = 0; i < 10 && a.windowCount() > 0; ++i) a.step(0.005f);
    CHECK(a.windowCount() == 0);
    CHECK(!IsWindow(hwnd));
    std::printf("  max / restore / min / close via non-client clicks ok\n");
    return 0;
#endif
}
} // namespace

int main(int argc, char** argv) {
    app::CommandLine cl(argc, argv);
    headless();
    const int r = native(std::string(cl.get("out")));
    if (g_fail) return 1;
    if (r == 77) return 77;
    std::printf("fuse_app_window_chrome: OK\n");
    return 0;
}
