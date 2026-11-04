// Desktop platform services gates (plan §4.13, UI1 / UI5) on real OS windows. One source, one executable per gate
// (FUSE_PLATFORM_GATE):
//   dpi_change      WM_DPICHANGED (X11: setWindowScale) re-scales a window: logical layout unchanged, pixel size,
//                   glyph size and the presented frame follow the new scale; going back restores the first frame
//   displays        at least one primary monitor with sane bounds / work area / scale / refresh rate
//   cursors         the widget under the pointer picks the OS cursor (I-beam, hand, resize; drag keeps it)
//   system_theme    OS theme query; a theme change (WM_SETTINGCHANGE) switches dark / light, the accent, the title
//                   bar and reduced motion
//   dialogs         file open / save / folder pickers and message boxes, dismissed by a helper thread (never waits
//                   for a human); URL / reveal argument checks
//   touch_synthetic injected touch (InitializeTouchInjection / InjectTouchInput): tap clicks, two fingers pinch
//   multiwindow     two windows with their own swapchains, input to the right one, close one, DPI per window
// Exit 77 when the OS feature (display, touch injection) is missing.
#include "fuse/app/native.hpp"
#include "fuse/app/platform.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef FUSE_PLATFORM_GATE
#define FUSE_PLATFORM_GATE "dpi_change"
#endif

using namespace fuse::ui;
namespace app = fuse::app;
namespace w = fuse::ui::widgets;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
void pump(app::Application& a, int n = 20) {
    for (int i = 0; i < n; ++i) a.step(0.005f);
}
bool waitFrames(app::Application& a, app::AppWindow& win, u64 frames = 1) {
    for (int i = 0; i < 600 && win.framesPresented() < frames; ++i) a.step(0.01f);
    pump(a);
    return win.framesPresented() >= frames;
}
/// What the OS shows vs the CPU raster of the same draw list (device pixels). Returns max channel error, 255 when the
/// sizes differ.
int presentError(app::NativePlatform& n, app::AppWindow& win, Image* shownOut = nullptr) {
    Image shown;
    if (!n.capture(win, shown)) return 255;
    const Image ref = win.rasterize({});
    if (shownOut) *shownOut = shown;
    if (shown.width != ref.width || shown.height != ref.height) {
        std::printf("  capture %dx%d vs raster %dx%d\n", shown.width, shown.height, ref.width, ref.height);
        return 255;
    }
    int e = 0;
    for (std::size_t i = 0; i < ref.pixels.size(); ++i)
        for (int c = 0; c < 3; ++c) e = std::max(e, std::abs(int((shown.pixels[i] >> (8 * c)) & 0xFF) - int((ref.pixels[i] >> (8 * c)) & 0xFF)));
    return e;
}
Vec2 centre(UiContext& ui, NodeHandle n, f32 scale) {
    const Rect r = ui.rect(n);
    return {(r.x + r.w * 0.5f) * scale, (r.y + r.h * 0.5f) * scale};
}
f32 maxGlyphH(const DrawList& dl) {
    f32 h = 0.f;
    for (const DrawCmd& c : dl.cmds())
        if (c.op == DrawOp::Glyph) h = std::max(h, c.rect.h);
    return h;
}
int skipNoDisplay(app::NativePlatform& n, const char* gate) {
    std::printf("%s: skipped (%.*s)\n", gate, int(n.lastError().size()), n.lastError().data());
    return 77;
}
/// Sends the DPI change the way Windows does (new DPI + suggested window rect); X11 uses setWindowScale.
void changeDpi(app::NativePlatform& n, app::AppWindow& win, f32 scale) {
#if defined(_WIN32)
    HWND hwnd = reinterpret_cast<HWND>(n.nativeWindow(win));
    const UINT dpi = UINT(std::lround(scale * 96.f));
    // the suggested rect keeps the logical client size; the frame stays the one the window has (the monitor's DPI
    // did not really change, so the OS keeps drawing the old frame)
    RECT wr{}, cr{};
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    const LONG cw = LONG(std::lround(win.size().w * scale)), ch = LONG(std::lround(win.size().h * scale));
    RECT suggested{wr.left, wr.top, wr.left + cw + (wr.right - wr.left) - cr.right, wr.top + ch + (wr.bottom - wr.top) - cr.bottom};
    SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
#else
    n.setWindowScale(win, scale);
#endif
}
} // namespace

