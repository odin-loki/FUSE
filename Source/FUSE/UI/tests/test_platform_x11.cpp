// X11 platform parity gates (plan §4.13, UI1 / UI10) under Xvfb. The gate drives a second X connection of its own as
// the desktop would (RandR monitors, XSETTINGS manager, EWMH work area). One source, one executable per gate
// (FUSE_X11_GATE):
//   randr      displays() lists RandR 1.5 monitors (two user-defined ones), work areas from _NET_WORKAREA, follows
//              a monitor being removed
//   xsettings  an XSETTINGS manager's Xft/DPI and Net/ThemeName: read at start, live changes re-scale every window
//              (onDpiChanged) and switch dark / light (onThemeChanged); manager exit falls back to RESOURCE_MANAGER
//   wake       NativePlatform::wake() from another thread ends a blocked pump at once; an accessibility action
//              queued from an AT thread (WindowAccessibility bridge) runs without any X event
// Exit 77 without a display or the X extension.
#include "fuse/app/native.hpp"
#include "fuse/ui/widgets/widgets.hpp"
#include "x11_internal.hpp"
#if defined(FUSE_X11_GATE_A11Y)
#include "fuse/app/accessibility.hpp"
#endif

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifndef FUSE_X11_GATE
#define FUSE_X11_GATE "randr"
#endif

using namespace fuse::ui;
namespace app = fuse::app;
namespace x11 = fuse::app::x11;
namespace w = fuse::ui::widgets;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
/// The gate's own connection: what a desktop session (window manager, settings daemon) would do.
struct Desk {
    x11::Lib x;
    x11::Display* d = nullptr;
    x11::Window root = 0;
    bool open() {
        if (!x.load() || !(d = x.OpenDisplay(nullptr))) return false;
        root = x.RootWindow(d, x.DefaultScreen(d));
        return true;
    }
    ~Desk() {
        if (d) x.CloseDisplay(d);
    }
    x11::Atom atom(const char* n) { return x.InternAtom(d, n, 0); }
    void setCardinals(x11::Window win, const char* prop, std::initializer_list<long> v) {
        std::vector<long> a(v);
        x.ChangeProperty(d, win, atom(prop), x11::kXA_CARDINAL, 32, x11::kPropModeReplace, reinterpret_cast<const unsigned char*>(a.data()),
                         int(a.size()));
        x.Sync(d, 0);
    }
};
void pump(app::Application& a, int n = 20) {
    for (int i = 0; i < n; ++i) a.step(0.005f);
}
bool waitFor(app::Application& a, auto pred, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred() && std::chrono::steady_clock::now() < end) a.step(0.01f);
    return pred();
}
int skip(const char* gate, std::string_view why) {
    std::printf("%s: skipped (%.*s)\n", gate, int(why.size()), why.data());
    return 77;
}
const app::DisplayInfo* named(const std::vector<app::DisplayInfo>& ds, std::string_view n) {
    for (const auto& d : ds)
        if (d.name == n) return &d;
    return nullptr;
}
} // namespace

