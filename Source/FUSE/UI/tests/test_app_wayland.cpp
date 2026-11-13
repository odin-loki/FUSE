// fuse_app Wayland backend gate (fuse_app_wayland, Linux / BSD).
// Without a compositor (always run):
//   * libwayland-client resolves (else skip, exit 77), the hand-written protocol tables are consistent (one types entry
//     per argument, opcodes as the backend uses them, core opcodes match libwayland-client's own tables), xkbcommon and
//     the no-xkb fallback map evdev codes to the same UI keys / text
//   * backend selection (NativeOptions::backend, $FUSE_UI_BACKEND, $WAYLAND_DISPLAY), wheel notches, uri-list decoding
//   * a forced Wayland backend without a compositor fails cleanly (unavailable, an error text, show() stays headless)
// Under a headless weston 9+ started by the gate (private XDG_RUNTIME_DIR; $FUSE_TEST_WESTON or `weston` in PATH),
// desktop shell, at output scale 1 and 2:
//   * NativePlatform picks Wayland, an xdg_toplevel with the wl_shm software presenter: the committed buffer equals the
//     CPU raster, at the output's buffer scale
//   * pointer click and key text through the backend's input path (weston's headless backend has no seat, so they
//     are injected below wl_pointer / wl_keyboard; the real seat events are fuse_app_wayland_seat's part)
//   * a client-side resize, title, size limits, maximise / fullscreen / restore reported back by the compositor
//     (AppWindow::mode + WindowEvent), hide / show (unmap + map), position / activate report "cannot"
//   * the frame-callback throttle: redraws asked faster than the compositor repaints are held back, none is lost
//   * xdg_toplevel.close goes through the close guard; wl_output displays
//   * $FUSE_UI_SCALE=1.5: a fractional scale through wp_viewporter (buffer = 1.5 x logical)
//   * NativeOptions::osPopups: a popup menu is an xdg_popup (left out of the window's draw list, its buffer equals the
//     CPU raster of its draw list, a click on an item outside the main window picks it); a child tool toplevel
//   * the Vulkan presenter on a VK_KHR_wayland_surface when the build has Vulkan and an ICD offers the extension
// Exit 77 (skip) when the library is missing or no compositor can be run.
#include "../App/src/wayland_internal.hpp"
#include "fuse/app/chrome.hpp"
#include "fuse/app/native.hpp"
#include "fuse/ui/widgets/widgets.hpp"
#include "test_util.hpp"

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace fuse::ui;
namespace app = fuse::app;
namespace wl = fuse::app::wl;
namespace wg = fuse::ui::widgets;
using app::NativeBackend;

namespace {
constexpr const char* kDoc = R"({ "fuseui": 1, "root": { "class": "panel", "id": "p", "children": [
    { "class": "label", "id": "title", "text": "Wayland window" },
    { "class": "checkbox", "id": "c", "text": "Option" },
    { "class": "lineedit", "id": "e", "text": "Type" } ] } })";

void presentAtLeast(app::Application& a, app::AppWindow& w, u64 frames) {
    for (int i = 0; i < 600 && w.framesPresented() < frames; ++i) a.step(0.01f);
}
void settle(app::Application& a, int n = 30) {
    for (int i = 0; i < n; ++i) a.step(0.005f);
}
/// Steps until `done()` (at most ~3 s): things the compositor answers asynchronously.
template <class F>
bool waitFor(app::Application& a, F done) {
    for (int i = 0; i < 600 && !done(); ++i) a.step(0.005f);
    return done();
}

