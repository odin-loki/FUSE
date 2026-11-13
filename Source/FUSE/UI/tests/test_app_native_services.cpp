// fuse_app native platform-services gate (plan §4.13): real OS windows (X11 under Xvfb; the API is platform neutral).
//   * HiDPI: NativeOptions::scale = 2 gives a window of logical size x 2 pixels, rendered at that resolution (the
//     read-back equals the CPU raster), input in window pixels lands on the logical layout, OS popup windows scale
//   * mouse cursor: what UiContext::cursor() asks for is applied to the OS window after the pump
//   * clipboard: the CLIPBOARD selection between two display connections (owner serves, the other converts)
//   * window management: position, show / hide, focus, size limits and their events
//   * displays, file dialog helper (zenity found in PATH / cancelled / unavailable -> toolkit fallback), openUrl
//   * window-less dialogs (open / save / folder / colour / message box through fake zenity / kdialog), revealInFileManager
//   * clipboard formats between two connections: image (lazy, PNG), custom MIME type (600 kB, INCR), PRIMARY
//   * XDND between two connections: a UI drag leaves window A and drops text / files on a drop target of window B
//   * OS tool windows: a floating dock in its own window (read-back, click, title drag, follows the main window)
//   * client-side chrome: caption drag moves, edges resize within the size limits, postClose / close button
//   * saved state: the next Application's window comes back with the saved position, size and UI state
//   * XSETTINGS: the gate is the settings manager; DPI, theme and cursor size are read at start and followed live
// Exit 77 (skip) without a display. --out=<dir>: scratch directory for the fake helper programs and screenshots.
#include "fuse/app/chrome.hpp"
#include "fuse/app/native.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if !defined(_WIN32)
#include <dlfcn.h>
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
// ---- clipboard formats: images, MIME types, lazy items, INCR, PRIMARY (two display connections) ----------------------
struct Lazy {
    int calls = 0;
};
bool provideThumb(void* u, app::ClipboardItem& it) {
    ++static_cast<Lazy*>(u)->calls;
    it.image.resize(48, 32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 48; ++x)
            it.image.pixels[std::size_t(y) * 48 + std::size_t(x)] = 0x80000000u | (u32(x * 5) & 0xFFu) | ((u32(y * 8) & 0xFFu) << 8) | ((u32(x + y) & 0xFFu) << 16);
    return true;
}
bool provideNothing(void*, app::ClipboardItem&) { return false; }
template <class Fn>
auto whileServing(app::Application& owner, Fn&& fn) {
    std::atomic<bool> stop{false};
    std::thread t([&] {
        while (!stop.load()) owner.step(0.002f);
    });
    auto r = fn();
    stop = true;
    t.join();
    return r;
}
bool hasFormat(const std::vector<std::string>& v, std::string_view f) {
    for (const std::string& s : v)
        if (s == f) return true;
    return false;
}
void clipboardFormats() {
    app::Application a{{.name = "gate_services_fmt_a", .loadSettings = false}};
    app::Application b{{.name = "gate_services_fmt_b", .loadSettings = false}};
    app::NativePlatform na(a, {.vulkan = false}), nb(b, {.vulkan = false});
    app::AppWindow& wa = a.createWindow({.title = "fmt a", .width = 120, .height = 80});
    app::AppWindow& wb = b.createWindow({.title = "fmt b", .width = 120, .height = 80});
    na.show(wa);
    nb.show(wb);

    std::vector<u8> blob(600000); // larger than one property: travels as INCR chunks
    for (std::size_t i = 0; i < blob.size(); ++i) blob[i] = u8((i * 2654435761u) >> 13);
    Lazy lazy;
    std::vector<app::ClipboardItem> items(4);
    items[0].mime = app::kClipboardText;
    items[0].text = "formats \xE2\x9C\x93";
    items[1].mime = app::kClipboardImage;
    items[1].provide = &provideThumb; // made only when someone pastes it
    items[1].user = &lazy;
    items[2].mime = "application/x-fuse-gate-nodes";
    items[2].bytes = blob;
    items[3].mime = "application/x-fuse-gate-refused";
    items[3].provide = &provideNothing;
    CHECK(na.setClipboard(std::move(items)));
    CHECK(na.clipboardOwned() && !nb.clipboardOwned());
    CHECK(lazy.calls == 0);

    // the other connection: formats, text, the custom type (INCR), the image (lazy, PNG on the wire)
    const std::vector<std::string> formats = whileServing(a, [&] { return nb.clipboardFormats(); });
    CHECK(hasFormat(formats, app::kClipboardText) && hasFormat(formats, app::kClipboardImage) && hasFormat(formats, "application/x-fuse-gate-nodes"));
    CHECK(!hasFormat(formats, "TARGETS"));
    CHECK(lazy.calls == 0); // listing formats does not make the lazy ones
    std::string text;
    CHECK(whileServing(a, [&] { return nb.clipboardText(text); }) && text == "formats \xE2\x9C\x93");
    std::vector<u8> got;
    CHECK(whileServing(a, [&] { return nb.clipboardData("application/x-fuse-gate-nodes", got); }));
    CHECK(got == blob);
    Image img;
    CHECK(whileServing(a, [&] { return nb.clipboardImage(img); }));
    CHECK(lazy.calls == 1);
    Image want;
    {
        app::ClipboardItem tmp;
        Lazy unused;
        provideThumb(&unused, tmp);
        want = tmp.image;
    }
    CHECK(img.width == 48 && img.height == 32 && img.pixels == want.pixels); // straight alpha survives
    CHECK(whileServing(a, [&] { return nb.clipboardImage(img); }));
    CHECK(lazy.calls == 1); // provided once
    std::vector<u8> png;
    CHECK(whileServing(a, [&] { return nb.clipboardData(app::kClipboardImage, png); }) && png.size() > 8 && png[1] == 'P' && png[2] == 'N');
    // a format its provider refuses, and one nobody offers
    CHECK(!whileServing(a, [&] { return nb.clipboardData("application/x-fuse-gate-refused", got); }));
    CHECK(!whileServing(a, [&] { return nb.clipboardData("application/x-fuse-gate-absent", got); }));
    // the owner reads its own formats without a round trip
    CHECK(na.clipboardData("application/x-fuse-gate-nodes", got) && got == blob);
    CHECK(na.clipboardImage(img) && img.pixels == want.pixels);
    CHECK(hasFormat(na.clipboardFormats(), app::kClipboardImage));

    // an image set directly; ownership moves and the previous owner is told
    Image red;
    red.resize(8, 8, 0xFF0000FFu);
    CHECK(nb.setClipboardImage(red));
    for (int i = 0; i < 20; ++i) a.step(0.005f);
    CHECK(!na.clipboardOwned() && nb.clipboardOwned());
    CHECK(whileServing(b, [&] { return na.clipboardImage(img); }) && img.width == 8 && img.pixels == red.pixels);
    const std::vector<std::string> f2 = whileServing(b, [&] { return na.clipboardFormats(); });
    CHECK(hasFormat(f2, app::kClipboardImage) && !hasFormat(f2, app::kClipboardText));
    CHECK(!whileServing(b, [&] { return na.clipboardText(text); })); // an image-only clipboard has no text

    // PRIMARY is a separate selection
    CHECK(na.setPrimarySelectionText("primary text"));
    CHECK(na.primarySelectionText(text) && text == "primary text");
    CHECK(whileServing(a, [&] { return nb.primarySelectionText(text); }) && text == "primary text");
    CHECK(nb.clipboardOwned()); // the clipboard was not touched
    CHECK(nb.clipboardImage(img) && img.pixels == red.pixels);
}

