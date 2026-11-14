// fuse_app platform-layer gates that need no display (the OS side is in test_app_native_services.cpp, under Xvfb):
//   saved_state      UI state + window geometry through Application::saveState / restoreState and a state file
//   window_chrome    WindowFrame / titleBar / chromeHitTest regions and the title-bar commands on a headless window
//   dialog_helpers   zenity / kdialog command lines of the window-less dialogs, their output parsers, uri lists
//   clipboard_png    the image/png clipboard codec (straight alpha round trip, rejects garbage)
//   frame_pacing     AppWindow::setMaxFrameRate on a virtual clock (a Continuous window sleeps between frames)
//   tool_surfaces    UiContext::setOsWindows: "os-window" nodes / floating docks leave drawList() for windowSurfaces();
//                    UiContext::dragFinishExternal ends a UI drag with the outside result
// Usage: fuse_app_platform --gate=<name> [--out=<dir>]
#include "fuse/app/app.hpp"
#include "fuse/app/chrome.hpp"
#include "fuse/app/platform.hpp" // the compatibility header must compile next to services.hpp / native.hpp
#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace fuse::ui;
namespace app = fuse::app;
namespace w = fuse::ui::widgets;

namespace {
bool has(const std::vector<std::string>& v, std::string_view s) {
    for (const std::string& x : v)
        if (x == s) return true;
    return false;
}

// ---- saved_state -----------------------------------------------------------------------------------------------------
struct Form {
    NodeHandle edit, check, slider, list, tabs;
};
Form buildForm(UiContext& ui) {
    Form f;
    f.edit = w::lineEdit(ui, ui.root(), "", "name");
    f.check = w::checkBox(ui, ui.root(), "Remember", false, "remember");
    f.slider = w::slider(ui, ui.root(), 0.f, 100.f, 10.f, 0.f, "volume");
    f.list = w::scrollArea(ui, ui.root(), "list");
    ui.setCss(f.list, "height", "80px");
    for (int i = 0; i < 40; ++i) w::label(ui, f.list, "row " + std::to_string(i));
    return f;
}
struct Extra {
    int saved = 0, restored = 0;
    std::string seen;
};
void saveExtra(void* u, app::Application&, app::Settings& out) {
    ++static_cast<Extra*>(u)->saved;
    out.set("doc.path", "/tmp/a file=1.txt");
}
void restoreExtra(void* u, app::Application&, const app::Settings& in) {
    ++static_cast<Extra*>(u)->restored;
    static_cast<Extra*>(u)->seen = in.get("doc.path");
}
void savedState(const std::string& outDir) {
    std::string bytes;
    {
        app::Application a{{.name = "gate_saved_a", .loadSettings = false}};
        app::AppWindow& win = a.createWindow({.title = "state", .width = 400, .height = 300});
        const Form f = buildForm(win.ui());
        a.step(0.f);
        win.ui().setFocus(f.edit);
        win.text("Ada L\xC3\xB6velace");
        win.ui().setChecked(f.check, true);
        win.ui().setValue(f.slider, 64.f);
        win.ui().scrollTo(f.list, {0.f, 120.f});
        win.resize(520, 340);
        win.setPosition({70.f, 45.f});
        a.step(0.f);
        UI_CHECK(win.ui().scroll(f.list).y == 120.f);
        Extra extra;
        a.setStateHooks({&extra, &saveExtra, &restoreExtra});
        bytes = a.saveState();
        UI_CHECK(extra.saved == 1);
        UI_CHECK(bytes.find("w0.text.name=Ada L\xC3\xB6velace") != std::string::npos);
        UI_CHECK(bytes.find("w0.checked.remember=1") != std::string::npos);
        UI_CHECK(bytes.find("w0.focus=name") != std::string::npos);
        UI_CHECK(bytes.find("w0.window.w=520") != std::string::npos);
        // a maximised window keeps the normal geometry it saved before
        app::Settings s;
        win.saveWindowState(s);
        win.maximize();
        win.saveWindowState(s);
        UI_CHECK(s.get("window.mode") == "maximized");
        UI_CHECK(s.number("window.w") == 520.0 && s.number("window.x") == 70.0);
        if (!outDir.empty()) {
            win.setMode(app::WindowMode::Normal);
            UI_CHECK(a.saveStateTo(outDir + "/gate_saved_state.ini"));
        }
    }
    {
        app::Application b{{.name = "gate_saved_b", .loadSettings = false}};
        app::AppWindow& win = b.createWindow({.title = "state", .width = 400, .height = 300});
        const Form f = buildForm(win.ui());
        int textChanged = 0, toggled = 0;
        win.ui().connect(f.edit, SignalKind::TextChanged, [&textChanged](UiContext&, NodeHandle, const SignalArgs&) { ++textChanged; });
        win.ui().connect(f.check, SignalKind::Toggled, [&toggled](UiContext&, NodeHandle, const SignalArgs&) { ++toggled; });
        Extra extra;
        b.setStateHooks({&extra, &saveExtra, &restoreExtra});
        UI_CHECK(!b.restored());
        b.restoreState(bytes);
        UI_CHECK(b.restored() && extra.restored == 1);
        UI_CHECK(extra.seen == "/tmp/a file=1.txt"); // '=' in a value survives the text form
        b.step(0.f);
        b.step(0.f);
        UiContext& ui = win.ui();
        UI_CHECK(ui.text(f.edit) == "Ada L\xC3\xB6velace");
        UI_CHECK(textChanged == 1 && toggled == 1);
        UI_CHECK(ui.checked(f.check));
        UI_CHECK(ui.value(f.slider) == 64.f);
        UI_CHECK(ui.focused() == f.edit);
        UI_CHECK(ui.scroll(f.list).y == 120.f); // applied after the first layout
        UI_CHECK(win.size().w == 520.f && win.size().h == 340.f);
        UI_CHECK(win.position().x == 70.f && win.position().y == 45.f);
        UI_CHECK(win.mode() == app::WindowMode::Normal);
        // unknown ids and a state without a size are ignored
        app::Settings junk;
        junk.set("text.nosuchnode", "x");
        win.restoreUiState(junk);
        UI_CHECK(!win.restoreWindowState(junk));
    }
    if (!outDir.empty()) { // the file form (desktop session restore)
        app::Application c{{.name = "gate_saved_c", .loadSettings = false}};
        app::AppWindow& win = c.createWindow({.title = "state", .width = 400, .height = 300});
        const Form f = buildForm(win.ui());
        UI_CHECK(!c.restoreStateFrom(outDir + "/no_such_state_file.ini"));
        UI_CHECK(c.restoreStateFrom(outDir + "/gate_saved_state.ini"));
        c.step(0.f);
        UI_CHECK(win.ui().text(f.edit) == "Ada L\xC3\xB6velace");
        UI_CHECK(win.size().w == 520.f);
    }
    // Settings text form round trip
    app::Settings s;
    s.set("a", "line1\nline2");
    s.set("b\\c", "x=y");
    app::Settings t;
    t.fromText(s.toText());
    UI_CHECK(t.size() == 2 && t.get("a") == "line1\nline2" && t.get("b\\c") == "x=y" && !t.dirty());
}

// ---- window_chrome ---------------------------------------------------------------------------------------------------
void windowChrome() {
    using app::HitArea;
    app::Application a{{.name = "gate_chrome", .loadSettings = false}};
    app::AppWindow& win = a.createWindow({.title = "Tool", .width = 400, .height = 300, .frame = app::WindowFrame::Custom, .resizeBorder = 6.f});
    UiContext& ui = win.ui();
    const NodeHandle bar = app::titleBar(win, ui.root(), "Tool", 32.f);
    const NodeHandle body = w::button(ui, ui.root(), "Body", "body");
    a.step(0.f);
    UI_CHECK(win.chrome().frame == app::WindowFrame::Custom && win.chrome().resizeBorder == 6.f);
    UI_CHECK(win.chrome().caption == bar && ui.find("titlebar") == bar);
    UI_CHECK(ui.rect(bar).h == 32.f && ui.rect(bar).w == 400.f);
    const Rect minR = ui.rect(ui.find("titlebar.min")), maxR = ui.rect(ui.find("titlebar.max")), closeR = ui.rect(ui.find("titlebar.close"));
    UI_CHECK(closeR.right() == 400.f && maxR.right() == closeR.x && minR.right() == maxR.x); // right-aligned, in order
    auto centre = [](Rect r) { return Vec2{r.x + r.w * 0.5f, r.y + r.h * 0.5f}; };
    // regions
    UI_CHECK(app::chromeHitTest(win, {150.f, 16.f}) == HitArea::Caption);
    UI_CHECK(app::chromeHitTest(win, {20.f, 16.f}) == HitArea::Caption); // the title label is part of the drag area
    UI_CHECK(app::chromeHitTest(win, centre(minR)) == HitArea::Minimize);
    UI_CHECK(app::chromeHitTest(win, centre(maxR)) == HitArea::Maximize);
    UI_CHECK(app::chromeHitTest(win, {closeR.x + 10.f, 20.f}) == HitArea::Close);
    UI_CHECK(app::chromeHitTest(win, centre(ui.rect(body))) == HitArea::Client);
    UI_CHECK(app::chromeHitTest(win, {200.f, 200.f}) == HitArea::Client);
    UI_CHECK(app::chromeHitTest(win, {2.f, 150.f}) == HitArea::Left);
    UI_CHECK(app::chromeHitTest(win, {397.f, 150.f}) == HitArea::Right);
    UI_CHECK(app::chromeHitTest(win, {200.f, 2.f}) == HitArea::Top);
    UI_CHECK(app::chromeHitTest(win, {200.f, 298.f}) == HitArea::Bottom);
    UI_CHECK(app::chromeHitTest(win, {1.f, 1.f}) == HitArea::TopLeft);
    UI_CHECK(app::chromeHitTest(win, {10.f, 2.f}) == HitArea::TopLeft); // corners reach 2 x border along each edge
    UI_CHECK(app::chromeHitTest(win, {398.f, 3.f}) == HitArea::TopRight);
    UI_CHECK(app::chromeHitTest(win, {3.f, 297.f}) == HitArea::BottomLeft);
    UI_CHECK(app::chromeHitTest(win, {396.f, 296.f}) == HitArea::BottomRight);
    UI_CHECK(app::hitAreaName(HitArea::BottomRight) == "bottom-right" && app::hitAreaName(HitArea::Caption) == "caption");
    // the buttons run the window commands (headless: the window is its own window system)
    win.click(centre(maxR));
    UI_CHECK(win.mode() == app::WindowMode::Maximized);
    UI_CHECK(app::chromeHitTest(win, {2.f, 150.f}) == HitArea::Client);  // no resize edges while maximised
    UI_CHECK(app::chromeHitTest(win, {200.f, 2.f}) == HitArea::Caption); // the top edge is caption then
    win.click(centre(maxR));
    UI_CHECK(win.mode() == app::WindowMode::Normal);
    win.click(centre(minR));
    UI_CHECK(win.mode() == app::WindowMode::Minimized);
    win.restore();
    // close goes through the close guard
    static bool allow = false;
    win.setCloseGuard({nullptr, [](void*, app::AppWindow&) { return allow; }});
    win.click(centre(closeR));
    UI_CHECK(!win.closing());
    allow = true;
    win.click(centre(closeR));
    UI_CHECK(win.closing());

    // Borderless: a caption, no resize edges; System: everything is client
    app::AppWindow& splash = a.createWindow({.title = "Splash", .width = 300, .height = 200, .frame = app::WindowFrame::Borderless});
    app::titleBar(splash, splash.ui().root(), "Splash");
    a.step(0.f);
    UI_CHECK(splash.chrome().resizeBorder == 0.f);
    UI_CHECK(app::chromeHitTest(splash, {1.f, 100.f}) == HitArea::Client);
    UI_CHECK(app::chromeHitTest(splash, {100.f, 10.f}) == HitArea::Caption);
    app::AppWindow& plain = a.createWindow({.title = "Plain", .width = 300, .height = 200});
    app::titleBar(plain, plain.ui().root(), "Plain");
    a.step(0.f);
    UI_CHECK(app::chromeHitTest(plain, {100.f, 10.f}) == HitArea::Client);
    UI_CHECK(app::chromeHitTest(plain, {1.f, 1.f}) == HitArea::Client);
}

// ---- dialog_helpers --------------------------------------------------------------------------------------------------
void dialogHelpers() {
    const app::FileFilter filters[] = {{"Images", "*.png;*.jpg"}, {"", "*.txt"}};
    const app::FileDialogDesc d{.title = "Open scene", .folder = "/home/u/proj", .fileName = "new.scene", .filters = filters, .multiple = true};
    auto z = app::zenityFileArgs(d, app::DialogKind::Open);
    UI_CHECK(z[0] == "zenity" && z[1] == "--file-selection");
    UI_CHECK(has(z, "--title=Open scene") && has(z, "--multiple") && has(z, "--separator=|"));
    UI_CHECK(has(z, "--filename=/home/u/proj/")); // open: the folder only
    UI_CHECK(has(z, "--file-filter=Images | *.png *.jpg") && has(z, "--file-filter=*.txt | *.txt"));
    UI_CHECK(!has(z, "--save") && !has(z, "--directory"));
    z = app::zenityFileArgs(d, app::DialogKind::Save);
    UI_CHECK(has(z, "--save") && has(z, "--confirm-overwrite") && has(z, "--filename=/home/u/proj/new.scene") && !has(z, "--multiple"));
    z = app::zenityFileArgs(d, app::DialogKind::Folder);
    UI_CHECK(has(z, "--directory") && !has(z, "--file-filter=Images | *.png *.jpg"));
    auto k = app::kdialogFileArgs(d, app::DialogKind::Open);
    UI_CHECK(k[0] == "kdialog" && has(k, "--getopenfilename") && has(k, "--multiple") && has(k, "--separate-output"));
    UI_CHECK(has(k, "/home/u/proj/") && has(k, "Images (*.png *.jpg)\n*.txt"));
    k = app::kdialogFileArgs({}, app::DialogKind::Folder);
    UI_CHECK(has(k, "--getexistingdirectory") && k.back() == ".");
    // colour
    const app::ColorDialogDesc cd{.title = "Pick", .initial = {1.f, 0.5f, 0.f, 0.25f}};
    UI_CHECK(has(app::zenityColorArgs(cd), "--color-selection") && has(app::zenityColorArgs(cd), "--color=#ff8000"));
    UI_CHECK(has(app::kdialogColorArgs(cd), "--getcolor") && app::kdialogColorArgs(cd).back() == "#ff8000");
    Color c;
    UI_CHECK(app::parseDialogColor("rgb(255,128,0)\n", 0.25f, c) && c.r == 1.f && std::fabs(c.g - 128.f / 255.f) < 1e-6f && c.b == 0.f && c.a == 0.25f);
    UI_CHECK(app::parseDialogColor("rgba(0, 0, 255, 0.5)", 1.f, c) && c.b == 1.f && c.a == 0.5f);
    UI_CHECK(app::parseDialogColor("#0080ff", 1.f, c) && c.r == 0.f && std::fabs(c.g - 128.f / 255.f) < 1e-6f && c.b == 1.f && c.a == 1.f);
    UI_CHECK(app::parseDialogColor("#ffff00008080", 0.5f, c) && c.r == 1.f && c.g == 0.f && std::fabs(c.b - 128.f / 255.f) < 1e-6f && c.a == 0.5f);
    UI_CHECK(!app::parseDialogColor("", 1.f, c) && !app::parseDialogColor("red", 1.f, c) && !app::parseDialogColor("#12345", 1.f, c) &&
             !app::parseDialogColor("rgb(1,2)", 1.f, c) && !app::parseDialogColor("#gg0000", 1.f, c));
    // message box
    auto mArgs = app::zenityMessageArgs("Quit", "Save changes?", app::MessageIcon::Question, app::MessageButtons::YesNoCancel);
    UI_CHECK(has(mArgs, "--question") && has(mArgs, "--title=Quit") && has(mArgs, "--text=Save changes?") && has(mArgs, "--extra-button=Cancel"));
    UI_CHECK(has(app::zenityMessageArgs("E", "t", app::MessageIcon::Error, app::MessageButtons::Ok), "--error"));
    UI_CHECK(has(app::zenityMessageArgs("W", "t", app::MessageIcon::Warning, app::MessageButtons::Ok), "--warning"));
    UI_CHECK(has(app::zenityMessageArgs("I", "t", app::MessageIcon::Info, app::MessageButtons::Ok), "--info"));
    UI_CHECK(has(app::zenityMessageArgs("O", "t", app::MessageIcon::Info, app::MessageButtons::OkCancel), "--ok-label=OK"));
    // multi-file output, URL schemes, error names
    const auto paths = app::splitDialogPaths("/a/b c.png|/d/e.png\n/f.png\r\n");
    UI_CHECK(paths.size() == 3 && paths[0] == "/a/b c.png" && paths[1] == "/d/e.png" && paths[2] == "/f.png");
    UI_CHECK(app::splitDialogPaths("").empty());
    UI_CHECK(app::isOpenableUrl("https://example.org/x") && app::isOpenableUrl("http://a") && app::isOpenableUrl("mailto:a@b.c") &&
             app::isOpenableUrl("file:///tmp/x"));
    UI_CHECK(!app::isOpenableUrl("javascript:alert(1)") && !app::isOpenableUrl("/tmp/x") && !app::isOpenableUrl("https://") &&
             !app::isOpenableUrl(""));
    UI_CHECK(std::string(app::platformErrorName(app::PlatformError::Cancelled)) == "cancelled" &&
             std::string(app::platformErrorName(app::PlatformError::Unavailable)) == "unavailable");
    // XSETTINGS property: build (what a settings manager writes) <-> parse, both byte orders, truncated input
    {
        const std::vector<app::XSetting> in{{"Xft/DPI", 0, 144 * 1024, {}}, {"Net/ThemeName", 1, 0, "Adwaita-dark"}, {"Net/Background", 2, 0, {}},
                                            {"Gtk/CursorThemeSize", 0, 48, {}}, {"Odd", 1, 0, "abcde"}};
        std::vector<u8> bytes = app::buildXSettings(in, 7);
        UI_CHECK(bytes.size() % 4 == 0 && bytes[0] == 0);
        std::vector<app::XSetting> out = app::parseXSettings(bytes);
        UI_CHECK(out.size() == 5);
        if (out.size() == 5) {
            UI_CHECK(out[0].name == "Xft/DPI" && out[0].type == 0 && out[0].integer == 144 * 1024);
            UI_CHECK(out[1].name == "Net/ThemeName" && out[1].type == 1 && out[1].string == "Adwaita-dark");
            UI_CHECK(out[2].type == 2 && out[3].name == "Gtk/CursorThemeSize" && out[3].integer == 48);
            UI_CHECK(out[4].name == "Odd" && out[4].string == "abcde"); // padding of odd lengths
        }
        // MSB first: the header and one integer setting written by hand
        const u8 msb[] = {1, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 1, /*type*/ 0, 0, /*name len*/ 0, 3, 'D', 'P', 'I', 0, /*serial*/ 0, 0, 0, 9,
                          /*value*/ 0, 1, 0x80, 0};
        out = app::parseXSettings(msb);
        UI_CHECK(out.size() == 1 && out[0].name == "DPI" && out[0].integer == 0x18000);
        // a cut-off property yields the complete entries only, never reads past the end
        for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
            const auto part = app::parseXSettings({bytes.data(), cut});
            UI_CHECK(part.size() <= 5);
            for (std::size_t i = 0; i < part.size() && i < in.size(); ++i) UI_CHECK(part[i].name == in[i].name);
        }
        UI_CHECK(app::parseXSettings({}).empty());
    }
    // uri lists (what an OS file drop carries) <-> paths
    UI_CHECK(app::pathsFromUriList("file:///home/u/a%20b.txt\r\n# comment\r\nhttp://x/y\r\nfile://localhost/tmp/c%C3%A9.png\r\n") ==
             "/home/u/a b.txt\n/tmp/c\xC3\xA9.png");
    UI_CHECK(app::pathsFromUriList("file:/plain/path") == "/plain/path");
    UI_CHECK(app::pathsFromUriList("").empty() && app::pathsFromUriList("text only").empty());
    const std::string uris = app::uriListFromPaths("/home/u/a b.txt\n/tmp/c\xC3\xA9#1.png");
    UI_CHECK(uris == "file:///home/u/a%20b.txt\r\nfile:///tmp/c%C3%A9%231.png\r\n");
    UI_CHECK(app::pathsFromUriList(uris) == "/home/u/a b.txt\n/tmp/c\xC3\xA9#1.png");
}