struct Weston {
    pid_t pid = -1;
    std::string dir, socket = "fuse-wl-gate";
    bool start(const std::string& exe, int scale) {
        const char* tmp = std::getenv("TMPDIR");
        std::string tmpl = std::string(tmp && *tmp ? tmp : "/tmp") + "/fuse-wl-XXXXXX";
        if (!::mkdtemp(tmpl.data())) return false;
        dir = tmpl;
        ::chmod(dir.c_str(), 0700);
        std::fflush(nullptr); // the child must not repeat buffered output
        pid = ::fork();
        if (pid < 0) return false;
        if (pid == 0) {
            ::setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
            ::unsetenv("WAYLAND_DISPLAY");
            ::unsetenv("DISPLAY");
            const std::string sock = "--socket=" + socket, sc = "--scale=" + std::to_string(scale), log = "--log=" + dir + "/weston.log";
            if (!std::freopen("/dev/null", "w", stdout) || !std::freopen("/dev/null", "w", stderr)) ::_exit(126);
            ::execlp(exe.c_str(), exe.c_str(), "--backend=headless", "--shell=desktop", "--renderer=pixman", "--no-config",
                     "--width=1024", "--height=768", sc.c_str(), sock.c_str(), log.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        const std::string path = dir + "/" + socket;
        for (int i = 0; i < 400; ++i) { // up to 8 s for the socket
            struct stat st{};
            if (::stat(path.c_str(), &st) == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300)); // listening, and the shell client is up
                return true;
            }
            int status = 0;
            if (::waitpid(pid, &status, WNOHANG) == pid) {
                pid = -1;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }
    void use() const {
        ::setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
        ::setenv("WAYLAND_DISPLAY", socket.c_str(), 1);
    }
    void stop() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            for (int i = 0; i < 150 && ::waitpid(pid, &status, WNOHANG) != pid; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
        if (!dir.empty()) {
            const std::string cmd = "rm -rf '" + dir + "'";
            [[maybe_unused]] int rc = std::system(cmd.c_str());
            dir.clear();
        }
    }
    ~Weston() { stop(); }
};

std::string westonPath() {
    if (const char* e = std::getenv("FUSE_TEST_WESTON"); e && *e) return e;
    const char* path = std::getenv("PATH");
    std::string p = path ? path : "/usr/bin:/bin";
    for (std::size_t at = 0; at <= p.size();) {
        const std::size_t end = std::min(p.find(':', at), p.size());
        const std::string cand = p.substr(at, end - at) + "/weston";
        if (end > at && ::access(cand.c_str(), X_OK) == 0) return cand;
        at = end + 1;
    }
    return {};
}

Vec2 centre(app::AppWindow& w, std::string_view id) { // device pixels
    const Rect r = w.ui().rect(w.ui().find(id));
    return {(r.x + r.w * 0.5f) * w.scale(), (r.y + r.h * 0.5f) * w.scale()};
}
std::size_t differing(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height) return std::size_t(-1);
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) n += (a.pixels[i] & 0xFFFFFFu) != (b.pixels[i] & 0xFFFFFFu);
    return n;
}

// ---- no compositor ----------------------------------------------------------------------------------------------------
void pureChecks() {
    // selection: the option wins, then the environment, then $WAYLAND_DISPLAY
    UI_CHECK(wl::chooseBackend(NativeBackend::Auto, "", "") == NativeBackend::X11);
    UI_CHECK(wl::chooseBackend(NativeBackend::Auto, "", "wayland-0") == NativeBackend::Wayland);
    UI_CHECK(wl::chooseBackend(NativeBackend::Auto, "x11", "wayland-0") == NativeBackend::X11);
    UI_CHECK(wl::chooseBackend(NativeBackend::Auto, "wayland", "") == NativeBackend::Wayland);
    UI_CHECK(wl::chooseBackend(NativeBackend::Auto, "nonsense", "wayland-0") == NativeBackend::Wayland);
    UI_CHECK(wl::chooseBackend(NativeBackend::X11, "wayland", "wayland-0") == NativeBackend::X11);
    UI_CHECK(wl::chooseBackend(NativeBackend::Wayland, "x11", "") == NativeBackend::Wayland);
    UI_CHECK(!wl::backendForced(NativeBackend::Auto, "") && !wl::backendForced(NativeBackend::Auto, "nonsense"));
    UI_CHECK(wl::backendForced(NativeBackend::Auto, "x11") && wl::backendForced(NativeBackend::Wayland, ""));
    // scrolling: steps win over the smooth value; a wheel is 10 units a notch, fingers 40 px; down is negative y
    Vec2 n = wl::wheelNotches({0.f, 10.f}, {}, false, false, false);
    UI_CHECK(n.x == 0.f && n.y == -1.f);
    n = wl::wheelNotches({0.f, 15.f}, {0.f, 1.f}, false, true, false); // 15 units, but one detent
    UI_CHECK(n.y == -1.f);
    n = wl::wheelNotches({-20.f, 0.f}, {}, false, false, false);
    UI_CHECK(n.x == -2.f && n.y == 0.f);
    n = wl::wheelNotches({8.f, -20.f}, {}, false, false, true); // touchpad: 20 px up, 8 px right
    UI_CHECK(std::fabs(n.x - 0.2f) < 1e-5f && std::fabs(n.y - 0.5f) < 1e-5f);
    UI_CHECK(wl::uriListToPaths("file:///tmp/fuse%20wl/a.png\r\nfile:///tmp/b.txt\r\n") == "/tmp/fuse wl/a.png\n/tmp/b.txt");
    UI_CHECK(wl::uriListToPaths("# comment\r\nhttp://example.org/x\r\nfile://localhost/etc/hosts") == "/etc/hosts");
    UI_CHECK(wl::uriListToPaths("").empty());
}