// ---- XDND between two display connections: a drag that starts in window A leaves it and is dropped on window B -------
struct DndLog {
    int enters = 0, moves = 0, leaves = 0, drops = 0, ends = 0;
    DropAction endAction = DropAction::None;
    u32 dropType = 0;
    std::string dropText;
    Vec2 dropPos{};
};
void dndSlot(void* u, const SignalArgs& a) {
    if (!a.event) return;
    DndLog& l = *static_cast<DndLog*>(u);
    switch (a.event->type) {
    case EventType::DragEnter: ++l.enters; break;
    case EventType::DragMove: ++l.moves; break;
    case EventType::DragLeave: ++l.leaves; break;
    case EventType::Drop:
        ++l.drops;
        l.dropPos = a.event->pos;
        if (a.event->drag) {
            l.dropType = a.event->drag->type;
            l.dropText = a.event->drag->text;
        }
        break;
    case EventType::DragEnd:
        ++l.ends;
        l.endAction = a.event->dropAction;
        break;
    default: break;
    }
}
bool textSource(UiContext&, NodeHandle, Vec2, DragData& out, DragImage&) {
    out.type = kDragText;
    out.text = "dragged \xC3\xA9 text";
    return true;
}
bool fileSource(UiContext&, NodeHandle, Vec2, DragData& out, DragImage&) {
    out.type = kDragFiles;
    out.text = "/tmp/fuse gate/a b.txt\n/tmp/c\xC3\xA9.png";
    return true;
}
bool privateSource(UiContext&, NodeHandle, Vec2, DragData& out, DragImage&) {
    out.type = dragType("application/x-fuse-gate-private"); // in-app only: never offered to other applications
    out.text = "private";
    return true;
}
void xdnd() {
    app::Application a{{.name = "gate_services_dnd_a", .loadSettings = false}};
    app::Application b{{.name = "gate_services_dnd_b", .loadSettings = false}};
    app::NativePlatform na(a, {.vulkan = false, .scale = 1.f}), nb(b, {.vulkan = false, .scale = 1.f});
    app::AppWindow& wa = a.createWindow({.title = "dnd source", .width = 300, .height = 200});
    app::AppWindow& wb = b.createWindow({.title = "dnd target", .width = 320, .height = 240});
    UiContext& ua = wa.ui();
    UiContext& ub = wb.ui();
    static DndLog srcLog, fileLog, privLog, textLog, filesLog;
    srcLog = fileLog = privLog = textLog = filesLog = {};
    const NodeHandle srcText = w::button(ua, ua.root(), "text", "src.text");
    const NodeHandle srcFile = w::button(ua, ua.root(), "files", "src.files");
    const NodeHandle srcPriv = w::button(ua, ua.root(), "private", "src.private");
    ua.setDragSource(srcText, &textSource);
    ua.setDragSource(srcFile, &fileSource);
    ua.setDragSource(srcPriv, &privateSource);
    ua.connect(srcText, SignalKind::Event, [](UiContext&, NodeHandle, const SignalArgs& s) { dndSlot(&srcLog, s); });
    ua.connect(srcFile, SignalKind::Event, [](UiContext&, NodeHandle, const SignalArgs& s) { dndSlot(&fileLog, s); });
    ua.connect(srcPriv, SignalKind::Event, [](UiContext&, NodeHandle, const SignalArgs& s) { dndSlot(&privLog, s); });
    // the target window: one node takes text, one takes files, the rest takes nothing
    ub.setCss(ub.root(), "flex-direction", "row");
    const NodeHandle dropText = w::panel(ub, ub.root(), "drop.text");
    const NodeHandle dropFiles = w::panel(ub, ub.root(), "drop.files");
    for (NodeHandle n : {dropText, dropFiles}) {
        ub.setCss(n, "width", "100px");
        ub.setCss(n, "height", "100px");
    }
    ub.setDropTypes(dropText, {kDragText});
    ub.setDropTypes(dropFiles, {kDragFiles});
    ub.connect(dropText, SignalKind::Event, [](UiContext&, NodeHandle, const SignalArgs& s) { dndSlot(&textLog, s); });
    ub.connect(dropFiles, SignalKind::Event, [](UiContext&, NodeHandle, const SignalArgs& s) { dndSlot(&filesLog, s); });
    na.show(wa);
    nb.show(wb);
    wb.setPosition({500.f, 120.f}); // side by side: nothing overlaps
    auto both = [&](int n = 6) {
        for (int i = 0; i < n; ++i) {
            a.step(0.003f);
            b.step(0.003f);
        }
    };
    both(20);
    CHECK(nb.screenOrigin(wb).x == 500.f && nb.screenOrigin(wb).y == 120.f);
    const unsigned long long ha = na.nativeWindow(wa);
    const Vec2 oa = na.screenOrigin(wa), ob = nb.screenOrigin(wb);
    auto overB = [&](NodeHandle n, Vec2 off = {}) { // a point of B's node, in A's window coordinates
        const Vec2 c = centre(ub.rect(n));
        return Vec2{ob.x - oa.x + c.x + off.x, ob.y - oa.y + c.y + off.y};
    };
    auto startDrag = [&](NodeHandle src) {
        const Vec2 p = centre(ua.rect(src));
        na.postPointerMove(ha, p);
        na.postPointerButton(ha, p, true);
        both(2);
        na.postPointerMove(ha, {p.x + 6.f, p.y + 3.f});
        na.postPointerMove(ha, {p.x + 14.f, p.y + 6.f});
        both(3);
        return ua.dragging();
    };

    // 1. text: A -> B's text target
    CHECK(startDrag(srcText));
    CHECK(!na.externalDragActive()); // still inside its own window
    na.postPointerMove(ha, overB(dropText));
    both(8);
    CHECK(na.externalDragActive());
    CHECK(ub.dragging() && ub.dragTarget() == dropText); // hover feedback in the other application
    CHECK(ub.states(dropText) & kStateDragOver);
    CHECK(textLog.enters == 1 && textLog.moves >= 1 && textLog.drops == 0);
    na.postPointerMove(ha, overB(dropText, {10.f, -5.f}));
    both(4);
    na.postPointerButton(ha, overB(dropText, {10.f, -5.f}), false);
    CHECK(pumpUntil(a, [&] { b.step(0.003f); return srcLog.ends == 1; }));
    CHECK(textLog.drops == 1 && textLog.dropType == kDragText && textLog.dropText == "dragged \xC3\xA9 text");
    const Vec2 wantPos{centre(ub.rect(dropText)).x + 10.f, centre(ub.rect(dropText)).y - 5.f};
    CHECK(std::fabs(textLog.dropPos.x - wantPos.x) <= 1.f && std::fabs(textLog.dropPos.y - wantPos.y) <= 1.f);
    CHECK(srcLog.endAction == DropAction::Copy); // what the other side did
    CHECK(!ua.dragging() && !ub.dragging() && !na.externalDragActive());
    CHECK(!(ub.states(dropText) & kStateDragOver));

    // 2. files: paths with spaces / UTF-8 travel as a uri-list and arrive as LF-separated paths
    CHECK(startDrag(srcFile));
    na.postPointerMove(ha, overB(dropFiles));
    both(8);
    CHECK(ub.dragTarget() == dropFiles);
    na.postPointerButton(ha, overB(dropFiles), false);
    CHECK(pumpUntil(a, [&] { b.step(0.003f); return fileLog.ends == 1; }));
    CHECK(filesLog.drops == 1 && filesLog.dropType == kDragFiles && filesLog.dropText == "/tmp/fuse gate/a b.txt\n/tmp/c\xC3\xA9.png");
    CHECK(fileLog.endAction == DropAction::Copy);

    // 3. a target that does not take the type refuses: the files source over the text-only node, then over no node
    fileLog = {};
    textLog = {};
    CHECK(startDrag(srcFile));
    na.postPointerMove(ha, overB(dropText));
    both(8);
    CHECK(!ub.dragTarget().valid());
    na.postPointerButton(ha, overB(dropText), false);
    CHECK(pumpUntil(a, [&] { b.step(0.003f); return fileLog.ends == 1; }));
    CHECK(fileLog.endAction == DropAction::None && textLog.drops == 0);
    both(4);
    CHECK(!ub.dragging() && !na.externalDragActive());

    // 4. leaving the other window again sends it a leave; coming home drops inside as usual
    srcLog = {};
    textLog = {};
    CHECK(startDrag(srcText));
    na.postPointerMove(ha, overB(dropText));
    both(8);
    CHECK(textLog.enters == 1);
    na.postPointerMove(ha, {150.f, 100.f}); // back over A
    both(8);
    CHECK(textLog.leaves == 1 && !ub.dragging() && !na.externalDragActive());
    na.postPointerButton(ha, {150.f, 100.f}, false);
    both(4);
    CHECK(srcLog.ends == 1 && srcLog.endAction == DropAction::None && textLog.drops == 0);

    // 5. an in-app type is never offered outside
    CHECK(startDrag(srcPriv));
    na.postPointerMove(ha, overB(dropText));
    both(8);
    CHECK(!na.externalDragActive() && !ub.dragging());
    na.postPointerButton(ha, overB(dropText), false);
    both(4);
    CHECK(privLog.ends == 1 && privLog.endAction == DropAction::None);

    // 6. NativeOptions::dragAndDrop = false: the window is no drop target
    app::Application c{{.name = "gate_services_dnd_c", .loadSettings = false}};
    app::NativePlatform nc(c, {.vulkan = false, .scale = 1.f, .dragAndDrop = false});
    app::AppWindow& wc = c.createWindow({.title = "no dnd", .width = 200, .height = 150});
    const NodeHandle dropC = w::panel(wc.ui(), wc.ui().root(), "drop");
    wc.ui().setCss(dropC, "flex-grow", "1");
    wc.ui().setDropTypes(dropC, {kDragText});
    nc.show(wc);
    wc.setPosition({860.f, 400.f});
    for (int i = 0; i < 20; ++i) c.step(0.003f);
    srcLog = {};
    CHECK(startDrag(srcText));
    const Vec2 oc = nc.screenOrigin(wc);
    na.postPointerMove(ha, {oc.x - oa.x + 100.f, oc.y - oa.y + 75.f});
    for (int i = 0; i < 8; ++i) { a.step(0.003f); c.step(0.003f); }
    CHECK(!na.externalDragActive() && !wc.ui().dragging());
    na.postPointerButton(ha, {oc.x - oa.x + 100.f, oc.y - oa.y + 75.f}, false);
    both(4);
    CHECK(srcLog.ends == 1 && srcLog.endAction == DropAction::None);
}