// ---- dpi_change ------------------------------------------------------------------------------------------------------
static int gateDpiChange(int argc, char** argv) {
    app::Application a{{.name = "gate_dpi", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform native(a);
    if (!native.available()) return skipNoDisplay(native, "fuse_platform_dpi_change");
    auto& win = a.createWindow({.title = "fuse platform DPI", .width = 320, .height = 180});
    UiContext& ui = win.ui();
    const NodeHandle col = w::panel(ui, ui.root(), "col");
    ui.setCss(col, "padding", "10px");
    ui.setCss(col, "gap", "6px");
    const NodeHandle b = w::button(ui, col, "Scaled button", "b");
    w::lineEdit(ui, col, "line edit text", "le");
    w::label(ui, col, "The quick brown fox", "lbl");
    f32 seen = 0.f;
    int calls = 0;
    win.onDpiChanged([&seen, &calls](app::AppWindow&, f32 s) { seen = s; ++calls; });
    native.show(win);
    CHECK(waitFrames(a, win));
    const f32 s0 = win.scale();
    const Size logical0 = win.size();
    const Rect br0 = ui.rect(b);
    const f32 glyph0 = maxGlyphH(ui.drawList());
    Image first;
    const int e0 = presentError(native, win, &first);
    std::printf("  start: scale %.2f, logical %.0fx%.0f, pixels %ux%u, max glyph h %.0f, present error %d (%s)\n", s0,
                logical0.w, logical0.h, win.pixelSize().w, win.pixelSize().h, glyph0, e0,
                native.kind(win) == app::PresenterKind::Vulkan ? "vulkan" : "software");
    CHECK(e0 <= 2);

    const f32 s1 = s0 * 1.5f;
    changeDpi(native, win, s1);
    const u64 f0 = win.framesPresented();
    CHECK(waitFrames(a, win, f0 + 1));
    const Size logical1 = win.size();
    const Rect br1 = ui.rect(b);
    const f32 glyph1 = maxGlyphH(ui.drawList());
    const int e1 = presentError(native, win);
    std::printf("  after WM_DPICHANGED: scale %.2f (callback %.2f x%d), logical %.1fx%.1f, pixels %ux%u, glyph h %.0f, "
                "present error %d\n", win.scale(), seen, calls, logical1.w, logical1.h, win.pixelSize().w, win.pixelSize().h,
                glyph1, e1);
    CHECK(std::abs(win.scale() - s1) < 1e-4f);
    CHECK(calls == 1 && std::abs(seen - s1) < 1e-4f);
    CHECK(std::abs(logical1.w - logical0.w) <= 2.f && std::abs(logical1.h - logical0.h) <= 2.f); // same logical size
    CHECK(std::abs(f32(win.pixelSize().w) - logical1.w * s1) <= 1.f);
    CHECK(br1.x == br0.x && br1.y == br0.y && std::abs(br1.w - br0.w) <= 0.01f); // layout in UI units unchanged
    CHECK(glyph1 >= glyph0 * 1.3f && glyph1 <= glyph0 * 1.7f + 1.f);           // text re-rasterised at 1.5x
    CHECK(e1 <= 2);
    Image shown1;
    native.capture(win, shown1);
    CHECK(shown1.width == int(win.pixelSize().w));
    // the button face sits at its logical rect * scale
    {
        const Vec2 c = centre(ui, b, s1);
        const int fx = int(br1.x * s1 + 3.f * s1), fy = int(c.y);
        CHECK(shown1.width > fx && shown1.height > fy && shown1.at(fx, fy) != shown1.at(shown1.width - 2, shown1.height - 2));
    }
    // input in device pixels lands on the scaled widget
    int clicks = 0;
    ui.connect(b, SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
    CHECK(native.postClick(native.nativeWindow(win), centre(ui, b, s1)));
    for (int i = 0; i < 200 && !clicks; ++i) a.step(0.01f);
    CHECK(clicks == 1);

    changeDpi(native, win, s0); // back: the first frame again
    CHECK(waitFrames(a, win, win.framesPresented() + 1));
    Image back;
    const int e2 = presentError(native, win, &back);
    int diff = 255;
    if (back.width == first.width && back.height == first.height) {
        diff = 0;
        for (std::size_t i = 0; i < back.pixels.size(); ++i)
            for (int c = 0; c < 3; ++c) diff = std::max(diff, std::abs(int((back.pixels[i] >> (8 * c)) & 0xFF) - int((first.pixels[i] >> (8 * c)) & 0xFF)));
    }
    std::printf("  back to %.2f: pixels %ux%u, present error %d, max diff vs first frame %d\n", win.scale(), win.pixelSize().w,
                win.pixelSize().h, e2, diff);
    CHECK(std::abs(win.scale() - s0) < 1e-4f && e2 <= 2);
    CHECK(diff <= 40); // hover state of the clicked button may tint it; geometry and text match
    return 0;
}

// ---- displays --------------------------------------------------------------------------------------------------------
static int gateDisplays(int argc, char** argv) {
    app::Application a{{.name = "gate_displays", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform native(a);
    if (!native.available()) return skipNoDisplay(native, "fuse_platform_displays");
    const auto ds = native.displays();
    int primaries = 0;
    for (const auto& d : ds) {
        std::printf("  %s%s: %.0f,%.0f %.0fx%.0f work %.0f,%.0f %.0fx%.0f scale %.2f refresh %.0f Hz\n", d.name.c_str(),
                    d.primary ? " (primary)" : "", d.bounds.x, d.bounds.y, d.bounds.w, d.bounds.h, d.work.x, d.work.y, d.work.w,
                    d.work.h, d.scale, d.refreshHz);
        primaries += d.primary;
        CHECK(d.bounds.w >= 320.f && d.bounds.h >= 200.f);
        CHECK(d.work.w > 0.f && d.work.h > 0.f && d.work.x >= d.bounds.x && d.work.y >= d.bounds.y &&
              d.work.x + d.work.w <= d.bounds.x + d.bounds.w && d.work.y + d.work.h <= d.bounds.y + d.bounds.h);
        CHECK(d.scale >= 0.5f && d.scale <= 4.f);
        CHECK(d.refreshHz == 0.f || (d.refreshHz >= 20.f && d.refreshHz <= 1000.f));
    }
    CHECK(!ds.empty() && primaries == 1);
#if defined(_WIN32)
    CHECK(ds.size() == std::size_t(GetSystemMetrics(SM_CMONITORS)));
    for (const auto& d : ds)
        if (d.primary) CHECK(d.bounds.x == 0.f && d.bounds.y == 0.f); // the primary monitor holds the origin
#endif
    // a shown window lies on one of them
    auto& win = a.createWindow({.title = "fuse displays", .width = 200, .height = 120});
    native.show(win);
    waitFrames(a, win);
    const Vec2 o = native.screenOrigin(win);
    bool inside = false;
    for (const auto& d : ds) inside |= o.x >= d.bounds.x - 1.f && o.y >= d.bounds.y - 1.f && o.x < d.bounds.x + d.bounds.w && o.y < d.bounds.y + d.bounds.h;
    std::printf("  window origin %.0f,%.0f on a display: %s; window scale %.2f\n", o.x, o.y, inside ? "yes" : "no", win.scale());
    CHECK(inside);
    return 0;
}

// ---- cursors ---------------------------------------------------------------------------------------------------------
static int gateCursors(int argc, char** argv) {
    app::Application a{{.name = "gate_cursors", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform native(a);
    if (!native.available()) return skipNoDisplay(native, "fuse_platform_cursors");
    auto& win = a.createWindow({.title = "fuse cursors", .width = 360, .height = 260});
    UiContext& ui = win.ui();
    const NodeHandle col = w::panel(ui, ui.root(), "col");
    ui.setCss(col, "padding", "8px");
    ui.setCss(col, "gap", "6px");
    const NodeHandle le = w::lineEdit(ui, col, "type here", "le");
    const NodeHandle ln = w::link(ui, col, "a link", "ln");
    const NodeHandle bt = w::button(ui, col, "button", "bt");
    const NodeHandle sp = w::splitter(ui, col, false, "split");
    ui.setCss(sp, "height", "80px");
    w::splitterPane(ui, sp, 120.f);
    w::splitterPane(ui, sp);
    native.show(win);
    CHECK(waitFrames(a, win));
    NodeHandle handle;
    for (NodeHandle c = ui.firstChild(sp); ui.alive(c); c = ui.nextSibling(c))
        if (ui.registry().get(ui.classOf(c)).name == "splithandle") handle = c;
    CHECK(ui.alive(handle));
    const unsigned long long hw = native.nativeWindow(win);
    const f32 s = win.scale();
    struct Case { NodeHandle node; CursorShape want; const char* name; };
    const Case cases[] = {{le, CursorShape::Text, "line edit"}, {ln, CursorShape::Hand, "link"}, {bt, CursorShape::Arrow, "button"},
                          {handle, CursorShape::ResizeEW, "splitter handle"}};
    for (const Case& c : cases) {
        CHECK(native.postPointerMove(hw, centre(ui, c.node, s)));
        for (int i = 0; i < 100 && ui.hovered() != c.node; ++i) a.step(0.005f);
        pump(a, 4);
#if defined(_WIN32)
        // what Windows asks when the pointer moves over the client area; the handler answers with SetCursor
        SendMessageW(reinterpret_cast<HWND>(hw), WM_SETCURSOR, WPARAM(hw), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        static const LPCSTR ids[] = {IDC_ARROW, IDC_ARROW, IDC_IBEAM, IDC_HAND, IDC_SIZENS, IDC_SIZEWE};
        const HCURSOR want = LoadCursorA(nullptr, ids[std::size_t(c.want)]);
        const bool osOk = GetCursor() == want;
#else
        const bool osOk = true;
#endif
        std::printf("  %-16s hovered %s, ui cursor %s, applied %s, OS cursor %s\n", c.name, ui.hovered() == c.node ? "yes" : "no",
                    cursorShapeName(ui.cursor()), cursorShapeName(native.appliedCursor(win)), osOk ? "matches" : "differs");
        CHECK(ui.cursor() == c.want);
        CHECK(native.appliedCursor(win) == c.want);
        CHECK(osOk);
    }
    // dragging the handle keeps the resize cursor although the pointer leaves it
    const Vec2 hc = centre(ui, handle, s);
#if defined(_WIN32)
    PostMessageW(reinterpret_cast<HWND>(hw), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(int(hc.x), int(hc.y)));
    PostMessageW(reinterpret_cast<HWND>(hw), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(int(hc.x + 60.f * s), int(hc.y)));
    pump(a, 30);
    CHECK(ui.pressed() == handle);
    CHECK(native.appliedCursor(win) == CursorShape::ResizeEW);
    PostMessageW(reinterpret_cast<HWND>(hw), WM_LBUTTONUP, 0, MAKELPARAM(int(hc.x + 60.f * s), int(hc.y)));
    pump(a, 30);
#else
    ui.pointerDown({hc.x / s, hc.y / s});
    ui.pointerMove({hc.x / s + 60.f, hc.y / s});
    CHECK(ui.cursor() == CursorShape::ResizeEW);
    ui.pointerUp({hc.x / s + 60.f, hc.y / s});
#endif
    std::printf("  splitter drag kept %s\n", cursorShapeName(CursorShape::ResizeEW));
    return 0;
}

// ---- system theme ----------------------------------------------------------------------------------------------------
namespace {
app::SystemTheme g_fake;
app::SystemTheme fakeTheme(void*) { return g_fake; }
} // namespace
static int gateSystemTheme(int argc, char** argv) {
    {
        app::Application a{{.name = "gate_theme_os", .argc = argc, .argv = argv, .loadSettings = false}};
        app::NativePlatform native(a);
        if (!native.available()) return skipNoDisplay(native, "fuse_platform_system_theme");
        const app::SystemTheme t = native.querySystemTheme();
        std::printf("  OS theme: %s, accent #%02x%02x%02x, high contrast %d, reduced motion %d, text scale %.2f\n",
                    t.dark ? "dark" : "light", int(t.accent.r * 255.f + 0.5f), int(t.accent.g * 255.f + 0.5f),
                    int(t.accent.b * 255.f + 0.5f), t.highContrast, t.reducedMotion, t.textScale);
        CHECK(t.accent.a == 1.f && t.textScale >= 1.f && t.textScale <= 2.25f);
        CHECK(a.systemTheme() == t); // the platform fed it to the application at start
#if defined(_WIN32)
        DWORD v = 1, size = sizeof v;
        const bool light = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS || v != 0;
        CHECK(t.dark == !light);
        BOOL anim = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
        CHECK(t.reducedMotion == !anim);
#endif
    }
    // live changes through a replaced query (the gate must not change the user's settings)
    g_fake = {};
    g_fake.dark = true;
    app::Application a{{.name = "gate_theme", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform native(a, {.themeQuery = &fakeTheme});
    a.setFollowSystemTheme(true);
    int changes = 0;
    bool lastDark = true;
    a.onThemeChanged([&changes, &lastDark](app::Application&, const app::SystemTheme& t) { ++changes; lastDark = t.dark; });
    auto& win = a.createWindow({.title = "fuse theme", .width = 240, .height = 140});
    UiContext& ui = win.ui();
    const NodeHandle p = w::panel(ui, ui.root(), "p");
    w::button(ui, p, "Accent", "acc");
    ui.addClass(ui.find("acc"), "primary");
    native.show(win);
    CHECK(waitFrames(a, win));
    Image shot;
    native.capture(win, shot);
    const u32 darkPx = shot.width > 0 ? shot.at(shot.width - 3, shot.height - 3) : 0;
    const bool titleDark = native.darkTitleBar(win);
    const Color accent0 = ui.computed(ui.find("acc")).background;

    auto broadcast = [&](bool expectFrame) {
#if defined(_WIN32)
        SendMessageW(reinterpret_cast<HWND>(native.nativeWindow(win)), WM_SETTINGCHANGE, 0,
                     reinterpret_cast<LPARAM>(L"ImmersiveColorSet"));
#else
        native.refreshSystemTheme();
#endif
        if (expectFrame) CHECK(waitFrames(a, win, win.framesPresented() + 1)); // the restyle repaints by itself
        else pump(a);
    };
    g_fake.dark = false;
    g_fake.accent = Color::rgb(0xD03030);
    g_fake.reducedMotion = true;
    broadcast(true);
    native.capture(win, shot);
    const u32 lightPx = shot.width > 0 ? shot.at(shot.width - 3, shot.height - 3) : 0;
    const Color accent1 = ui.computed(ui.find("acc")).background;
    std::printf("  dark -> light: %d change(s), background 0x%08x -> 0x%08x, accent %.2f,%.2f,%.2f -> %.2f,%.2f,%.2f, "
                "title bar dark %d -> %d, reduced motion %d\n", changes, darkPx, lightPx, accent0.r, accent0.g, accent0.b,
                accent1.r, accent1.g, accent1.b, titleDark, native.darkTitleBar(win), ui.reducedMotion());
    CHECK(changes == 1 && !lastDark);
    CHECK((darkPx & 0xFF) < 0x40 && (lightPx & 0xFF) > 0xC0); // the window background followed the scheme
    CHECK(accent1.r > 0.7f && accent1.g < 0.3f);              // system accent applied to the theme tokens
    CHECK(ui.reducedMotion());
#if defined(_WIN32)
    CHECK(titleDark && !native.darkTitleBar(win));
#endif
    // reduced motion: an animation lands in one frame
    ui.animate(p, UiContext::AnimProp::Opacity, 0.2f, 2.f);
    pump(a, 3);
    CHECK(!ui.animating());
    // the same settings again: no change event
    broadcast(false);
    CHECK(changes == 1);
    g_fake.dark = true;
    g_fake.reducedMotion = false;
    broadcast(true);
    CHECK(changes == 2 && lastDark && !ui.reducedMotion());
#if defined(_WIN32)
    CHECK(native.darkTitleBar(win));
#endif
    // not following: the dark default theme stays whatever the OS says
    a.setFollowSystemTheme(false);
    g_fake.dark = false;
    broadcast(false);
    win.requestRedraw();
    waitFrames(a, win, win.framesPresented() + 1);
    native.capture(win, shot);
    CHECK(shot.width > 0 && (shot.at(shot.width - 3, shot.height - 3) & 0xFF) < 0x40);
    return 0;
}

// ---- dialogs ---------------------------------------------------------------------------------------------------------
#if defined(_WIN32)
namespace {
struct FoundDlg { DWORD pid; const wchar_t* title; HWND hwnd; };
HWND findDialog(const wchar_t* title) {
    FoundDlg f{GetCurrentProcessId(), title, nullptr};
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        auto& f = *reinterpret_cast<FoundDlg*>(l);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        wchar_t t[256]{};
        GetWindowTextW(h, t, 255);
        if (pid == f.pid && IsWindowVisible(h) && std::wcscmp(t, f.title) == 0) { f.hwnd = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.hwnd;
}
HWND findFileNameEdit(HWND dlg) { // the "File name" box: an Edit under the control with id 1148 (open) / 1001 (save)
    struct S { HWND dlg, edit; } s{dlg, nullptr};
    EnumChildWindows(dlg, [](HWND h, LPARAM l) -> BOOL {
        auto& s = *reinterpret_cast<S*>(l);
        wchar_t cls[64]{};
        GetClassNameW(h, cls, 63);
        if (std::wcscmp(cls, L"Edit") != 0 || !IsWindowVisible(h)) return TRUE;
        for (HWND a = h; a && a != s.dlg; a = GetParent(a))
            if (GetDlgCtrlID(a) == 1148 || GetDlgCtrlID(a) == 1001) { s.edit = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.edit;
}
/// Helper thread: waits for the dialog titled `title`, optionally types `text` into its file name box, then presses
/// `button` (IDOK / IDCANCEL / IDNO ...). Gives up (closing it) after a timeout so the gate can never hang.
std::thread dismiss(std::wstring title, std::wstring text, int button, std::atomic<int>& state) {
    return std::thread([title, text, button, &state] {
        HWND dlg = nullptr;
        for (int i = 0; i < 400 && !(dlg = findDialog(title.c_str())); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        if (!dlg) { state = -1; return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let it finish initialising
        if (!text.empty()) {
            HWND edit = nullptr;
            for (int i = 0; i < 80 && !(edit = findFileNameEdit(dlg)); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(25));
            if (!edit) { state = -2; PostMessageW(dlg, WM_COMMAND, IDCANCEL, 0); return; }
            SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
        }
        PostMessageW(dlg, WM_COMMAND, WPARAM(button), 0);
        for (int i = 0; i < 200 && IsWindow(dlg); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        if (IsWindow(dlg)) { state = -3; PostMessageW(dlg, WM_COMMAND, IDCANCEL, 0); PostMessageW(dlg, WM_CLOSE, 0, 0); return; }
        state = 1;
    });
}
std::wstring widen(const std::string& s) {
    std::wstring w(std::size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}
bool samePath(const std::string& a, const std::string& b) { return lstrcmpiW(widen(a).c_str(), widen(b).c_str()) == 0; }
} // namespace
#endif

static int gateDialogs(int, char**) {
    // non-interactive checks (every platform)
    CHECK(app::isOpenableUrl("https://example.org/x") && app::isOpenableUrl("HTTP://a.b") && app::isOpenableUrl("mailto:a@b.c"));
    CHECK(!app::isOpenableUrl("javascript:alert(1)") && !app::isOpenableUrl("") && !app::isOpenableUrl("https://") &&
          !app::isOpenableUrl("ftp://x") && !app::isOpenableUrl("https://a\"b"));
    CHECK(!app::openUrl("javascript:alert(1)") && app::openUrl("javascript:alert(1)").error() == app::PlatformError::InvalidArgument);
    CHECK(!app::revealInFileManager("") && app::revealInFileManager("").error() == app::PlatformError::InvalidArgument);
    const auto missing = app::revealInFileManager("Z:/definitely/not/here/fuse_missing.txt");
    CHECK(!missing && missing.error() == app::PlatformError::Failed);
#if defined(_WIN32)
    // a watchdog: whatever happens, the gate ends
    std::thread([] { std::this_thread::sleep_for(std::chrono::seconds(90)); std::fprintf(stderr, "dialog gate watchdog\n"); std::_Exit(3); }).detach();
    char tmp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, tmp);
    const std::string pid = std::to_string(GetCurrentProcessId());
    const std::string existing = std::string(tmp) + "fuse_dialog_gate_" + pid + ".txt";
    const std::string fresh = std::string(tmp) + "fuse_dialog_gate_" + pid + "_new.txt";
    if (FILE* f = std::fopen(existing.c_str(), "wb")) { std::fputs("x", f); std::fclose(f); }
    std::remove(fresh.c_str());
    const app::FileFilter filters[] = {{"Text files", "*.txt"}, {"All files", "*.*"}};
    {
        std::atomic<int> st{0};
        const std::string title = "fuse gate open " + pid;
        auto th = dismiss(widen(title), widen(existing), IDOK, st);
        const auto r = app::openFileDialog({.title = title, .folder = tmp, .filters = filters});
        th.join();
        std::printf("  open: helper %d, result %s\n", st.load(), r ? r->front().c_str() : app::platformErrorName(r.error()));
        CHECK(st == 1 && r && r->size() == 1 && samePath(r->front(), existing));
    }
    {
        std::atomic<int> st{0};
        const std::string title = "fuse gate save " + pid;
        auto th = dismiss(widen(title), widen(fresh), IDOK, st);
        const auto r = app::saveFileDialog({.title = title, .folder = tmp, .fileName = "untitled.txt", .filters = filters});
        th.join();
        std::printf("  save: helper %d, result %s\n", st.load(), r ? r->c_str() : app::platformErrorName(r.error()));
        CHECK(st == 1 && r && samePath(*r, fresh));
    }
    {
        std::atomic<int> st{0};
        const std::string title = "fuse gate folder " + pid;
        auto th = dismiss(widen(title), L"", IDCANCEL, st);
        const auto r = app::pickFolderDialog({.title = title, .folder = tmp});
        th.join();
        std::printf("  folder: helper %d, result %s\n", st.load(), r ? r->c_str() : app::platformErrorName(r.error()));
        CHECK(st == 1 && !r && r.error() == app::PlatformError::Cancelled);
    }
    {
        std::atomic<int> st{0};
        const std::string title = "fuse gate question " + pid;
        auto th = dismiss(widen(title), L"", IDNO, st);
        const auto r = app::messageBox(title, "Keep going?", app::MessageIcon::Question, app::MessageButtons::YesNo);
        th.join();
        std::printf("  message box (yes / no): helper %d, answer %s\n", st.load(), r ? (*r == app::MessageResult::No ? "no" : "other") : "error");
        CHECK(st == 1 && r && *r == app::MessageResult::No);
    }
    {
        std::atomic<int> st{0};
        const std::string title = "fuse gate warning " + pid;
        auto th = dismiss(widen(title), L"", IDCANCEL, st);
        const auto r = app::messageBox(title, "Discard?", app::MessageIcon::Warning, app::MessageButtons::OkCancel);
        th.join();
        CHECK(st == 1 && r && *r == app::MessageResult::Cancel);
    }
    std::remove(existing.c_str());
    std::remove(fresh.c_str());
#else
    std::printf("  POSIX: dialogs need a human (zenity); only argument checks run\n");
#endif
    return 0;
}

// ---- touch -----------------------------------------------------------------------------------------------------------
#if defined(_WIN32)
namespace {
using InitTouch = BOOL(WINAPI*)(UINT32, DWORD);
using InjectTouch = BOOL(WINAPI*)(UINT32, const POINTER_TOUCH_INFO*);
POINTER_TOUCH_INFO contact(UINT32 id, POINT p, POINTER_FLAGS flags) {
    POINTER_TOUCH_INFO t{};
    t.pointerInfo.pointerType = PT_TOUCH;
    t.pointerInfo.pointerId = id;
    t.pointerInfo.ptPixelLocation = p;
    t.pointerInfo.pointerFlags = flags;
    t.touchFlags = TOUCH_FLAG_NONE;
    t.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
    t.orientation = 90;
    t.pressure = 32000;
    t.rcContact = {p.x - 2, p.y - 2, p.x + 2, p.y + 2};
    return t;
}
} // namespace
#endif

static int gateTouch(int argc, char** argv) {
#if !defined(_WIN32)
    (void)argc; (void)argv;
    std::printf("fuse_platform_touch_synthetic: skipped (touch injection is Win32 only here)\n");
    return 77;
#else
    app::Application a{{.name = "gate_touch", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform native(a);
    auto& win = a.createWindow({.title = "fuse touch", .width = 360, .height = 220});
    UiContext& ui = win.ui();
    const NodeHandle row = w::panel(ui, ui.root(), "row");
    ui.addClass(row, "row");
    ui.setCss(row, "padding", "20px");
    const NodeHandle b1 = w::button(ui, row, "Tap me", "b1");
    const NodeHandle b2 = w::button(ui, row, "Not me", "b2");
    ui.setCss(b1, "width", "120px");
    ui.setCss(b2, "width", "120px");
    int c1 = 0, c2 = 0;
    ui.connect(b1, SignalKind::Clicked, [&c1](UiContext&, NodeHandle, const SignalArgs&) { ++c1; });
    ui.connect(b2, SignalKind::Clicked, [&c2](UiContext&, NodeHandle, const SignalArgs&) { ++c2; });
    native.show(win);
    HWND hwnd = reinterpret_cast<HWND>(native.nativeWindow(win));
    SetWindowPos(hwnd, HWND_TOPMOST, 40, 40, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW); // touch hits the window on top
    CHECK(waitFrames(a, win));
    pump(a, 20);
    const f32 s = win.scale();
    const Vec2 o = native.screenOrigin(win);
    auto screen = [&](NodeHandle n, f32 dx = 0.f) {
        const Vec2 c = centre(ui, n, s);
        return POINT{LONG(o.x + c.x + dx), LONG(o.y + c.y)};
    };
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto init = reinterpret_cast<InitTouch>(reinterpret_cast<void*>(GetProcAddress(user32, "InitializeTouchInjection")));
    const auto inject = reinterpret_cast<InjectTouch>(reinterpret_cast<void*>(GetProcAddress(user32, "InjectTouchInput")));
    const bool injection = init && inject && init(10, TOUCH_FEEDBACK_NONE);
    const POINTER_FLAGS down = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
    const POINTER_FLAGS move = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
    const POINTER_FLAGS up = POINTER_FLAG_UP;
    bool injected = false;
    if (injection) {
        POINTER_TOUCH_INFO t = contact(0, screen(b1), down);
        injected = inject(1, &t) != 0;
        if (injected) {
            for (int i = 0; i < 200 && ui.touches().empty(); ++i) a.step(0.005f);
            t.pointerInfo.pointerFlags = up;
            inject(1, &t);
            for (int i = 0; i < 200 && !c1; ++i) a.step(0.01f);
        } else {
            std::printf("  InjectTouchInput failed (%lu)\n", GetLastError());
        }
    } else {
        std::printf("  InitializeTouchInjection unavailable (%lu)\n", GetLastError());
    }
    if (!injected) { // no injection here: WM_POINTER messages the decoder treats as touch (unknown pointer ids)
        const POINT p = screen(b1);
        PostMessageW(hwnd, WM_POINTERDOWN, MAKEWPARAM(4242, POINTER_MESSAGE_FLAG_NEW | POINTER_MESSAGE_FLAG_INCONTACT), MAKELPARAM(p.x, p.y));
        PostMessageW(hwnd, WM_POINTERUP, MAKEWPARAM(4242, 0), MAKELPARAM(p.x, p.y));
        for (int i = 0; i < 100 && !c1; ++i) a.step(0.01f);
    }
    std::printf("  tap via %s: button clicks %d / %d, pointer kind %s, touches left %zu\n",
                injected ? "InjectTouchInput" : "posted WM_POINTER", c1, c2,
                ui.pointerKind() == UiContext::PointerKind::Touch ? "touch" : "other", ui.touches().size());
    CHECK(c1 == 1 && c2 == 0);
    CHECK(ui.pointerKind() == UiContext::PointerKind::Touch && ui.touches().empty());

    // two fingers: the primary presses b1, the second lands on b2 (no press there) and spreads: pinch scale 2
    f32 pinch = 0.f;
    std::size_t contacts = 0;
    NodeHandle pressed;
    if (injected) {
        // Dispatch message by message so the transient two-contact state is seen (a pump batch may hold down and up).
        auto observe = [&] {
            if (!contacts && ui.touches().size() == 2) { contacts = 2; pressed = ui.pressed(); }
            if (ui.touches().size() == 2) pinch = std::max(pinch, ui.pinchScale());
        };
        auto drain = [&](int ms) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < end) {
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                    observe();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        POINTER_TOUCH_INFO t[2] = {contact(0, screen(b1), down), contact(1, screen(b2), down)};
        drain(150);
        const BOOL okDown = inject(2, t);
        if (!okDown) std::printf("  two-finger InjectTouchInput failed (%lu)\n", GetLastError());
        CHECK(okDown);
        drain(40);
        t[0].pointerInfo.pointerFlags = move;
        t[1].pointerInfo.pointerFlags = move;
        const LONG x2 = t[1].pointerInfo.ptPixelLocation.x, dx = x2 - t[0].pointerInfo.ptPixelLocation.x;
        for (int k = 0; k <= 8; ++k) { // spread the second finger to twice the distance
            t[1].pointerInfo.ptPixelLocation.x = x2 + dx * k / 8;
            CHECK(inject(2, t));
            drain(15);
        }
        t[0].pointerInfo.pointerFlags = up;
        t[1].pointerInfo.pointerFlags = up;
        inject(2, t);
        drain(100);
        pump(a, 10);
    } else {
        const POINT p1 = screen(b1), p2 = screen(b2), p3{p2.x + (p2.x - p1.x), p2.y};
        PostMessageW(hwnd, WM_POINTERDOWN, MAKEWPARAM(5001, 0), MAKELPARAM(p1.x, p1.y));
        PostMessageW(hwnd, WM_POINTERDOWN, MAKEWPARAM(5002, 0), MAKELPARAM(p2.x, p2.y));
        pump(a, 10);
        contacts = ui.touches().size();
        pressed = ui.pressed();
        PostMessageW(hwnd, WM_POINTERUPDATE, MAKEWPARAM(5002, 0), MAKELPARAM(p3.x, p3.y));
        pump(a, 10);
        pinch = ui.pinchScale();
        PostMessageW(hwnd, WM_POINTERUP, MAKEWPARAM(5002, 0), MAKELPARAM(p3.x, p3.y));
        PostMessageW(hwnd, WM_POINTERUP, MAKEWPARAM(5001, 0), MAKELPARAM(p1.x, p1.y));
        pump(a, 20);
    }
    std::printf("  two fingers: %zu contacts, primary pressed b1 %s, pinch scale %.2f, clicks %d / %d\n", contacts,
                pressed == b1 ? "yes" : "no", pinch, c1, c2);
    CHECK(contacts == 2 && pressed == b1);
    CHECK(std::abs(pinch - 2.f) < 0.1f);
    CHECK(c1 == 2 && c2 == 0 && ui.touches().empty());
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    return 0;
#endif
}

// ---- multiwindow -----------------------------------------------------------------------------------------------------
static int gateMultiwindow(int argc, char** argv) {
    app::Application a{{.name = "gate_multiwindow", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform native(a);
    if (!native.available()) return skipNoDisplay(native, "fuse_platform_multiwindow");
    auto& wa = a.createWindow({.title = "fuse window A", .width = 300, .height = 160});
    auto& wb = a.createWindow({.title = "fuse window B", .width = 340, .height = 200});
    int ca = 0, cb = 0;
    const NodeHandle ba = w::button(wa.ui(), wa.ui().root(), "Button A", "ba");
    const NodeHandle bb = w::button(wb.ui(), wb.ui().root(), "Button B", "bb");
    w::label(wb.ui(), wb.ui().root(), "second window", "lb");
    wa.ui().connect(ba, SignalKind::Clicked, [&ca](UiContext&, NodeHandle, const SignalArgs&) { ++ca; });
    wb.ui().connect(bb, SignalKind::Clicked, [&cb](UiContext&, NodeHandle, const SignalArgs&) { ++cb; });
    const app::PresenterKind ka = native.show(wa), kb = native.show(wb);
    CHECK(waitFrames(a, wa) && waitFrames(a, wb));
    const unsigned long long ha = native.nativeWindow(wa), hb = native.nativeWindow(wb);
    std::printf("  presenters: A %s, B %s (%.*s)\n", ka == app::PresenterKind::Vulkan ? "vulkan" : "software",
                kb == app::PresenterKind::Vulkan ? "vulkan" : "software", int(native.gpuName().size()), native.gpuName().data());
    CHECK(ha && hb && ha != hb && ka == kb);
    CHECK(presentError(native, wa) <= 2 && presentError(native, wb) <= 2);
    Image ia, ib;
    native.capture(wa, ia);
    native.capture(wb, ib);
    CHECK(ia.width == int(wa.pixelSize().w) && ib.width == int(wb.pixelSize().w) && ia.width != ib.width);

    // input goes to the window it was sent to
    CHECK(native.postClick(hb, centre(wb.ui(), bb, wb.scale())));
    for (int i = 0; i < 200 && !cb; ++i) a.step(0.01f);
    CHECK(cb == 1 && ca == 0);
    CHECK(native.postClick(ha, centre(wa.ui(), ba, wa.scale())));
    for (int i = 0; i < 200 && !ca; ++i) a.step(0.01f);
    CHECK(ca == 1 && cb == 1);

    // DPI per window: B moves to a 1.5x monitor, A stays
    const f32 sa = wa.scale(), sb = wb.scale();
    changeDpi(native, wb, sb * 1.5f);
    CHECK(waitFrames(a, wb, wb.framesPresented() + 1));
    std::printf("  DPI: A %.2f -> %.2f, B %.2f -> %.2f (pixels %ux%u)\n", sa, wa.scale(), sb, wb.scale(), wb.pixelSize().w, wb.pixelSize().h);
    CHECK(wa.scale() == sa && std::abs(wb.scale() - sb * 1.5f) < 1e-4f);
    CHECK(presentError(native, wb) <= 2 && presentError(native, wa) <= 2);
    CHECK(native.postClick(hb, centre(wb.ui(), bb, wb.scale())));
    for (int i = 0; i < 200 && cb < 2; ++i) a.step(0.01f);
    CHECK(cb == 2 && ca == 1);

    // closing A through the OS keeps B (and the app) alive
    CHECK(native.postClose(ha));
    bool running = true;
    for (int i = 0; i < 200 && a.windowCount() > 1 && running; ++i) running = a.step(0.01f);
    std::printf("  after closing A: %u window(s), app running %s\n", a.windowCount(), running ? "yes" : "no");
    CHECK(running && a.windowCount() == 1 && a.window(0) == &wb);
    const u64 before = wb.framesPresented();
    wb.requestRedraw();
    CHECK(waitFrames(a, wb, before + 1));
    CHECK(native.postClick(hb, centre(wb.ui(), bb, wb.scale())));
    for (int i = 0; i < 200 && cb < 3; ++i) a.step(0.01f);
    CHECK(cb == 3);
    CHECK(presentError(native, wb) <= 2);
    return 0;
}

int main(int argc, char** argv) {
    const std::string gate = FUSE_PLATFORM_GATE;
    int rc = 0;
    if (gate == "dpi_change") rc = gateDpiChange(argc, argv);
    else if (gate == "displays") rc = gateDisplays(argc, argv);
    else if (gate == "cursors") rc = gateCursors(argc, argv);
    else if (gate == "system_theme") rc = gateSystemTheme(argc, argv);
    else if (gate == "dialogs") rc = gateDialogs(argc, argv);
    else if (gate == "touch_synthetic") rc = gateTouch(argc, argv);
    else if (gate == "multiwindow") rc = gateMultiwindow(argc, argv);
    else { std::fprintf(stderr, "unknown gate %s\n", gate.c_str()); return 2; }
    if (rc == 77) return 77;
    if (g_fail || rc) {
        std::fprintf(stderr, "fuse_platform_%s: FAILED (%d checks)\n", gate.c_str(), g_fail);
        return 1;
    }
    std::printf("fuse_platform_%s: OK\n", gate.c_str());
    return 0;
}