void noCompositorRun() {
    // a socket name nothing listens on, in a directory that exists
    const std::string keepDir = std::getenv("XDG_RUNTIME_DIR") ? std::getenv("XDG_RUNTIME_DIR") : "";
    const std::string keepWd = std::getenv("WAYLAND_DISPLAY") ? std::getenv("WAYLAND_DISPLAY") : "";
    const char* tmp = std::getenv("TMPDIR");
    ::setenv("XDG_RUNTIME_DIR", tmp && *tmp ? tmp : "/tmp", 1);
    ::setenv("WAYLAND_DISPLAY", "fuse-wl-nobody-listens", 1);
    {
        app::Application a{{.name = "gate_wayland_none", .loadSettings = false}};
        app::NativePlatform native(a, {.vulkan = false, .backend = NativeBackend::Wayland});
        UI_CHECK(!native.available());
        UI_CHECK(native.backend() == NativeBackend::Auto);
        UI_CHECK(!native.lastError().empty());
        std::printf("  forced, no compositor: %.*s\n", int(native.lastError().size()), native.lastError().data());
        auto& w = a.createWindow({.title = "none", .width = 64, .height = 48});
        UI_CHECK(native.show(w) == app::PresenterKind::Headless);
        UI_CHECK(native.nativeWindow(w) == 0 && native.nativeDisplay() == nullptr);
        UI_CHECK(native.displays().empty());
        std::string clip;
        UI_CHECK(!native.clipboardText(clip));
        a.step(0.f); // the default pump still runs the headless window
        UI_CHECK(w.framesPresented() == 1);
    }
    { // not forced: the same environment falls back (X11 when there is a DISPLAY, else unavailable) without a crash
        ::unsetenv("FUSE_UI_BACKEND");
        app::Application a{{.name = "gate_wayland_fallback", .loadSettings = false}};
        app::NativePlatform native(a, {.vulkan = false});
        UI_CHECK(native.backend() != NativeBackend::Wayland);
        std::printf("  auto, no compositor: %s\n", native.backend() == NativeBackend::X11 ? "X11" : "no window system");
    }
    if (keepDir.empty()) ::unsetenv("XDG_RUNTIME_DIR");
    else ::setenv("XDG_RUNTIME_DIR", keepDir.c_str(), 1);
    if (keepWd.empty()) ::unsetenv("WAYLAND_DISPLAY");
    else ::setenv("WAYLAND_DISPLAY", keepWd.c_str(), 1);
}

// ---- under weston -----------------------------------------------------------------------------------------------------
struct Seen {
    int modeChanges = 0, vetoed = 0, hidden = 0, shown = 0, scaleChanges = 0;
    app::WindowMode lastMode = app::WindowMode::Normal;
    bool allowClose = false;
};
void onEvent(void* user, app::AppWindow&, const app::WindowEvent& e) {
    auto& s = *static_cast<Seen*>(user);
    using T = app::WindowEvent::Type;
    if (e.type == T::ModeChanged) { ++s.modeChanges; s.lastMode = e.mode; }
    if (e.type == T::CloseVetoed) ++s.vetoed;
    if (e.type == T::Hidden) ++s.hidden;
    if (e.type == T::Shown) ++s.shown;
    if (e.type == T::ScaleChanged) ++s.scaleChanges;
}

