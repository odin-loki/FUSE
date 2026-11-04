// Custom window chrome gate (plan §4.13 Windows): WindowFrame::Custom + the FUSE UI title bar.
//   * chromeHitTest (headless): caption, the four edges and corners, min / max / close buttons, client area, an
//     interactive widget inside the caption (client), System frames (always client); the close button closes
//   * on a real Win32 window: client rect == window rect (WM_NCCALCSIZE removed the OS frame), WM_NCHITTEST at screen
//     points answers HTCAPTION / HTLEFT / HTBOTTOMRIGHT / HTMINBUTTON / HTMAXBUTTON / HTCLOSE / HTCLIENT ...,
//     WM_NCMOUSEMOVE hovers the FUSE button, non-client clicks on the buttons maximise / restore / minimise / close
// Exit 77 without a display or off Win32 (X11 custom chrome not done). --out=<dir> writes window_chrome.png.
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
#endif

using namespace fuse::ui;
namespace app = fuse::app;
namespace w = fuse::ui::widgets;
using app::HitArea;

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

int native(const std::string& out) {
#if !defined(_WIN32)
    (void)out;
    std::printf("fuse_app_window_chrome: native part skipped (custom chrome is Win32 only for now)\n");
    return 77;
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