// ---- OS tool windows: a floating dock in its own OS window ------------------------------------------------------------
void toolWindows(const std::string& out) {
    for (const bool osWindows : {false, true}) {
        app::Application a{{.name = "gate_services_tools", .loadSettings = false}};
        app::NativePlatform native(a, {.vulkan = false, .scale = 1.f, .osWindows = osWindows});
        app::AppWindow& win = a.createWindow({.title = "tools", .width = 400, .height = 300});
        UiContext& ui = win.ui();
        const NodeHandle area = w::dockArea(ui, ui.root(), "area");
        ui.setCss(area, "flex-grow", "1");
        const NodeHandle dock = w::dockWidget(ui, area, "Inspector", w::DockSide::Left, "insp");
        const NodeHandle btn = w::button(ui, w::dockBody(ui, dock), "Apply", "apply");
        static int clicks;
        clicks = 0;
        ui.connect(btn, SignalKind::Clicked, [](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
        native.show(win);
        win.setPosition({100.f, 80.f});
        CHECK(pumpUntil(a, [&] { return win.framesPresented() >= 1 && native.screenOrigin(win).x == 100.f; }));
        CHECK(ui.osWindows() == osWindows);
        CHECK(native.toolWindowCount(win) == 0);
        w::dockFloat(ui, dock, {440.f, 30.f, 220.f, 160.f}); // to the right of the main window
        for (int i = 0; i < 10; ++i) a.step(0.005f);
        if (!osWindows) { // the default: an in-window panel, kept reachable inside the viewport
            CHECK(native.toolWindowCount(win) == 0 && ui.windowSurfaces().empty());
            CHECK(w::dockFloatRect(ui, dock).x <= 360.f);
            continue;
        }
        CHECK(native.toolWindowCount(win) == 1);
        const unsigned long long tool = native.toolWindow(win, 0);
        CHECK(tool != 0 && tool != native.nativeWindow(win));
        Rect scr = native.toolWindowScreenRect(win, 0);
        CHECK(scr.x == 540.f && scr.y == 110.f && scr.w == 220.f && scr.h == 160.f); // window origin + float rect
        // what the OS shows in the tool window is the CPU raster of its surface
        Image shot;
        CHECK(native.captureToolWindow(win, 0, shot));
        CHECK(shot.width == 220 && shot.height == 160);
        if (!ui.windowSurfaces().empty() && shot.width == 220) {
            Image ref;
            ref.resize(220, 160, shot.pixels[0]);
            CpuRasterizer rz = win.rasterizer();
            rz.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
            rz.render(ui.windowSurfaces()[0].draw, ref);
            int worst = 0;
            for (std::size_t i = 0; i < ref.pixels.size(); ++i)
                for (int ch = 0; ch < 3; ++ch) worst = std::max(worst, std::abs(int((ref.pixels[i] >> (8 * ch)) & 0xFF) - int((shot.pixels[i] >> (8 * ch)) & 0xFF)));
            std::printf("  tool window read-back %dx%d, max channel error vs CPU raster %d\n", shot.width, shot.height, worst);
            CHECK(worst <= 1);
            if (!out.empty()) app::writePng(shot, out + "/native_tool_window.png");
        }
        // the main window no longer paints the panel
        Image mainShot;
        CHECK(native.capture(win, mainShot) && mainShot.width == 400);
        // a click in the tool window reaches the widget in context coordinates
        const Rect br = ui.rect(btn);
        const Rect fr = w::dockFloatRect(ui, dock);
        CHECK(native.postClick(tool, {br.x - fr.x + br.w * 0.5f, br.y - fr.y + br.h * 0.5f}));
        CHECK(pumpUntil(a, [&] { return clicks == 1; }));
        // dragging the panel's title moves the OS window (the press is in the tool window, the pointer leaves it)
        const Vec2 grab{60.f, 10.f};
        native.postPointerMove(tool, grab);
        native.postPointerButton(tool, grab, true);
        for (int i = 0; i < 4; ++i) a.step(0.005f);
        native.postPointerMove(tool, {grab.x + 30.f, grab.y + 20.f});
        for (int i = 0; i < 4; ++i) a.step(0.005f);
        native.postPointerMove(tool, grab); // the window followed the pointer: the grab point is under it again
        for (int i = 0; i < 4; ++i) a.step(0.005f);
        native.postPointerButton(tool, grab, false);
        for (int i = 0; i < 10; ++i) a.step(0.005f);
        const Rect moved = w::dockFloatRect(ui, dock);
        std::printf("  tool window dragged: float rect %.0f,%.0f -> %.0f,%.0f\n", fr.x, fr.y, moved.x, moved.y);
        CHECK(w::dockFloating(ui, dock));
        CHECK(moved.x > fr.x + 20.f && moved.y > fr.y + 10.f && moved.w == fr.w);
        scr = native.toolWindowScreenRect(win, 0);
        CHECK(scr.x == 100.f + moved.x && scr.y == 80.f + moved.y);
        // the tool window follows the main window
        win.setPosition({160.f, 120.f});
        CHECK(pumpUntil(a, [&] { return native.toolWindowScreenRect(win, 0).x == 160.f + moved.x; }));
        CHECK(native.toolWindowScreenRect(win, 0).y == 120.f + moved.y);
        // a second floating panel is a second window, in paint order; docking takes a window away
        const NodeHandle dock2 = w::dockWidget(ui, area, "Log", w::DockSide::Bottom, "log");
        w::dockFloat(ui, dock2, {-260.f, 40.f, 200.f, 120.f}); // left of the main window
        for (int i = 0; i < 10; ++i) a.step(0.005f);
        CHECK(native.toolWindowCount(win) == 2);
        CHECK(native.toolWindowScreenRect(win, 1).x == 160.f - 260.f || native.toolWindowScreenRect(win, 0).x == 160.f - 260.f);
        w::dockTo(ui, dock, w::DockSide::Right);
        for (int i = 0; i < 10; ++i) a.step(0.005f);
        CHECK(native.toolWindowCount(win) == 1);
        CHECK(native.toolWindowScreenRect(win, 0).w == 200.f);
        // dock splits next to tool windows: a tool window docks into a split beside a docked panel (its OS window
        // goes), a panel floated out of the split is a tool window again and the split collapses
        const NodeHandle dock3 = w::dockWidget(ui, area, "Notes", w::DockSide::Right, "notes");
        for (int i = 0; i < 4; ++i) a.step(0.005f);
        CHECK(w::dockSplit(ui, dock2, dock, w::DockEdge::Bottom) && w::dockSplit(ui, dock3, dock2, w::DockEdge::Center));
        for (int i = 0; i < 10; ++i) a.step(0.005f);
        CHECK(native.toolWindowCount(win) == 0 && !ui.hasClass(dock2, "os-window"));
        CHECK(w::dockSplitOf(ui, dock2).valid() && w::dockSplitOf(ui, dock2) == w::dockSplitOf(ui, dock));
        CHECK(w::dockSaveLayout(ui, area).rfind("fusedock 2\n", 0) == 0);
        w::dockFloat(ui, dock, {420.f, 30.f, 210.f, 150.f});
        for (int i = 0; i < 10; ++i) a.step(0.005f);
        CHECK(native.toolWindowCount(win) == 1 && native.toolWindowScreenRect(win, 0).w == 210.f);
        CHECK(!w::dockSplitOf(ui, dock2).valid() && w::dockTabBar(ui, dock2).valid()); // the tab group stays, unsplit
        const std::string layout = w::dockSaveLayout(ui, area);
        w::dockTo(ui, dock, w::DockSide::Left);
        for (int i = 0; i < 10; ++i) a.step(0.005f);
        CHECK(native.toolWindowCount(win) == 0);
        CHECK(w::dockRestoreLayout(ui, area, layout)); // the saved layout brings the tool window back
        for (int i = 0; i < 10; ++i) a.step(0.005f);
        CHECK(native.toolWindowCount(win) == 1 && native.toolWindowScreenRect(win, 0).w == 210.f);
        win.requestClose();
        a.step(0.f);
        CHECK(a.windowCount() == 0u);
    }
}

// ---- client-side chrome on a real window: caption drag, edge resize, buttons (no window manager under Xvfb) ----------
void chromeAndClose() {
    app::Application a{{.name = "gate_services_chrome", .loadSettings = false}};
    app::NativePlatform native(a, {.vulkan = false, .scale = 1.f});
    app::AppWindow& win = a.createWindow({.title = "chrome", .width = 400, .height = 300, .frame = app::WindowFrame::Custom});
    UiContext& ui = win.ui();
    app::titleBar(win, ui.root(), "chrome");
    const NodeHandle body = w::button(ui, ui.root(), "Body", "body");
    static int bodyClicks;
    bodyClicks = 0;
    ui.connect(body, SignalKind::Clicked, [](UiContext&, NodeHandle, const SignalArgs&) { ++bodyClicks; });
    win.setPosition({200.f, 150.f}); // before show(): the window is created there
    native.show(win);
    CHECK(pumpUntil(a, [&] { return win.framesPresented() >= 1; }));
    CHECK(native.screenOrigin(win).x == 200.f && native.screenOrigin(win).y == 150.f);
    const unsigned long long h = native.nativeWindow(win);
    auto settle = [&] { for (int i = 0; i < 8; ++i) a.step(0.004f); };
    if (win.mode() != app::WindowMode::Normal) return;
    // caption: the press starts a window move and never reaches the UI
    native.postPointerMove(h, {150.f, 16.f});
    native.postPointerButton(h, {150.f, 16.f}, true);
    settle();
    CHECK(!ui.pressed().valid());
    native.postPointerMove(h, {200.f, 46.f}); // + 50, + 30
    settle();
    CHECK(native.screenOrigin(win).x == 250.f && native.screenOrigin(win).y == 180.f);
    native.postPointerButton(h, {150.f, 16.f}, false); // the caption is under the pointer again
    settle();
    CHECK(win.position().x == 250.f && win.position().y == 180.f && win.size().w == 400.f);
    // right edge: resize, the left edge stays
    native.postPointerMove(h, {398.f, 150.f});
    settle();
    CHECK(native.cursor(win) == Cursor::ResizeH);
    native.postPointerButton(h, {398.f, 150.f}, true);
    native.postPointerMove(h, {438.f, 150.f});
    CHECK(pumpUntil(a, [&] { return win.size().w == 440.f; }));
    native.postPointerButton(h, {438.f, 150.f}, false);
    settle();
    CHECK(win.size().w == 440.f && win.size().h == 300.f && native.screenOrigin(win).x == 250.f);
    // top-left corner: both axes, the opposite corner stays
    native.postPointerMove(h, {1.f, 1.f});
    settle();
    CHECK(native.cursor(win) == Cursor::ResizeNWSE);
    native.postPointerButton(h, {1.f, 1.f}, true);
    native.postPointerMove(h, {-19.f, -9.f});
    CHECK(pumpUntil(a, [&] { return win.size().w == 460.f && win.size().h == 310.f; }));
    native.postPointerButton(h, {1.f, 1.f}, false);
    settle();
    CHECK(native.screenOrigin(win).x == 230.f && native.screenOrigin(win).y == 170.f);
    // the size limits hold while dragging an edge
    win.setMinimumSize({300.f, 200.f});
    settle();
    native.postPointerButton(h, {458.f, 150.f}, true);
    native.postPointerMove(h, {58.f, 150.f}); // far past the minimum
    CHECK(pumpUntil(a, [&] { return win.size().w == 300.f; }));
    native.postPointerButton(h, {298.f, 150.f}, false);
    settle();
    CHECK(win.size().w == 300.f);
    // the client area still gets its clicks; the title-bar buttons are widgets
    native.postPointerMove(h, centre(ui.rect(body)));
    settle();
    CHECK(native.cursor(win) != Cursor::ResizeH);
    CHECK(native.postClick(h, centre(ui.rect(body))));
    CHECK(pumpUntil(a, [&] { return bodyClicks == 1; }));
    // closing: postClose is the window manager's close button (the guard may veto), titlebar.close the client-side one
    static bool allow;
    allow = false;
    win.setCloseGuard({nullptr, [](void*, app::AppWindow&) { return allow; }});
    CHECK(native.postClose(h));
    settle();
    CHECK(!win.closing());
    allow = true;
    CHECK(native.postClick(h, centre(ui.rect(ui.find("titlebar.close")))));
    CHECK(pumpUntil(a, [&] { return a.windowCount() == 0u; }));
}

// ---- XSETTINGS: the gate is the desktop's settings manager on its own X connection (libX11 loaded like the backend does)
struct SettingsManager {
    using Display = void;
    void* so = nullptr;
    Display* dpy = nullptr;
    unsigned long win = 0, prop = 0;
    Display* (*OpenDisplay)(const char*) = nullptr;
    int (*CloseDisplay)(Display*) = nullptr;
    int (*DefaultScreen)(Display*) = nullptr;
    unsigned long (*RootWindow)(Display*, int) = nullptr;
    unsigned long (*CreateSimpleWindow)(Display*, unsigned long, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long) = nullptr;
    unsigned long (*InternAtom)(Display*, const char*, int) = nullptr;
    int (*SetSelectionOwner)(Display*, unsigned long, unsigned long, unsigned long) = nullptr;
    int (*ChangeProperty)(Display*, unsigned long, unsigned long, unsigned long, int, int, const unsigned char*, int) = nullptr;
    int (*Sync)(Display*, int) = nullptr;
    bool start() {
        so = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!so) return false;
        auto get = [&](auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(so, name)); return fn != nullptr; };
        if (!get(OpenDisplay, "XOpenDisplay") || !get(CloseDisplay, "XCloseDisplay") || !get(DefaultScreen, "XDefaultScreen") ||
            !get(RootWindow, "XRootWindow") || !get(CreateSimpleWindow, "XCreateSimpleWindow") || !get(InternAtom, "XInternAtom") ||
            !get(SetSelectionOwner, "XSetSelectionOwner") || !get(ChangeProperty, "XChangeProperty") || !get(Sync, "XSync"))
            return false;
        dpy = OpenDisplay(nullptr);
        if (!dpy) return false;
        const int scr = DefaultScreen(dpy);
        win = CreateSimpleWindow(dpy, RootWindow(dpy, scr), -20, -20, 1, 1, 0, 0, 0); // never mapped
        prop = InternAtom(dpy, "_XSETTINGS_SETTINGS", 0);
        SetSelectionOwner(dpy, InternAtom(dpy, ("_XSETTINGS_S" + std::to_string(scr)).c_str(), 0), win, 0);
        return win != 0;
    }
    void publish(const std::vector<app::XSetting>& settings, u32 serial) {
        const std::vector<u8> bytes = app::buildXSettings(settings, serial);
        ChangeProperty(dpy, win, prop, prop, 8, 0, bytes.data(), int(bytes.size()));
        Sync(dpy, 0);
    }
    ~SettingsManager() {
        if (dpy) CloseDisplay(dpy); // the selection goes with the connection
        if (so) dlclose(so);
    }
};
void xsettings() {
    using T = app::WindowEvent::Type;
    if (std::getenv("FUSE_UI_SCALE")) ::unsetenv("FUSE_UI_SCALE"); // it would win over the desktop's DPI
    SettingsManager manager;
    if (!manager.start()) {
        std::printf("  xsettings: skipped (cannot act as the settings manager)\n");
        return;
    }
    manager.publish({{"Xft/DPI", 0, 96 * 1024, {}}, {"Net/ThemeName", 1, 0, "Adwaita"}, {"Gtk/CursorThemeSize", 0, 24, {}}}, 1);
    app::Application a{{.name = "gate_services_xsettings", .loadSettings = false}};
    app::NativePlatform native(a, {.vulkan = false}); // scale 0 = the desktop's
    CHECK(a.systemTheme().scheme == app::ColorScheme::Light); // read at start, before any window
    app::AppWindow& win = a.createWindow({.title = "xsettings", .width = 300, .height = 200});
    Events ev;
    win.setEventHandler({&ev, &onEvent});
    const NodeHandle link = w::link(win.ui(), win.ui().root(), "link", "lnk");
    native.show(win);
    CHECK(pumpUntil(a, [&] { return win.framesPresented() >= 1; }));
    CHECK(native.scale(win) == 1.f && win.pixelSize().w == 300.f);
    const bool themedCursors = native.cursorThemeSize() > 0; // libXcursor present
    if (themedCursors) CHECK(native.cursorThemeSize() == 24);
    // the desktop switches to 192 dpi, a dark theme and larger cursors: followed live
    manager.publish({{"Xft/DPI", 0, 192 * 1024, {}}, {"Net/ThemeName", 1, 0, "Adwaita-dark"}, {"Gtk/CursorThemeSize", 0, 48, {}}}, 2);
    CHECK(pumpUntil(a, [&] { return native.scale(win) == 2.f && win.pixelSize().w == 600.f; }));
    CHECK(win.scale() == 2.f && win.size().w == 300.f && win.size().h == 200.f); // the logical size is kept
    CHECK(win.pixelSize().h == 400.f);
    CHECK(ev.saw(T::ScaleChanged) && ev.saw(T::ThemeChanged));
    CHECK(a.systemTheme().scheme == app::ColorScheme::Dark && native.systemTheme().scheme == app::ColorScheme::Dark);
    if (themedCursors) CHECK(native.cursorThemeSize() == 48);
    for (int i = 0; i < 6; ++i) a.step(0.004f);
    Image shot;
    CHECK(native.capture(win, shot) && shot.width == 600 && shot.height == 400); // the OS window was resized
    // input still lands on the logical layout, and the cursor is re-made from the new theme size
    native.postPointerMove(native.nativeWindow(win), {centre(win.ui().rect(link)).x * 2.f, centre(win.ui().rect(link)).y * 2.f});
    CHECK(pumpUntil(a, [&] { return native.cursor(win) == Cursor::PointingHand; }));
    // "prefer dark" wins over a light theme name; a new window starts at the current scale
    manager.publish({{"Xft/DPI", 0, 192 * 1024, {}}, {"Net/ThemeName", 1, 0, "Adwaita"}, {"Gtk/ApplicationPreferDarkTheme", 0, 1, {}}}, 3);
    for (int i = 0; i < 10; ++i) a.step(0.004f);
    CHECK(a.systemTheme().scheme == app::ColorScheme::Dark);
    app::AppWindow& second = a.createWindow({.title = "second", .width = 100, .height = 80});
    native.show(second);
    CHECK(native.scale(second) == 2.f && second.pixelSize().w == 200.f);
    manager.publish({{"Xft/DPI", 0, 96 * 1024, {}}, {"Net/ThemeName", 1, 0, "Adwaita"}}, 4);
    CHECK(pumpUntil(a, [&] { return native.scale(win) == 1.f && native.scale(second) == 1.f; }));
    CHECK(a.systemTheme().scheme == app::ColorScheme::Light && win.pixelSize().w == 300.f);
    // a window that follows the system theme, with the client-side title bar: the desktop's theme restyles the bar
    // (its sheet names theme tokens only), and a high-contrast theme name selects the high-contrast token set
    second.setFollowSystemTheme(true);
    const NodeHandle bar = app::titleBar(second, second.ui().root(), "second");
    auto barIs = [&](w::ThemeVariant v) {
        Color surface;
        for (int i = 0; i < 40; ++i) a.step(0.016f); // style transitions run out
        return w::themeTokenColor({.variant = v}, "--surface", surface) && second.ui().computed(bar).background == surface;
    };
    CHECK(barIs(w::ThemeVariant::Light));
    manager.publish({{"Xft/DPI", 0, 96 * 1024, {}}, {"Net/ThemeName", 1, 0, "Adwaita-dark"}}, 5);
    CHECK(pumpUntil(a, [&] { return a.systemTheme().scheme == app::ColorScheme::Dark; }));
    CHECK(!a.systemTheme().highContrast && barIs(w::ThemeVariant::Dark));
    manager.publish({{"Xft/DPI", 0, 96 * 1024, {}}, {"Net/ThemeName", 1, 0, "HighContrast"}}, 6);
    CHECK(pumpUntil(a, [&] { return a.systemTheme().highContrast; }));
    CHECK(a.systemTheme().scheme == app::ColorScheme::Light && barIs(w::ThemeVariant::HighContrast));
    CHECK(second.ui().computed(second.chrome().close).color == w::HighContrastPalette{}.windowText);
    manager.publish({{"Xft/DPI", 0, 96 * 1024, {}}, {"Net/ThemeName", 1, 0, "Adwaita"}}, 7);
    CHECK(pumpUntil(a, [&] { return !a.systemTheme().highContrast; }));
    CHECK(barIs(w::ThemeVariant::Light));
}