/// One run on the current compositor: software window, input, window management; `scale` the output's scale.
void softwareRun(int scale) {
    app::Application a{{.name = "gate_wayland", .loadSettings = false}};
    // $WAYLAND_DISPLAY is set: Wayland without being asked. weston has no xdg-decoration; the client-side fallback
    // (a title bar in the UI) has its own run below, the others keep their documents as written
    app::NativePlatform native(a, {.vulkan = false, .clientDecorations = false});
    UI_CHECK(native.available());
    UI_CHECK(native.backend() == NativeBackend::Wayland);
    if (native.backend() != NativeBackend::Wayland) {
        std::fprintf(stderr, "  no Wayland backend: %.*s\n", int(native.lastError().size()), native.lastError().data());
        return;
    }
    wl::Backend* b = wl::current();
    UI_CHECK(b != nullptr && native.nativeDisplay() == wl::display(b));
    const auto disp = native.displays();
    UI_CHECK(disp.size() == 1);
    if (!disp.empty()) {
        std::printf("  output '%s' %gx%g scale %g, %g Hz\n", disp[0].name.c_str(), double(disp[0].bounds.w), double(disp[0].bounds.h),
                    double(disp[0].scale), double(disp[0].refreshHz));
        UI_CHECK(disp[0].bounds.w == 1024.f * f32(scale) && disp[0].bounds.h == 768.f * f32(scale)); // the mode, physical pixels
        UI_CHECK(disp[0].scale == f32(scale) && disp[0].primary);
    }
    auto& w = a.createWindow({.title = "fuse_app wayland gate", .width = 320, .height = 200});
    Seen seen;
    w.setEventHandler({&seen, &onEvent});
    w.setCloseGuard({&seen, [](void* u, app::AppWindow&) { return static_cast<Seen*>(u)->allowClose; }});
    UI_CHECK(w.loadText(kDoc).has_value());
    UI_CHECK(native.show(w) == app::PresenterKind::Software);
    UI_CHECK(native.kind(w) == app::PresenterKind::Software && !native.gpuDraws(w));
    wl::Window* win = wl::findWindow(b, w);
    UI_CHECK(win != nullptr);
    if (!win) return;
    UI_CHECK(native.nativeWindow(w) == reinterpret_cast<std::uintptr_t>(wl::surface(win)));
    UI_CHECK(wl::serialOf(win) >= 1); // the first configure was acknowledged
    presentAtLeast(a, w, 1);
    settle(a);
    // HiDPI: the output's scale is the window's, the buffer is logical size x scale (a viewport or the buffer scale)
    UI_CHECK(w.scale() == f32(scale) && native.scale(w) == f32(scale));
    UI_CHECK(wl::hasGlobal(b, "wp_viewporter") ? wl::bufferScale(win) == 1 : wl::bufferScale(win) == scale);
    UI_CHECK(w.size().w == 320.f && w.size().h == 200.f); // the desktop shell leaves the size to the client
    Image shown;
    UI_CHECK(native.capture(w, shown));
    UI_CHECK(shown.width == 320 * scale && shown.height == 200 * scale);
    {
        const Image ref = w.rasterize({});
        const std::size_t d = differing(shown, ref);
        std::printf("  scale %d: committed buffer %dx%d, %zu pixels differ from the CPU raster\n", scale, shown.width, shown.height, d);
        UI_CHECK(d == 0);
    }
    std::printf("  decorations: %s\n", wl::serverDecorated(win) ? "server side (xdg-decoration)"
                                       : wl::hasGlobal(b, "zxdg_decoration_manager_v1") ? "client side asked for by the compositor" : "none (no xdg-decoration)");

    // pointer: a click on the checkbox toggles it
    UiContext& ui = w.ui();
    const NodeHandle cb = ui.find("c");
    const bool before = ui.checked(cb);
    const unsigned long long h = native.nativeWindow(w);
    UI_CHECK(native.postClick(h, centre(w, "c")));
    settle(a);
    UI_CHECK(ui.checked(cb) != before);
    UI_CHECK(!native.postClick(h + 8, {1.f, 1.f})); // not one of ours
    // keys: focus the line edit, type "a" (evdev KEY_A) through xkbcommon (or the US fallback); the I-beam is wanted
    const NodeHandle e = ui.find("e");
    UI_CHECK(native.postClick(h, centre(w, "e")));
    settle(a);
    UI_CHECK(native.cursor(w) == Cursor::IBeam);
    const std::string t0(ui.text(e));
    wl::injectKey(b, win, 30);
    settle(a);
    const std::string t1(ui.text(e));
    std::printf("  line edit '%s' -> '%s'\n", t0.c_str(), t1.c_str());
    UI_CHECK(t1.size() == t0.size() + 1 && t1.find('a') != std::string::npos);
    wl::injectKey(b, win, 14); // Backspace: a key without text
    settle(a);
    UI_CHECK(std::string(ui.text(e)) == t0);

    // the frame throttle: 40 redraws asked back to back - some are held until the compositor shows the last one,
    // and the final state is on screen at the end
    {
        const u64 f0 = w.framesPresented();
        const unsigned long long th0 = wl::framesThrottled(win);
        for (int i = 0; i < 40; ++i) {
            ui.setText(ui.find("title"), "frame " + std::to_string(i));
            a.step(0.f);
        }
        settle(a, 60);
        const u64 frames = w.framesPresented() - f0;
        std::printf("  throttle: 40 changes -> %llu frames presented, %llu held back\n", static_cast<unsigned long long>(frames),
                    wl::framesThrottled(win) - th0);
        UI_CHECK(frames >= 1 && frames < 40);
        UI_CHECK(wl::framesThrottled(win) > th0);
        UI_CHECK(native.capture(w, shown));
        const Image ref = w.rasterize({});
        UI_CHECK(differing(shown, ref) == 0); // "frame 39"
    }

    // client-side resize: the next committed buffer has the new size
    UI_CHECK(native.resizeWindow(w, 400, 260));
    settle(a);
    UI_CHECK(native.capture(w, shown));
    UI_CHECK(shown.width == 400 * scale && shown.height == 260 * scale);
    // title, size limits (the current size is made to obey)
    w.setTitle("renamed");
    w.setMinimumSize({420.f, 0.f});
    settle(a);
    UI_CHECK(w.size().w == 420.f && w.size().h == 260.f);
    w.setMinimumSize({});
    // maximise: the compositor configures the work area and reports the state
    w.maximize();
    UI_CHECK(waitFor(a, [&] { return w.mode() == app::WindowMode::Maximized; }));
    settle(a);
    std::printf("  maximised %gx%g\n", double(w.size().w), double(w.size().h));
    UI_CHECK(w.size().w == 1024.f && w.size().h > 600.f && w.size().h <= 768.f); // minus the shell's panel
    UI_CHECK(seen.lastMode == app::WindowMode::Maximized && wl::mode(win) == app::WindowMode::Maximized);
    UI_CHECK(native.capture(w, shown) && shown.width == 1024 * scale);
    w.setFullscreen(true);
    UI_CHECK(waitFor(a, [&] { return w.mode() == app::WindowMode::Fullscreen; }));
    settle(a);
    UI_CHECK(w.size().w == 1024.f && w.size().h == 768.f);
    w.setMode(app::WindowMode::Normal);
    UI_CHECK(waitFor(a, [&] { return w.mode() == app::WindowMode::Normal; }));
    settle(a);
    UI_CHECK(seen.modeChanges >= 3);
    std::printf("  restored %gx%g\n", double(w.size().w), double(w.size().h));
    UI_CHECK(w.size().w == 420.f && w.size().h == 260.f); // the size from before maximise
    // what Wayland does not let a client do
    const Vec2 pos0 = w.position();
    w.setPosition({50.f, 60.f});
    settle(a, 5);
    UI_CHECK(w.position().x == pos0.x && w.position().y == pos0.y);
    UI_CHECK(native.screenOrigin(w).x == 0.f && native.screenOrigin(w).y == 0.f);
    // hide (unmap) and show (map again): frames stop and resume
    w.setVisible(false);
    settle(a, 10);
    UI_CHECK(!w.visible() && seen.hidden == 1 && !wl::mapped(win));
    const u64 hiddenFrames = w.framesPresented();
    ui.setText(ui.find("title"), "while hidden");
    settle(a, 10);
    UI_CHECK(w.framesPresented() == hiddenFrames);
    w.setVisible(true);
    UI_CHECK(w.visible() && seen.shown == 1 && wl::mapped(win));
    presentAtLeast(a, w, hiddenFrames + 1);
    settle(a);
    UI_CHECK(native.capture(w, shown));
    {
        const Image ref = w.rasterize({});
        UI_CHECK(differing(shown, ref) == 0);
    }
    // the compositor's close (xdg_toplevel.close) asks the close guard
    wl::injectClose(win);
    UI_CHECK(!w.closing() && seen.vetoed == 1);
    seen.allowClose = true;
    wl::injectClose(win);
    UI_CHECK(w.closing());
    a.step(0.f); // the window goes away (its presenter destroys the surface)
    UI_CHECK(a.windowCount() == 0 && wl::findWindow(b, w) == nullptr);
}