// ---- randr -----------------------------------------------------------------------------------------------------------
static int gateRandr(int argc, char** argv) {
    Desk desk;
    x11::Randr xr;
    int eb = 0, erb = 0;
    if (!desk.open()) return skip("fuse_x11_randr", "no X display");
    if (!xr.load() || !xr.QueryExtension(desk.d, &eb, &erb)) return skip("fuse_x11_randr", "no RandR (libXrandr.so.2 / extension)");
    // two monitors side by side: the left one takes the screen's output (replacing its automatic monitor)
    x11::ScreenResources* res = xr.GetScreenResourcesCurrent(desk.d, desk.root);
    const x11::XID out0 = res && res->noutput > 0 ? res->outputs[0] : 0;
    if (res) xr.FreeScreenResources(res);
    x11::MonitorInfo* l = xr.AllocateMonitor(desk.d, 1);
    l->name = desk.atom("FUSE-L");
    l->primary = 1;
    l->x = 0; l->y = 0; l->width = 640; l->height = 800; l->mwidth = 169; l->mheight = 211;
    l->noutput = out0 ? 1 : 0;
    l->outputs[0] = out0;
    xr.SetMonitor(desk.d, desk.root, l);
    x11::MonitorInfo* r = xr.AllocateMonitor(desk.d, 0);
    r->name = desk.atom("FUSE-R");
    r->x = 640; r->y = 0; r->width = 640; r->height = 800; r->mwidth = 169; r->mheight = 211;
    xr.SetMonitor(desk.d, desk.root, r);
    desk.x.Free(l);
    desk.x.Free(r);
    desk.setCardinals(desk.root, "_NET_WORKAREA", {0, 24, 1280, 776}); // a 24 px top panel

    app::Application a{{.name = "gate_x11_randr", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform native(a);
    if (!native.available()) return skip("fuse_x11_randr", native.lastError());
    auto ds = native.displays();
    for (const auto& d : ds)
        std::printf("  %s%s: %.0f,%.0f %.0fx%.0f work %.0f,%.0f %.0fx%.0f scale %.2f refresh %.1f Hz\n", d.name.c_str(),
                    d.primary ? " (primary)" : "", d.bounds.x, d.bounds.y, d.bounds.w, d.bounds.h, d.work.x, d.work.y, d.work.w,
                    d.work.h, d.scale, d.refreshHz);
    const app::DisplayInfo* dl = named(ds, "FUSE-L");
    const app::DisplayInfo* dr = named(ds, "FUSE-R");
    CHECK(dl && dr);
    if (dl && dr) {
        CHECK(dl->primary && !dr->primary);
        CHECK(dl->bounds.x == 0.f && dl->bounds.w == 640.f && dl->bounds.h == 800.f);
        CHECK(dr->bounds.x == 640.f && dr->bounds.y == 0.f && dr->bounds.w == 640.f);
        CHECK(dl->work.y == 24.f && dl->work.h == 776.f && dl->work.x == 0.f && dl->work.w == 640.f);
        CHECK(dr->work.x == 640.f && dr->work.y == 24.f && dr->work.w == 640.f && dr->work.h == 776.f);
        CHECK(dl->refreshHz == 0.f || (dl->refreshHz >= 20.f && dl->refreshHz <= 1000.f));
    }
    int primaries = 0;
    for (const auto& d : ds) primaries += d.primary;
    CHECK(primaries == 1);
    // a window shown at the origin lies on the left monitor; its popups get that monitor's work area
    auto& win = a.createWindow({.title = "fuse x11 randr", .width = 200, .height = 120});
    native.show(win);
    waitFor(a, [&] { return win.framesPresented() >= 1; });
    const Vec2 o = native.screenOrigin(win);
    CHECK(dl && o.x >= dl->bounds.x && o.x < dl->bounds.x + dl->bounds.w);
    // live: the right monitor goes away
    xr.DeleteMonitor(desk.d, desk.root, desk.atom("FUSE-R"));
    desk.x.Sync(desk.d, 0);
    ds = native.displays();
    std::printf("  after removing FUSE-R: %zu monitor(s)\n", ds.size());
    CHECK(named(ds, "FUSE-L") && !named(ds, "FUSE-R"));
    xr.DeleteMonitor(desk.d, desk.root, desk.atom("FUSE-L"));
    desk.x.DeleteProperty(desk.d, desk.root, desk.atom("_NET_WORKAREA"));
    desk.x.Sync(desk.d, 0);
    ds = native.displays();
    CHECK(!ds.empty() && ds[0].primary && ds[0].work.h == ds[0].bounds.h);
    return 0;
}

// ---- xsettings -------------------------------------------------------------------------------------------------------
namespace {
/// A minimal XSETTINGS manager (what gnome-settings-daemon / xsettingsd do).
struct XSettingsManager {
    Desk& desk;
    x11::Window win = 0;
    std::uint32_t serial = 0;
    explicit XSettingsManager(Desk& d) : desk(d) {}
    void set(int dpi, const std::string& theme) {
        std::vector<x11::XSetting> s(2);
        s[0].name = "Xft/DPI";
        s[0].integer = dpi * 1024;
        s[1].name = "Net/ThemeName";
        s[1].type = 1;
        s[1].string = theme;
        const auto bytes = x11::buildXSettings(s, ++serial);
        const x11::Atom prop = desk.atom("_XSETTINGS_SETTINGS");
        desk.x.ChangeProperty(desk.d, win, prop, prop, 8, x11::kPropModeReplace, bytes.data(), int(bytes.size()));
        desk.x.Sync(desk.d, 0);
    }
    void start(int dpi, const std::string& theme) {
        win = desk.x.CreateSimpleWindow(desk.d, desk.root, -10, -10, 1, 1, 0, 0, 0);
        set(dpi, theme);
        const x11::Atom sel = desk.atom("_XSETTINGS_S0");
        desk.x.SetSelectionOwner(desk.d, sel, win, 0);
        // MANAGER announcement: l[0] time, l[1] selection, l[2] owner
        x11::Event ev{};
        ev.client.type = x11::ClientMessage;
        ev.client.window = desk.root;
        ev.client.message_type = desk.atom("MANAGER");
        ev.client.format = 32;
        ev.client.l[1] = long(sel);
        ev.client.l[2] = long(win);
        desk.x.SendEvent(desk.d, desk.root, 0, x11::kStructureNotifyMask, &ev);
        desk.x.Sync(desk.d, 0);
    }
    void stop() {
        desk.x.DestroyWindow(desk.d, win);
        desk.x.Sync(desk.d, 0);
        win = 0;
    }
};
} // namespace

static int gateXSettings(int argc, char** argv) {
    ::unsetenv("FUSE_UI_SCALE");
    ::unsetenv("FUSE_UI_THEME");
    ::unsetenv("GTK_THEME");
    Desk desk;
    if (!desk.open()) return skip("fuse_x11_xsettings", "no X display");
    XSettingsManager mgr(desk);
    mgr.start(96, "Adwaita"); // light, 1x

    app::Application a{{.name = "gate_x11_xsettings", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform native(a);
    if (!native.available()) return skip("fuse_x11_xsettings", native.lastError());
    a.setFollowSystemTheme(true);
    int themeChanges = 0;
    bool dark = a.systemTheme().dark;
    a.onThemeChanged([&](app::Application&, const app::SystemTheme& t) { ++themeChanges; dark = t.dark; });
    CHECK(!native.querySystemTheme().dark && !a.systemTheme().dark); // Net/ThemeName "Adwaita" is light
    auto& wa = a.createWindow({.title = "fuse xsettings A", .width = 300, .height = 160});
    auto& wb = a.createWindow({.title = "fuse xsettings B", .width = 200, .height = 100});
    w::button(wa.ui(), wa.ui().root(), "Scaled", "b");
    int dpiCalls = 0;
    f32 seen = 0.f;
    wa.onDpiChanged([&](app::AppWindow&, f32 s) { ++dpiCalls; seen = s; });
    native.show(wa);
    native.show(wb);
    waitFor(a, [&] { return wa.framesPresented() >= 1 && wb.framesPresented() >= 1; });
    std::printf("  start: scale %.2f, theme %s\n", wa.scale(), a.systemTheme().dark ? "dark" : "light");
    CHECK(wa.scale() == 1.f && wb.scale() == 1.f);

    // live change: 144 dpi + a dark theme
    mgr.set(144, "Adwaita-dark");
    CHECK(waitFor(a, [&] { return wa.scale() == 1.5f && wb.scale() == 1.5f && themeChanges == 1; }));
    waitFor(a, [&] { return wa.pixelSize().w == 450u; }, 1000);
    std::printf("  Xft/DPI 144, Adwaita-dark: scale A %.2f B %.2f (callback %d x %.2f), pixels %ux%u, theme changes %d (%s)\n",
                wa.scale(), wb.scale(), dpiCalls, seen, wa.pixelSize().w, wa.pixelSize().h, themeChanges, dark ? "dark" : "light");
    CHECK(dpiCalls == 1 && seen == 1.5f);
    CHECK(wa.pixelSize().w == 450u && wa.pixelSize().h == 240u && wa.size().w == 300.f);
    CHECK(dark && a.systemTheme().dark);
    for (const auto& d : native.displays()) CHECK(d.scale == 1.5f);
    // a settings change that touches neither: no events
    mgr.set(144, "Adwaita-dark");
    pump(a);
    CHECK(dpiCalls == 1 && themeChanges == 1);

    // the manager exits: RESOURCE_MANAGER Xft.dpi (192) takes over, the theme falls back to the default
    const std::string rm = "Xft.dpi:\t192\n";
    desk.x.ChangeProperty(desk.d, desk.root, desk.atom("RESOURCE_MANAGER"), x11::kXA_STRING, 8, x11::kPropModeReplace,
                          reinterpret_cast<const unsigned char*>(rm.data()), int(rm.size()));
    mgr.stop();
    CHECK(waitFor(a, [&] { return wa.scale() == 2.f; }));
    std::printf("  manager gone, RESOURCE_MANAGER Xft.dpi 192: scale %.2f\n", wa.scale());
    desk.x.DeleteProperty(desk.d, desk.root, desk.atom("RESOURCE_MANAGER"));
    desk.x.Sync(desk.d, 0);
    CHECK(waitFor(a, [&] { return wa.scale() == 1.f; }));
    // a new manager announces itself
    mgr.start(120, "Breeze");
    CHECK(waitFor(a, [&] { return wa.scale() == 1.25f && !a.systemTheme().dark; }));
    std::printf("  new manager (120 dpi, Breeze): scale %.2f, theme %s, %d theme change(s)\n", wa.scale(),
                a.systemTheme().dark ? "dark" : "light", themeChanges);
    CHECK(!dark);
    // dpiAware = false keeps scale 1
    {
        app::Application a2{{.name = "gate_x11_xsettings_unaware", .loadSettings = false}};
        app::NativePlatform n2(a2, {.dpiAware = false});
        auto& w2 = a2.createWindow({.title = "unaware", .width = 100, .height = 60});
        n2.show(w2);
        pump(a2);
        CHECK(w2.scale() == 1.f);
    }
    mgr.stop();
    return 0;
}

// ---- wake ------------------------------------------------------------------------------------------------------------
static int gateWake(int argc, char** argv) {
    app::Application a{{.name = "gate_x11_wake", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform native(a);
    if (!native.available()) return skip("fuse_x11_wake", native.lastError());
    auto& win = a.createWindow({.title = "fuse wake", .width = 240, .height = 120});
    const NodeHandle b = w::button(win.ui(), win.ui().root(), "Press", "b");
    int clicks = 0;
    win.ui().connect(b, SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
    native.show(win);
    waitFor(a, [&] { return win.framesPresented() >= 1; });
    pump(a);
    // idle: a step with a long wait sleeps; wake() from another thread ends it
    using clk = std::chrono::steady_clock;
    for (int i = 0; i < 50; ++i) { // until a step really sleeps its whole wait (no redraw / X event pending)
        const auto s = clk::now();
        a.step(0.2f);
        if (clk::now() - s >= std::chrono::milliseconds(150)) break;
    }
    std::thread t([&native] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        native.wake();
    });
    const auto t0 = clk::now();
    a.step(5.f);
    const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    t.join();
    std::printf("  wake(): the blocked pump returned after %.0f ms\n", ms);
    CHECK(ms >= 100.0 && ms < 2000.0);
    // keyboard focus follows FocusIn / FocusOut (what the AT-SPI adapter is told)
    Desk desk;
    if (desk.open()) {
        desk.x.SetInputFocus(desk.d, x11::Window(native.nativeWindow(win)), 1 /*RevertToPointerRoot*/, 0);
        desk.x.Sync(desk.d, 0);
        const bool in = waitFor(a, [&] { return native.windowFocused(win); }, 1000);
        desk.x.SetInputFocus(desk.d, desk.root, 1, 0);
        desk.x.Sync(desk.d, 0);
        const bool out = waitFor(a, [&] { return !native.windowFocused(win); }, 1000);
        std::printf("  focus: in %d, out %d\n", in, out);
        CHECK(in && out);
    }
#if defined(FUSE_X11_GATE_A11Y)
    // an AT-SPI style action from another thread: queued on the bridge, the wake runs it without an X event
    app::WindowAccessibility acc(native, win);
    for (int i = 0; i < 50; ++i) {
        const auto s = clk::now();
        a.step(0.2f);
        if (clk::now() - s >= std::chrono::milliseconds(150)) break;
    }
    std::thread at([&acc, b] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        fuse::ui::a11y::ActionRequest r;
        r.action = fuse::ui::a11y::Action::Click;
        r.target = fuse::ui::a11y::idOf(b);
        acc.bridge().enqueue(r);
    });
    const auto t1 = clk::now();
    for (int i = 0; i < 3 && !clicks; ++i) a.step(5.f); // the click's press / release may take an extra iteration
    const double ms2 = std::chrono::duration<double, std::milli>(clk::now() - t1).count();
    at.join();
    std::printf("  accessibility click from an AT thread: %d click(s) after %.0f ms (adapter %s)\n", clicks, ms2,
                acc.active() ? "AccessKit" : "none");
    CHECK(clicks == 1 && ms2 < 2000.0);
#else
    (void)clicks;
#endif
    return 0;
}

int main(int argc, char** argv) {
    const std::string g = FUSE_X11_GATE;
    int r = 0;
    if (g == "randr") r = gateRandr(argc, argv);
    else if (g == "xsettings") r = gateXSettings(argc, argv);
    else if (g == "wake") r = gateWake(argc, argv);
    if (g_fail) {
        std::printf("fuse_x11_%s: FAILED (%d checks)\n", g.c_str(), g_fail);
        return 1;
    }
    if (r == 77) return 77;
    std::printf("fuse_x11_%s: OK\n", g.c_str());
    return r;
}