// ---- saved state on real windows: the next process's window comes back where and how it was ---------------------------
void savedStateRestore(const std::string& out) {
    if (out.empty()) return;
    const std::string file = out + "/gate_services_state.ini";
    {
        app::Application a{{.name = "gate_services_state_a", .loadSettings = false}};
        app::NativePlatform native(a, {.vulkan = false, .scale = 1.f});
        app::AppWindow& win = a.createWindow({.title = "state", .width = 320, .height = 200});
        const NodeHandle edit = w::lineEdit(win.ui(), win.ui().root(), "", "note");
        w::checkBox(win.ui(), win.ui().root(), "Pin", false, "pin");
        native.show(win);
        win.setPosition({180.f, 140.f});
        CHECK(native.resizeWindow(win, 460, 280));
        CHECK(pumpUntil(a, [&] { return win.size().w == 460.f && win.size().h == 280.f && win.position().x == 180.f; }));
        win.ui().setFocus(edit);
        win.text("left open");
        win.ui().setChecked(win.ui().find("pin"), true);
        a.step(0.f);
        CHECK(a.saveStateTo(file));
    }
    {
        app::Application b{{.name = "gate_services_state_b", .loadSettings = false}};
        app::NativePlatform native(b, {.vulkan = false, .scale = 1.f});
        app::AppWindow& win = b.createWindow({.title = "state", .width = 320, .height = 200});
        const NodeHandle edit = w::lineEdit(win.ui(), win.ui().root(), "", "note");
        const NodeHandle pin = w::checkBox(win.ui(), win.ui().root(), "Pin", false, "pin");
        CHECK(b.restoreStateFrom(file)); // before show(): the OS window is created with the saved geometry
        native.show(win);
        CHECK(pumpUntil(b, [&] { return win.framesPresented() >= 1; }));
        for (int i = 0; i < 6; ++i) b.step(0.004f);
        CHECK(win.size().w == 460.f && win.size().h == 280.f);
        const Vec2 o = native.screenOrigin(win);
        CHECK(o.x == 180.f && o.y == 140.f);
        Image shot;
        CHECK(native.capture(win, shot) && shot.width == 460 && shot.height == 280);
        CHECK(win.ui().text(edit) == "left open" && win.ui().checked(pin) && win.ui().focused() == edit);
        CHECK(win.mode() == app::WindowMode::Normal);
    }
}

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

    // window-less dialogs (the free functions of native.hpp): no helper -> Unavailable, else the helper's answer
    CHECK(!app::openFileDialog({}) && app::openFileDialog({}).error() == app::PlatformError::Unavailable);
    CHECK(!app::colorDialog({}) && app::colorDialog({}).error() == app::PlatformError::Unavailable);
    CHECK(!app::messageBox("t", "x") && app::messageBox("t", "x").error() == app::PlatformError::Unavailable);
    writeScript(bin + "/zenity", "printf '%s\\n' \"$@\" > \"" + bin + "/zenity.args\"\necho '/a/one two.png|/b/three.png'");
    {
        const app::FileFilter filters[] = {{"Images", "*.png;*.jpg"}};
        const auto many = app::openFileDialog({.title = "Pick", .folder = "/a", .filters = filters, .multiple = true});
        CHECK(many && many->size() == 2 && (*many)[0] == "/a/one two.png" && (*many)[1] == "/b/three.png");
        std::ifstream f(bin + "/zenity.args");
        std::string line, all;
        while (std::getline(f, line)) all += line + "|";
        CHECK(all == "--file-selection|--title=Pick|--multiple|--separator=||--filename=/a/|--file-filter=Images | *.png *.jpg|");
        const auto one = app::openFileDialog({.title = "Pick"});
        CHECK(one && one->size() == 1 && (*one)[0] == "/a/one two.png|/b/three.png"); // single: the output is one path
    }
    writeScript(bin + "/zenity", "echo 'rgb(10,20,30)'");
    {
        const auto c = app::colorDialog({.title = "Colour", .initial = {1.f, 1.f, 1.f, 0.5f}});
        CHECK(c && std::fabs(c->r - 10.f / 255.f) < 1e-6f && std::fabs(c->b - 30.f / 255.f) < 1e-6f && c->a == 0.5f);
    }
    writeScript(bin + "/zenity", "echo 'not a colour'");
    CHECK(!app::colorDialog({}) && app::colorDialog({}).error() == app::PlatformError::Failed);
    writeScript(bin + "/zenity", "exit 1");
    CHECK(!app::saveFileDialog({}) && app::saveFileDialog({}).error() == app::PlatformError::Cancelled);
    CHECK(app::messageBox("Quit", "Sure?", app::MessageIcon::Question, app::MessageButtons::YesNo) == app::MessageResult::No);
    CHECK(app::messageBox("Quit", "Sure?", app::MessageIcon::Question, app::MessageButtons::OkCancel) == app::MessageResult::Cancel);
    writeScript(bin + "/zenity", "exit 0");
    CHECK(app::messageBox("Quit", "Sure?", app::MessageIcon::Question, app::MessageButtons::YesNo) == app::MessageResult::Yes);
    CHECK(app::messageBox("Done", "ok") == app::MessageResult::Ok);
    writeScript(bin + "/zenity", "echo Cancel\nexit 1");
    CHECK(app::messageBox("Quit", "Save?", app::MessageIcon::Warning, app::MessageButtons::YesNoCancel) == app::MessageResult::Cancel);
    ::remove((bin + "/zenity").c_str());
    writeScript(bin + "/kdialog", "echo /home/k/projects");
    {
        const auto dir = app::pickFolderDialog({.title = "Folder"});
        CHECK(dir && *dir == "/home/k/projects");
    }
    writeScript(bin + "/kdialog", "echo '#102030'");
    {
        const auto c = app::colorDialog({});
        CHECK(c && std::fabs(c->r - 16.f / 255.f) < 1e-6f && std::fabs(c->g - 32.f / 255.f) < 1e-6f && c->a == 1.f);
    }
    ::remove((bin + "/kdialog").c_str());
    {
        const auto missing = app::revealInFileManager(bin + "/no such file");
        CHECK(!missing && missing.error() == app::PlatformError::Failed);
        const auto noManager = app::revealInFileManager(bin);
        CHECK(!noManager && noManager.error() == app::PlatformError::Unavailable); // no xdg-open in PATH yet
    }

    // openUrl / revealFile: xdg-open with the URL / the containing folder; false when there is no xdg-open
    CHECK(!app::openUrl("https://example.org/"));
    const std::string log = bin + "/xdg.log";
    ::remove(log.c_str());
    writeScript(bin + "/xdg-open", "echo \"$1\" >> \"" + log + "\"");
    ::setenv("PATH", (bin + ":/usr/bin:/bin").c_str(), 1);
    CHECK(app::openUrl("https://example.org/a?b=c"));
    CHECK(app::revealFile("/tmp/some dir/picture.png"));
    CHECK(app::revealInFileManager(bin).has_value());
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
    clipboardFormats();
    xdnd();
    toolWindows(out);
    chromeAndClose();
    savedStateRestore(out);
    xsettings();
#endif
    std::printf("fuse_app_native_services: %s\n", g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}