/// $FUSE_UI_SCALE=1.5 on a scale-1 output: a fractional scale needs wp_viewporter (weston has it).
void fractionalRun() {
    ::setenv("FUSE_UI_SCALE", "1.5", 1);
    {
        app::Application a{{.name = "gate_wayland_frac", .loadSettings = false}};
        app::NativePlatform native(a, {.vulkan = false, .clientDecorations = false});
        UI_CHECK(native.backend() == NativeBackend::Wayland);
        if (native.backend() == NativeBackend::Wayland) {
            wl::Backend* b = wl::current();
            auto& w = a.createWindow({.title = "fractional", .width = 320, .height = 200});
            UI_CHECK(w.loadText(kDoc).has_value());
            native.show(w);
            presentAtLeast(a, w, 1);
            settle(a);
            wl::Window* win = wl::findWindow(b, w);
            Image shown;
            UI_CHECK(native.capture(w, shown));
            if (wl::hasGlobal(b, "wp_viewporter")) {
                UI_CHECK(w.scale() == 1.5f && wl::bufferScale(win) == 1);
                UI_CHECK(shown.width == 480 && shown.height == 300); // device pixels; the viewport shows it as 320x200
                UI_CHECK(w.size().w == 320.f && w.size().h == 200.f);
                const Image ref = w.rasterize({});
                UI_CHECK(differing(shown, ref) == 0);
                std::printf("  fractional 1.5: buffer %dx%d for a 320x200 window\n", shown.width, shown.height);
            } else { // an integer buffer scale is all there is
                UI_CHECK(w.scale() == 2.f && wl::bufferScale(win) == 2);
            }
            // a click still lands in logical coordinates
            const bool before = w.ui().checked(w.ui().find("c"));
            UI_CHECK(native.postClick(native.nativeWindow(w), centre(w, "c")));
            settle(a);
            UI_CHECK(w.ui().checked(w.ui().find("c")) != before);
        }
    }
    ::unsetenv("FUSE_UI_SCALE");
    { // NativeOptions::scale wins the same way
        app::Application a{{.name = "gate_wayland_opt", .loadSettings = false}};
        app::NativePlatform native(a, {.vulkan = false, .scale = 2.f, .clientDecorations = false});
        auto& w = a.createWindow({.title = "option scale", .width = 100, .height = 80});
        native.show(w);
        presentAtLeast(a, w, 1);
        settle(a);
        Image shown;
        UI_CHECK(w.scale() == 2.f && native.capture(w, shown) && shown.width == 200 && shown.height == 160);
    }
}