// ---- clipboard_png ---------------------------------------------------------------------------------------------------
void clipboardPng() {
    Image img;
    img.resize(37, 21);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x)
            img.pixels[std::size_t(y) * 37 + std::size_t(x)] = (u32(x * 7) & 0xFFu) | ((u32(y * 11) & 0xFFu) << 8) | ((u32(x ^ y) & 0xFFu) << 16) |
                                                               ((u32(255 - x * 3) & 0xFFu) << 24); // varying, non-premultiplied alpha
    std::vector<u8> png;
    UI_CHECK(app::encodePng(img, png));
    UI_CHECK(png.size() > 8 && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
    Image back;
    UI_CHECK(app::decodePng(png, back));
    UI_CHECK(back.width == 37 && back.height == 21 && back.pixels == img.pixels); // lossless, straight alpha kept
    Image none;
    UI_CHECK(!app::encodePng(none, png) && png.empty());
    const u8 junk[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    UI_CHECK(!app::decodePng(junk, back));
    UI_CHECK(!app::decodePng({}, back));
    UI_CHECK(app::kClipboardImage == "image/png" && app::kClipboardText == "text/plain;charset=utf-8");
}

// ---- frame_pacing ----------------------------------------------------------------------------------------------------
struct VirtualTime {
    double now = 100.0;
    double slept = 0.0;
    int pumps = 0;
};
double vtNow(void* u) { return static_cast<VirtualTime*>(u)->now; }
bool vtPump(void* u, app::Application&, f32 wait) {
    auto& t = *static_cast<VirtualTime*>(u);
    ++t.pumps;
    if (wait > 0.f && std::isfinite(wait)) {
        t.now += double(wait);
        t.slept += double(wait);
    }
    return true;
}
void framePacing() {
    VirtualTime t;
    app::Application a{{.name = "gate_pacing", .loadSettings = false}};
    a.setClock({&t, &vtNow});
    a.setEventSource({&t, &vtPump});
    app::AppWindow& win = a.createWindow({.title = "paced", .width = 200, .height = 100});
    w::label(win.ui(), win.ui().root(), "x");
    win.setRedrawPolicy(app::RedrawPolicy::Continuous);
    UI_CHECK(win.maxFrameRate() == 0.f);
    // unlimited (the default): every loop iteration presents, the loop never sleeps
    for (int i = 0; i < 50; ++i) a.step();
    UI_CHECK(win.framesPresented() == 50);
    UI_CHECK(t.slept == 0.0);
    // 60 Hz: a virtual second presents 60 frames (+- the slot it starts in) and the loop sleeps between them
    win.setMaxFrameRate(60.f);
    const u64 f0 = win.framesPresented();
    const double start = t.now;
    int iterations = 0;
    while (t.now - start < 1.0 && iterations < 100000) {
        a.step();
        ++iterations;
    }
    const u64 frames = win.framesPresented() - f0;
    const double expected = (t.now - start) * 60.0; // the last step may run past the second
    if (std::fabs(double(frames) - expected) > 1.5)
        std::fprintf(stderr, "frames %llu in %f s, %d iterations\n", static_cast<unsigned long long>(frames), t.now - start, iterations);
    UI_CHECK(std::fabs(double(frames) - expected) <= 1.5);
    UI_CHECK(t.slept > 0.9);         // it slept nearly the whole second
    UI_CHECK(iterations <= 2 * 61);  // and did not spin
    // a vsynced presenter is paced by the display, not by the rate
    struct Count {
        int n = 0;
    } count;
    app::Presenter p{&count, [](void* u, app::AppWindow& wnd, Rect dmg) { wnd.rasterize(dmg); ++static_cast<Count*>(u)->n; return true; }, nullptr, true};
    win.setPresenter(p);
    for (int i = 0; i < 30; ++i) a.step();
    UI_CHECK(count.n == 30);
    win.setPresenter({});
    // on demand: an idle paced window presents nothing and sleeps until asked
    win.setRedrawPolicy(app::RedrawPolicy::OnDemand);
    win.setMaxFrameRate(30.f);
    for (int i = 0; i < 5; ++i) a.step(0.5f);
    const u64 idle = win.framesPresented();
    for (int i = 0; i < 20; ++i) a.step(0.5f);
    UI_CHECK(win.framesPresented() == idle);
    // 10 redraw requests inside one frame slot coalesce into one presented frame
    for (int i = 0; i < 10; ++i) {
        win.requestRedraw();
        a.step(0.001f);
    }
    UI_CHECK(win.framesPresented() - idle <= 2);
    for (int i = 0; i < 4; ++i) a.step(0.1f);
    UI_CHECK(win.framesPresented() - idle >= 1 && win.framesPresented() - idle <= 2);
    win.setMaxFrameRate(-5.f);
    UI_CHECK(win.maxFrameRate() == 0.f);
}

// ---- tool_surfaces ---------------------------------------------------------------------------------------------------
bool dragSource(UiContext&, NodeHandle, Vec2, DragData& out, DragImage&) {
    out.type = kDragText;
    out.text = "payload";
    return true;
}
void toolSurfaces() {
    const fuse::text::Font& font = fuse::app::defaultFont();
    UiContext ui({.viewport = {600.f, 400.f}, .font = &font});
    w::install(ui);
    const NodeHandle area = w::dockArea(ui, ui.root(), "area");
    ui.setCss(area, "flex-grow", "1");
    const NodeHandle dock = w::dockWidget(ui, area, "Inspector", w::DockSide::Left, "insp");
    w::label(ui, w::dockBody(ui, dock), "content");
    ui.update(0.f);
    UI_CHECK(!ui.osWindows() && ui.windowSurfaces().empty());
    // off (the default): a floating dock is an in-window panel, clamped to the viewport
    w::dockFloat(ui, dock, {900.f, 50.f, 220.f, 160.f});
    ui.update(0.f);
    UI_CHECK(w::dockFloating(ui, dock) && ui.hasClass(dock, "os-window"));
    UI_CHECK(ui.windowSurfaces().empty());
    UI_CHECK(w::dockFloatRect(ui, dock).x <= 600.f - 40.f); // title strip kept reachable
    const std::size_t inWindow = ui.drawList().size();
    // on: the panel leaves the main draw list for its own surface and may leave the window
    ui.setOsWindows(true);
    w::dockFloat(ui, dock, {900.f, -30.f, 220.f, 160.f});
    ui.update(0.f);
    UI_CHECK(ui.windowSurfaces().size() == 1);
    if (!ui.windowSurfaces().empty()) {
        const auto& ws = ui.windowSurfaces()[0];
        UI_CHECK(ws.node == dock);
        UI_CHECK_RECT(ws.rect, 900, -30, 220, 160);
        UI_CHECK(ws.draw.size() > 0);
        UI_CHECK(ui.drawList().size() < inWindow);
        Rect b; // surface-local: every command sits inside the panel's own rect
        for (const DrawCmd& c : ws.draw.cmds())
            if (c.op == DrawOp::Rect || c.op == DrawOp::Glyph) b = b.unite(c.rect);
        UI_CHECK(b.x >= -1.f && b.y >= -1.f && b.right() <= 221.f && b.bottom() <= 161.f);
        // unchanged frames keep the revision (the host does not re-rasterise the tool window)
        const u64 rev = ws.revision;
        ui.markAllDirty();
        ui.update(0.f);
        UI_CHECK(ui.windowSurfaces().size() == 1 && ui.windowSurfaces()[0].revision == rev);
        // input in context coordinates reaches the panel although it lies outside the viewport
        UI_CHECK(ui.hitTest({1000.f, 60.f}).valid());
        bool inDock = false;
        for (NodeHandle n = ui.hitTest({1000.f, 60.f}); ui.alive(n); n = ui.parent(n)) inDock = inDock || n == dock;
        UI_CHECK(inDock);
        // a move bumps the revision
        w::dockFloat(ui, dock, {700.f, 20.f, 220.f, 160.f});
        ui.update(0.f);
        UI_CHECK(ui.windowSurfaces().size() == 1 && ui.windowSurfaces()[0].revision != rev);
    }
    // docking again takes the window away; a plain overlay node with the class works too
    w::dockTo(ui, dock, w::DockSide::Right);
    ui.update(0.f);
    UI_CHECK(ui.windowSurfaces().empty() && !ui.hasClass(dock, "os-window"));
    const NodeHandle note = w::panel(ui, w::overlayLayer(ui), "note");
    ui.addClass(note, "os-window");
    ui.setCss(note, "position", "absolute");
    ui.setCss(note, "left", "640px");
    ui.setCss(note, "top", "10px");
    ui.setCss(note, "width", "100px");
    ui.setCss(note, "height", "50px");
    ui.setCss(note, "background", "#336699");
    ui.update(0.f);
    UI_CHECK(ui.windowSurfaces().size() == 1 && ui.windowSurfaces()[0].node == note);
    ui.setOsWindows(false);
    ui.update(0.f);
    UI_CHECK(ui.windowSurfaces().empty());

    // dragFinishExternal: a UI drag dropped outside ends with the outside result
    const NodeHandle src = w::button(ui, ui.root(), "drag me", "src");
    ui.setDragSource(src, &dragSource);
    struct Seen {
        int ends = 0;
        DropAction action = DropAction::None;
    } seen;
    ui.connect(src, SignalKind::Event, [&seen](UiContext&, NodeHandle, const SignalArgs& a) {
        if (a.event && a.event->type == EventType::DragEnd) {
            ++seen.ends;
            seen.action = a.event->dropAction;
        }
    });
    ui.update(0.f);
    DragData d;
    d.type = kDragText;
    d.text = "payload";
    UI_CHECK(ui.beginDrag(src, d));
    UI_CHECK(ui.dragging());
    ui.pointerMove({-50.f, -50.f}); // left the window
    ui.dragFinishExternal(DropAction::Copy);
    UI_CHECK(!ui.dragging() && seen.ends == 1 && seen.action == DropAction::Copy);
    UI_CHECK(ui.lastDropAction() == DropAction::Copy && !ui.lastDropTarget().valid());
    ui.dragFinishExternal(DropAction::Move); // no drag: nothing happens
    UI_CHECK(seen.ends == 1);
    // an external (OS) drag hovering is not ended by it
    ui.dragExternalMove({10.f, 10.f}, d);
    ui.dragFinishExternal(DropAction::Copy);
    UI_CHECK(ui.dragging());
    ui.dragExternalLeave();
    UI_CHECK(!ui.dragging());
}
} // namespace

int main(int argc, char** argv) {
    app::CommandLine args(argc, argv);
    const std::string g(args.get("gate"));
    const std::string out(args.get("out"));
    if (g == "saved_state") savedState(out);
    else if (g == "window_chrome") windowChrome();
    else if (g == "dialog_helpers") dialogHelpers();
    else if (g == "clipboard_png") clipboardPng();
    else if (g == "frame_pacing") framePacing();
    else if (g == "tool_surfaces") toolSurfaces();
    else {
        std::fprintf(stderr, "unknown gate '%s'\n", g.c_str());
        return 2;
    }
    const int f = fuse::ui::test::failures();
    std::printf("fuse_app_%s: %s\n", g.c_str(), f ? "FAILED" : "OK");
    return f ? 1 : 0;
}