/// NativeOptions::osPopups: xdg_popup surfaces, and a child tool toplevel at the protocol layer.
void popupRun() {
    app::Application a{{.name = "gate_wayland_popups", .loadSettings = false}};
    app::NativePlatform native(a, {.vulkan = false, .osPopups = true, .clientDecorations = false});
    UI_CHECK(native.backend() == NativeBackend::Wayland);
    if (native.backend() != NativeBackend::Wayland) return;
    wl::Backend* b = wl::current();
    auto& win = a.createWindow({.title = "fuse_app wayland popups", .width = 320, .height = 160});
    UiContext& ui = win.ui();
    const NodeHandle btn = wg::button(ui, ui.root(), "File", "file");
    native.show(win);
    presentAtLeast(a, win, 1);
    settle(a);
    UI_CHECK(ui.osPopups());
    f32 picked = -1.f;
    std::string pickedText;
    ui.connect(btn, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& s) { picked = s.value; pickedText = s.text; });
    std::string items;
    for (int i = 1; i <= 12; ++i) items += (i > 1 ? "\n" : "") + std::string("Item ") + std::to_string(i);
    const NodeHandle scope = wg::popupMenu(ui, btn, items, {40.f, 60.f});
    win.requestRedraw();
    settle(a, 60);
    UI_CHECK(ui.popupSurfaces().size() == 1);
    UI_CHECK(native.popupCount(win) == 1);
    if (native.popupCount(win) == 1) {
        const Rect menu = ui.rect(ui.firstChild(scope));
        const Rect placed = native.popupScreenRect(win, 0);
        std::printf("  menu rect %.0f,%.0f %.0fx%.0f (window 320x160); xdg_popup placed at %.0f,%.0f\n", double(menu.x), double(menu.y),
                    double(menu.w), double(menu.h), double(placed.x), double(placed.y));
        UI_CHECK(menu.y + menu.h > 160.f); // extends past the main window
        UI_CHECK(placed.x == menu.x && placed.y == menu.y && placed.w == menu.w && placed.h == menu.h); // room on the output: not slid
        // the popup surface shows exactly the CPU raster of the popup draw list, and the window does not draw it
        Image shown, ref;
        UI_CHECK(native.capturePopup(win, 0, shown));
        const auto& ps = ui.popupSurfaces()[0];
        ref.resize(shown.width, shown.height, shown.pixels.empty() ? 0u : shown.pixels[0]);
        {
            u32 clear = 0xFF000000u;
            for (const DrawCmd& c : ps.draw.cmds())
                if (c.op != DrawOp::Shadow && c.fill.a > 0.9f) { clear = packPremultiplied(c.fill) | 0xFF000000u; break; }
            ref.resize(shown.width, shown.height, clear);
            CpuRasterizer rz = win.rasterizer();
            rz.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
            rz.render(ps.draw, ref);
        }
        UI_CHECK(shown.width == int(menu.w) && shown.height == int(menu.h));
        UI_CHECK(differing(shown, ref) == 0);
        // a click on "Item 9" (below the main window's bottom edge) through the popup surface's input path
        const NodeHandle list = ui.firstChild(scope);
        NodeHandle item = ui.firstChild(list);
        for (int i = 1; i < 9 && item.valid(); ++i) item = ui.nextSibling(item);
        const Rect ir = ui.rect(item);
        UI_CHECK(ir.y > 160.f);
        const Vec2 local{ir.x + ir.w * 0.5f - menu.x, ir.y + ir.h * 0.5f - menu.y};
        UI_CHECK(native.postClick(native.popupWindow(win, 0), local));
        win.requestRedraw();
        settle(a, 40);
        std::printf("  picked %g '%s'\n", double(picked), pickedText.c_str());
        UI_CHECK(picked == 8.f && pickedText == "Item 9");
        UI_CHECK(native.popupCount(win) == 0 && ui.popupSurfaces().empty());
    }
    // a tool window: a child xdg_toplevel of the window, its own buffer
    {
        std::string err;
        wl::Window* owner = wl::findWindow(b, win);
        wl::Child* tool = wl::createTool(b, owner, {120.f, 80.f}, {400.f, 0.f}, "Tool", err);
        UI_CHECK(tool != nullptr);
        if (tool) {
            Image img, back;
            img.resize(120, 80, 0xFF2040C0u);
            wl::presentChild(b, tool, img);
            UI_CHECK(wl::roundtrip(b));
            UI_CHECK(wl::captureChild(tool, back) && back.width == 120 && back.height == 80 && differing(back, img) == 0);
            UI_CHECK(!wl::childGrabbed(tool) && !wl::childDismissed(tool));
            // input on it reaches the owner's UI at its origin: (400 + 5, 0 + 5) is outside the main window's widgets
            wl::injectChildPointer(b, tool, {5.f, 5.f}, 0);
            wl::destroyChild(b, tool);
        } else {
            std::fprintf(stderr, "  tool window: %s\n", err.c_str());
        }
    }
}

/// weston's desktop shell has no xdg-decoration: with NativeOptions::clientDecorations (the default) a System-frame
/// window gets the client-side chrome (chrome.hpp) when it is shown.
void decorationRun() {
    app::Application a{{.name = "gate_wayland_csd", .loadSettings = false}};
    app::NativePlatform native(a, {.vulkan = false});
    UI_CHECK(native.backend() == NativeBackend::Wayland);
    if (native.backend() != NativeBackend::Wayland) return;
    wl::Backend* b = wl::current();
    UI_CHECK(!wl::hasGlobal(b, "zxdg_decoration_manager_v1")); // what this run is about
    auto& w = a.createWindow({.title = "undecorated", .width = 320, .height = 200});
    UiContext& ui = w.ui();
    ui.setCss(ui.root(), "flex-direction", "column");
    int clicks = 0;
    const NodeHandle body = fuse::ui::widgets::button(ui, ui.root(), "Body", "body");
    ui.connect(body, SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
    UI_CHECK(native.show(w) == app::PresenterKind::Software);
    wl::Window* win = wl::findWindow(b, w);
    UI_CHECK(win != nullptr && !wl::serverDecorated(win));
    if (!win) return;
    const NodeHandle bar = ui.find("titlebar");
    UI_CHECK(w.chrome().frame == app::WindowFrame::Custom && ui.alive(bar) && ui.firstChild(ui.root()) == bar);
    UI_CHECK(ui.text(ui.find("titlebar.title")) == "undecorated");
    presentAtLeast(a, w, 1);
    settle(a);
    UI_CHECK(ui.rect(bar).y == 0.f && ui.rect(bar).h == 32.f && ui.rect(body).y >= 32.f);
    Image shown;
    UI_CHECK(native.capture(w, shown));
    const Image ref = w.rasterize({});
    UI_CHECK(shown.width == ref.width && shown.height == ref.height);
    std::size_t off = shown.pixels.size() == ref.pixels.size() ? 0 : 1;
    for (std::size_t i = 0; !off && i < ref.pixels.size(); ++i) off += (shown.pixels[i] & 0xFFFFFFu) != (ref.pixels[i] & 0xFFFFFFu);
    UI_CHECK(off == 0); // the bar is in the frame shown
    UI_CHECK(app::chromeHitTest(w, {100.f, 16.f}) == app::HitArea::Caption && app::chromeHitTest(w, {160.f, 198.f}) == app::HitArea::Bottom);
    // a press on the caption / an edge goes to the compositor (xdg_toplevel.move / resize), not to the UI. Injected
    // presses have no serial, so weston does not start a drag here; real ones are in fuse_app_wayland_seat.
    const unsigned long long h = native.nativeWindow(w);
    UI_CHECK(native.postPointerMove(h, {100.f, 16.f}) && native.postPointerButton(h, {100.f, 16.f}, true));
    UI_CHECK(wl::chromeMoves(win) == 1u && !ui.alive(ui.pressed()));
    native.postPointerButton(h, {100.f, 16.f}, false);
    UI_CHECK(native.postPointerMove(h, {160.f, 198.f}) && native.postPointerButton(h, {160.f, 198.f}, true));
    UI_CHECK(wl::chromeResizes(win) == 1u);
    native.postPointerButton(h, {160.f, 198.f}, false);
    settle(a, 5);
    UI_CHECK(native.cursor(w) == Cursor::Arrow || native.cursor(w) == Cursor::ResizeV); // ResizeV once a pointer entered
    // the body is clicked as before; the title bar's buttons work: maximise through the compositor, then close
    const Rect rb = ui.rect(body);
    UI_CHECK(native.postClick(h, {rb.x + rb.w * 0.5f, rb.y + rb.h * 0.5f}));
    settle(a, 5);
    UI_CHECK(clicks == 1);
    const Rect rm = ui.rect(ui.find("titlebar.max"));
    UI_CHECK(native.postClick(h, {rm.x + rm.w * 0.5f, rm.y + rm.h * 0.5f}));
    for (int i = 0; i < 300 && w.mode() != app::WindowMode::Maximized; ++i) a.step(0.004f);
    UI_CHECK(w.mode() == app::WindowMode::Maximized);
    settle(a);
    UI_CHECK(app::chromeHitTest(w, {160.f, w.size().h - 2.f}) == app::HitArea::Client); // no resize edges while maximised
    const Rect rc = ui.rect(ui.find("titlebar.close"));
    UI_CHECK(app::chromeHitTest(w, {rc.x + 4.f, rc.y + 4.f}) == app::HitArea::Close);
    UI_CHECK(native.postClick(h, {rc.x + rc.w * 0.5f, rc.y + rc.h * 0.5f}));
    for (int i = 0; i < 300 && a.windowCount() > 0; ++i) a.step(0.004f);
    UI_CHECK(a.windowCount() == 0);
}

/// The Vulkan presenter on the wl_surface. Returns false when this build / machine has none (not a failure).
bool vulkanRun() {
    app::Application a{{.name = "gate_wayland_vk", .loadSettings = false}};
    app::NativePlatform native(a, {.clientDecorations = false});
    UI_CHECK(native.backend() == NativeBackend::Wayland);
    auto& w = a.createWindow({.title = "fuse_app wayland vulkan", .width = 256, .height = 160});
    UI_CHECK(w.loadText(kDoc).has_value());
    const app::PresenterKind k = native.show(w);
    if (k != app::PresenterKind::Vulkan) {
        std::printf("  no Vulkan presenter on the wl_surface (%.*s): wl_shm instead\n", int(native.lastError().size()), native.lastError().data());
        UI_CHECK(k == app::PresenterKind::Software);
        return false;
    }
    presentAtLeast(a, w, 3);
    settle(a);
    std::printf("  vulkan on %.*s (%s)\n", int(native.gpuName().size()), native.gpuName().data(), native.gpuDraws(w) ? "GPU draw" : "CPU upload");
    Image shown;
    UI_CHECK(native.capture(w, shown));
    UI_CHECK(shown.width == int(w.pixelSize().w) && shown.height == int(w.pixelSize().h));
    const Image ref = w.rasterize({});
    if (shown.width == ref.width && shown.height == ref.height) {
        std::size_t far = 0;
        for (std::size_t i = 0; i < ref.pixels.size(); ++i) {
            int e = 0;
            for (int c = 0; c < 3; ++c) e = std::max(e, std::abs(int((shown.pixels[i] >> (8 * c)) & 0xFF) - int((ref.pixels[i] >> (8 * c)) & 0xFF)));
            far += e > 2;
        }
        std::printf("  vulkan frame: %zu pixels off the CPU raster by more than 2/255\n", far);
        UI_CHECK(far * 200 < ref.pixels.size()); // < 0.5 %
    }
    UI_CHECK(native.resizeWindow(w, 300, 180)); // the swapchain follows the client-side size
    settle(a, 40);
    UI_CHECK(native.capture(w, shown));
    UI_CHECK(shown.width == int(std::lround(300.f * w.scale())) && shown.height == int(std::lround(180.f * w.scale())));
    return true;
}
} // namespace

int main() {
    ::unsetenv("FUSE_UI_BACKEND");
    ::unsetenv("FUSE_UI_SCALE");
    pureChecks();
    std::string err;
    if (!wl::loadLibrary(err)) {
        std::printf("fuse_app_wayland: skipped (%s)%s\n", err.c_str(), fuse::ui::test::failures() ? "; selection checks FAILED" : "");
        return fuse::ui::test::failures() ? 1 : 77;
    }
    const std::string self = wl::selfTest();
    std::printf("fuse_app_wayland: library / protocol table / keymap checks: %s\n", self.empty() ? "pass" : self.c_str());
    UI_CHECK(self.empty());
    UI_CHECK(wl::protocolInterface("xdg_wm_base") != nullptr && wl::protocolInterface("wl_nonsense") == nullptr);
    noCompositorRun();
    if (fuse::ui::test::failures()) return 1;

    const std::string exe = westonPath();
    if (exe.empty()) {
        std::printf("fuse_app_wayland: the compositor part is skipped (no weston in PATH; set FUSE_TEST_WESTON)\n");
        return 77;
    }
    for (int scale : {1, 2}) {
        Weston ws;
        if (!ws.start(exe, scale)) {
            std::printf("fuse_app_wayland: the compositor part is skipped (%s did not start)\n", exe.c_str());
            return fuse::ui::test::failures() ? 1 : 77;
        }
        ws.use();
        std::printf("fuse_app_wayland: headless weston, output scale %d\n", scale);
        softwareRun(scale);
        if (scale == 1) {
            fractionalRun();
            popupRun();
            decorationRun();
            if (!vulkanRun()) std::printf("fuse_app_wayland: the Vulkan presenter did not run here\n");
        }
    }
    std::printf("fuse_app_wayland: %s\n", fuse::ui::test::failures() ? "FAIL" : "pass");
    return fuse::ui::test::failures() ? 1 : 0;
}
