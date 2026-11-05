// fuse_app Wayland parity gates (Linux): what fuse_app_os_popups / the editor's float_window / the clipboard and IME
// paths check on X11, on Wayland.
//
// --gate=popups (fuse_app_wayland_popups): headless weston 13 with the desktop shell (windows are not fullscreened,
//   so popups can extend past them). Weston's headless backend has no seat (the test plugin, weston-test, is not in
//   Ubuntu's weston package), so input comes through the backend's injection path and popups are not grabbed:
//     * an OS popup is an xdg_popup: left out of the window's draw list, shown in exactly one popup surface, placed
//       relative to the window (xdg_positioner), its committed buffer equal to the CPU raster of its draw list
//     * a click on an item that lies outside the main window picks it and the popup goes away; a submenu is a
//       nested xdg_popup on the menu's surface
//     * a tool window ("os-window" overlay node) is a child xdg_toplevel (set_parent): its buffer is the CPU raster
//       of its surface list, a click into it reaches the panel's button in context coordinates
//     * title changes, fullscreen and maximise (custom chrome) through xdg_toplevel; icons / cursor shapes report
//       whether the compositor has xdg_toplevel_icon_v1 / wp_cursor_shape_v1
// --gate=seat (fuse_app_wayland_seat): headless sway 1.9 (wlroots 0.17, unpacked rootless from Ubuntu's sway +
//   libwlroots12t64 into ~/opt/sway), which has a seat driven by a second client (tests/wayland_peer.hpp:
//   zwlr_virtual_pointer_v1 / zwp_virtual_keyboard_v1), wp_fractional_scale_v1, wp_cursor_shape_v1,
//   zwp_text_input_v3 + zwp_input_method_v2 and zwp_primary_selection_v1:
//     * fractional scale 1.5 (buffer = logical * 1.5 through a wp_viewport), then per-output changes to 2 and 1.25
//       (swaymsg output scale) re-scale the window live
//     * real pointer / keyboard input: a click opens a menu as a grabbed xdg_popup; a click on empty desktop ends the
//       grab (popup_done) and closes it; keys typed with the popup open drive the menu (keyboard focus on the popup)
//     * clipboard round trip with the second client: text, image/png, a custom MIME type and a lazy item (made only
//       when the peer pastes it), then the peer's selection (text + custom type) read back by the app; the same for
//       the primary selection
//     * IME: the peer is the input method; its pre-edit reaches UiContext::composition, its commit the line edit
//     * cursor shape (wp_cursor_shape_v1 "text" over a line edit), the title as sway reports it
//     * drag and drop: the peer drags text/uri-list from its window onto the app window -> NativeHooks::dropped
// Exit 77 when the compositor cannot be run.
#include "fuse/app/clipboard.hpp"
#include "fuse/app/native.hpp"
#include "fuse/app/chrome.hpp"
#include "fuse/ui/widgets/widgets.hpp"
#include "wayland_internal.hpp"
#include "wayland_peer.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace fuse::ui;
namespace app = fuse::app;
namespace wl = fuse::app::wl;
namespace w = fuse::ui::widgets;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
void settle(app::Application& a, int n = 20) {
    for (int i = 0; i < n; ++i) a.step(0.005f);
}
template <class P>
bool pumpUntil(app::Application& a, P pred, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) return false;
        a.step(0.005f);
    }
    return true;
}
std::string tmpDir(const char* tag) {
    std::string t = std::string("/tmp/") + tag + "-XXXXXX";
    std::vector<char> b(t.begin(), t.end());
    b.push_back(0);
    if (!::mkdtemp(b.data())) return {};
    ::chmod(b.data(), 0700);
    return b.data();
}
bool waitSocket(const std::string& path, pid_t pid) {
    for (int i = 0; i < 400; ++i) { // up to 8 s
        struct stat st{};
        if (::stat(path.c_str(), &st) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            return true;
        }
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
void stopChild(pid_t& pid, std::string& dir) {
    if (pid > 0) {
        ::kill(pid, SIGTERM);
        int status = 0;
        for (int i = 0; i < 100 && ::waitpid(pid, &status, WNOHANG) != pid; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, WNOHANG);
        pid = -1;
    }
    if (!dir.empty()) {
        const std::string cmd = "rm -rf '" + dir + "'";
        [[maybe_unused]] int rc = std::system(cmd.c_str());
        dir.clear();
    }
}
std::string findExe(const char* env, const char* name, const char* configured) {
    if (const char* e = std::getenv(env); e && *e) return e;
    if (configured && *configured && ::access(configured, X_OK) == 0) return configured;
    if (const char* home = std::getenv("HOME")) { // unpacked rootless: ~/opt/*/usr/bin/<name>
        const std::string opt = std::string(home) + "/opt";
        if (DIR* d = ::opendir(opt.c_str())) {
            while (dirent* e = ::readdir(d)) {
                const std::string p = opt + "/" + e->d_name + "/usr/bin/" + name;
                if (e->d_name[0] != '.' && ::access(p.c_str(), X_OK) == 0) {
                    ::closedir(d);
                    return p;
                }
            }
            ::closedir(d);
        }
    }
    return {};
}
std::string rootOf(const std::string& exe) { return exe.find("/usr/bin/") != std::string::npos ? exe.substr(0, exe.rfind("/usr/bin/")) : std::string(); }

// ---- headless weston 13, desktop shell ----
struct Weston {
    pid_t pid = -1;
    std::string dir, socket = "fuse-wl-parity";
    bool start(const std::string& exe) {
        dir = tmpDir("fuse-wlp");
        if (dir.empty()) return false;
        const std::string root = rootOf(exe), lib = root + "/usr/lib/x86_64-linux-gnu";
        const bool unpacked = !root.empty() && ::access((lib + "/libweston-13.so.0").c_str(), R_OK) == 0;
        std::fflush(nullptr);
        pid = ::fork();
        if (pid < 0) return false;
        if (pid == 0) {
            ::setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
            ::unsetenv("WAYLAND_DISPLAY");
            ::unsetenv("DISPLAY");
            if (unpacked) {
                const std::string ld = lib + ":" + lib + "/weston:" + lib + "/libweston-13";
                ::setenv("LD_LIBRARY_PATH", ld.c_str(), 1);
                const std::string map = "headless-backend.so=" + lib + "/libweston-13/headless-backend.so;desktop-shell.so=" + lib +
                                        "/weston/desktop-shell.so;weston-desktop-shell=" + root + "/usr/libexec/weston-desktop-shell";
                ::setenv("WESTON_MODULE_MAP", map.c_str(), 1);
            }
            const std::string sock = "--socket=" + socket, log = "--log=" + dir + "/weston.log";
            if (!std::freopen("/dev/null", "w", stdout) || !std::freopen("/dev/null", "w", stderr)) ::_exit(126);
            ::execl(exe.c_str(), exe.c_str(), "--backend=headless", "--shell=desktop", "--renderer=pixman", "--no-config", "--width=1280",
                    "--height=1024", sock.c_str(), log.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        return waitSocket(dir + "/" + socket, pid);
    }
    void use() const {
        ::setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
        ::setenv("WAYLAND_DISPLAY", socket.c_str(), 1);
    }
    ~Weston() { stopChild(pid, dir); }
};

// ---- headless sway 1.9 ----
struct Sway {
    pid_t pid = -1;
    std::string dir, exe, ipc, socket;
    bool start(const std::string& swayExe, f32 scale) {
        exe = swayExe;
        dir = tmpDir("fuse-sway");
        if (dir.empty()) return false;
        char sc[32];
        std::snprintf(sc, sizeof sc, "%g", double(scale));
        const std::string cfg = dir + "/config";
        if (FILE* f = std::fopen(cfg.c_str(), "w")) {
            std::fprintf(f, "xwayland disable\noutput HEADLESS-1 resolution 1024x768 scale %s\ndefault_border none\n"
                            "default_floating_border none\nfocus_follows_mouse no\n"
                            "for_window [app_id=\"fuse\"] floating enable\n"
                            "for_window [title=\"^fuse-seat-main$\"] move position 300 40\n"
                            "for_window [app_id=\"peer\"] floating enable, move position 10 300\n", sc);
            std::fclose(f);
        } else {
            return false;
        }
        const std::string root = rootOf(exe);
        std::fflush(nullptr);
        pid = ::fork();
        if (pid < 0) return false;
        if (pid == 0) {
            ::setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
            ::unsetenv("WAYLAND_DISPLAY");
            ::unsetenv("DISPLAY");
            ::unsetenv("SWAYSOCK");
            if (!root.empty()) ::setenv("LD_LIBRARY_PATH", (root + "/usr/lib/x86_64-linux-gnu").c_str(), 1);
            ::setenv("WLR_BACKENDS", "headless", 1);
            ::setenv("WLR_RENDERER", "pixman", 1);
            ::setenv("WLR_LIBINPUT_NO_DEVICES", "1", 1);
            ::setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
            const std::string log = dir + "/sway.log";
            if (!std::freopen("/dev/null", "w", stdout) || !std::freopen(log.c_str(), "w", stderr)) ::_exit(126);
            if (std::getenv("FUSE_TEST_SWAY_DEBUG")) ::execl(exe.c_str(), exe.c_str(), "-d", "-c", cfg.c_str(), static_cast<char*>(nullptr));
            ::execl(exe.c_str(), exe.c_str(), "-c", cfg.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        for (int i = 0; i < 400; ++i) { // the Wayland socket and the IPC socket
            if (DIR* d = ::opendir(dir.c_str())) {
                while (dirent* e = ::readdir(d)) {
                    const std::string n = e->d_name;
                    if (n.rfind("wayland-", 0) == 0 && n.find(".lock") == std::string::npos) socket = n;
                    if (n.rfind("sway-ipc.", 0) == 0) ipc = dir + "/" + n;
                }
                ::closedir(d);
            }
            if (!socket.empty() && !ipc.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                return true;
            }
            int status = 0;
            if (::waitpid(pid, &status, WNOHANG) == pid) { pid = -1; return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }
    void use() const {
        ::setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
        ::setenv("WAYLAND_DISPLAY", socket.c_str(), 1);
    }
    /// swaymsg -s <ipc> <args>: its output.
    std::string msg(const std::string& args) const {
        const std::string root = rootOf(exe);
        const std::string cmd = (root.empty() ? std::string() : "LD_LIBRARY_PATH='" + root + "/usr/lib/x86_64-linux-gnu' ") + "'" +
                                exe.substr(0, exe.rfind('/')) + "/swaymsg' -s '" + ipc + "' " + args + " 2>&1";
        std::string out;
        if (FILE* p = ::popen(cmd.c_str(), "r")) {
            char b[4096];
            std::size_t n;
            while ((n = std::fread(b, 1, sizeof b, p)) > 0) out.append(b, n);
            ::pclose(p);
        }
        return out;
    }
    /// Layout rect (logical) of the window titled `title` (sway's get_tree), false when not found.
    bool windowRect(const std::string& title, Rect& r) const {
        const std::string t = msg("-t get_tree");
        const std::size_t at = t.find("\"name\": \"" + title + "\"");
        if (at == std::string::npos) return false;
        const std::size_t rp = t.rfind("\"rect\": {", at);
        if (rp == std::string::npos) return false;
        auto num = [&](const char* key) {
            const std::size_t k = t.find(std::string("\"") + key + "\": ", rp);
            return k == std::string::npos ? 0.f : std::strtof(t.c_str() + k + std::strlen(key) + 4, nullptr);
        };
        r = {num("x"), num("y"), num("width"), num("height")};
        return r.w > 0.f;
    }
    bool outputSize(f32& w, f32& h) const {
        const std::string t = msg("-t get_outputs");
        const std::size_t rp = t.find("\"rect\": {");
        if (rp == std::string::npos) return false;
        auto num = [&](const char* key) {
            const std::size_t k = t.find(std::string("\"") + key + "\": ", rp);
            return k == std::string::npos ? 0.f : std::strtof(t.c_str() + k + std::strlen(key) + 4, nullptr);
        };
        w = num("width");
        h = num("height");
        return w > 0.f && h > 0.f;
    }
    ~Sway() {
        if (const char* keep = std::getenv("FUSE_TEST_SWAY_DEBUG"); keep && *keep && !dir.empty()) { // keep sway's log
            const std::string cmd = "cp '" + dir + "/sway.log' '" + keep + "'";
            [[maybe_unused]] int rc = std::system(cmd.c_str());
        }
        stopChild(pid, dir);
    }
};

/// Max channel error between two images (255 when the sizes differ).
int maxError(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height) return 255;
    int e = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i)
        for (int c = 0; c < 3; ++c) e = std::max(e, std::abs(int((a.pixels[i] >> (8 * c)) & 0xFF) - int((b.pixels[i] >> (8 * c)) & 0xFF)));
    return e;
}
/// The CPU raster of a popup / window surface list, like NativePlatform renders it.
Image rasterOf(app::AppWindow& win, const UiContext::PopupSurface& ps, int width, int height) {
    u32 clear = 0xFF000000u;
    for (const DrawCmd& c : ps.draw.cmds())
        if (c.op != DrawOp::Shadow && c.fill.a > 0.9f) { clear = packPremultiplied(c.fill) | 0xFF000000u; break; }
    Image ref;
    ref.resize(width, height, clear);
    CpuRasterizer rz = win.rasterizer();
    rz.setGlyphAtlas(win.ui().atlas().pixels(), win.ui().atlas().size());
    rz.render(ps.draw, ref);
    return ref;
}
NodeHandle itemNamed(UiContext& ui, NodeHandle panel, std::string_view text) {
    for (NodeHandle it = ui.firstChild(panel); ui.alive(it); it = ui.nextSibling(it))
        if (ui.text(it).substr(0, ui.text(it).find('\n')) == text) return it;
    return {};
}

// ============================================================================================================== popups
int gatePopups() {
    const std::string exe = findExe("FUSE_TEST_WESTON", "weston",
#ifdef FUSE_TEST_WESTON_PATH
                                    FUSE_TEST_WESTON_PATH
#else
                                    nullptr
#endif
    );
    if (exe.empty()) {
        std::printf("fuse_app_wayland_popups: skipped (no weston; set FUSE_TEST_WESTON)\n");
        return 77;
    }
    Weston ws;
    if (!ws.start(exe)) {
        std::printf("fuse_app_wayland_popups: skipped (weston %s did not start)\n", exe.c_str());
        return 77;
    }
    ws.use();
    app::Application a{{.name = "gate_wayland_popups", .loadSettings = false}};
    app::NativePlatform native(a, {.vulkan = false, .osPopups = true});
    if (!native.available()) {
        std::printf("fuse_app_wayland_popups: no Wayland backend (%.*s)\n", int(native.lastError().size()), native.lastError().data());
        return 1;
    }
    wl::Backend* b = wl::current();
    std::printf("  weston: seat %s, viewporter %d, fractional scale %d, cursor shape %d, text-input-v3 %d, toplevel icon %d, "
                "primary selection %d, data device %d\n",
                wl::hasGlobal(b, "wl_seat") ? "yes" : "no (headless backend)", wl::hasGlobal(b, "wp_viewporter"),
                wl::hasGlobal(b, "wp_fractional_scale_manager_v1"), wl::hasGlobal(b, "wp_cursor_shape_manager_v1"),
                wl::hasGlobal(b, "zwp_text_input_manager_v3"), wl::hasGlobal(b, "xdg_toplevel_icon_manager_v1"),
                wl::hasGlobal(b, "zwp_primary_selection_device_manager_v1"), int(wl::hasDataDevice(b, false)));
    auto& win = a.createWindow({.title = "fuse_app wayland popups", .width = 320, .height = 160});
    UiContext& ui = win.ui();
    const NodeHandle btn = w::button(ui, ui.root(), "File", "file");
    CHECK(native.show(win) == app::PresenterKind::Software);
    for (int i = 0; i < 400 && win.framesPresented() < 1; ++i) a.step(0.01f);
    settle(a);
    CHECK(ui.osPopups());
    const f32 s = win.scale();
    CHECK(native.screenOrigin(win).x == 0.f && native.screenOrigin(win).y == 0.f); // Wayland: window-relative

    f32 picked = -1.f;
    std::string pickedText;
    ui.connect(btn, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& sa) { picked = sa.value; pickedText = sa.text; });
    std::string items;
    for (int i = 1; i <= 12; ++i) items += (i > 1 ? "\n" : "") + std::string("Item ") + std::to_string(i);
    items += "\nMore\n\tSub A\n\tSub B";

    // ---- the popup: an xdg_popup, placement and pixels ----
    const NodeHandle scope = w::popupMenu(ui, btn, items, {40.f, 60.f});
    win.requestRedraw();
    settle(a, 40);
    CHECK(ui.alive(scope) && ui.popupSurfaces().size() == 1 && native.popupCount(win) == 1);
    if (ui.popupSurfaces().size() != 1 || native.popupCount(win) != 1) return 1;
    const Rect menu = ui.rect(ui.firstChild(scope));
    std::printf("  menu rect %.0f,%.0f %.0fx%.0f (window %.0fx%.0f)\n", menu.x, menu.y, menu.w, menu.h, win.size().w, win.size().h);
    CHECK(menu.y + menu.h > win.size().h); // extends past the main window
    bool inMain = false;
    for (const DrawCmd& c : ui.drawList().cmds()) inMain = inMain || c.node == ui.firstChild(scope).index;
    CHECK(!inMain);
    const Rect scr = native.popupScreenRect(win, 0);
    auto* pc = reinterpret_cast<wl::Child*>(native.popupWindow(win, 0));
    const Rect placed = wl::childPlacement(pc);
    std::printf("  xdg_popup at %.0f,%.0f %.0fx%.0f (window-relative, logical); popupScreenRect %.0f,%.0f %.0fx%.0f; grabbed %d\n",
                placed.x, placed.y, placed.w, placed.h, scr.x, scr.y, scr.w, scr.h, int(wl::childGrabbed(pc)));
    CHECK(!wl::childGrabbed(pc)); // no seat: nothing to grab with
    CHECK(std::abs(placed.w - std::ceil(menu.w)) <= 1.f && std::abs(placed.h - std::ceil(menu.h)) <= 1.f);
    CHECK(std::abs(scr.w - std::ceil(menu.w * s)) <= 0.5f && std::abs(scr.h - std::ceil(menu.h * s)) <= 0.5f);
    if (placed.x == std::round(menu.x) && placed.y == std::round(menu.y)) // not slid by the compositor: exactly where the UI put it
        CHECK(std::abs(scr.x - std::round(menu.x * s)) <= 0.5f && std::abs(scr.y - std::round(menu.y * s)) <= 0.5f);
    else
        std::printf("  (the compositor slid the popup to keep it on the output)\n");
    {
        Image shown;
        CHECK(native.capturePopup(win, 0, shown));
        const Image ref = rasterOf(win, ui.popupSurfaces()[0], int(scr.w), int(scr.h));
        const int e = maxError(shown, ref);
        std::printf("  popup buffer %dx%d, max channel error vs CPU raster %d\n", shown.width, shown.height, e);
        CHECK(e <= 2);
    }

    // ---- submenu: a nested xdg_popup on the menu's surface ----
    {
        const NodeHandle panel = ui.popupSurfaces()[0].node;
        const NodeHandle more = itemNamed(ui, panel, "More");
        CHECK(ui.alive(more));
        const Rect mr = ui.rect(more), pr = ui.rect(panel);
        CHECK(native.postClick(native.popupWindow(win, 0), {(mr.x + mr.w * 0.5f - pr.x) * s, (mr.y + mr.h * 0.5f - pr.y) * s}));
        settle(a, 30);
        std::printf("  submenu: popup surfaces %zu, popup windows %zu\n", ui.popupSurfaces().size(), native.popupCount(win));
        CHECK(ui.popupSurfaces().size() == 2 && native.popupCount(win) == 2);
        if (native.popupCount(win) == 2) {
            const Rect sub = ui.popupSurfaces()[1].rect;
            const Rect sp = wl::childPlacement(reinterpret_cast<wl::Child*>(native.popupWindow(win, 1)));
            std::printf("  submenu rect %.0f,%.0f, nested xdg_popup at %.0f,%.0f (window-relative)\n", sub.x, sub.y, sp.x, sp.y);
            Image shown;
            CHECK(native.capturePopup(win, 1, shown));
            const Rect s1 = native.popupScreenRect(win, 1);
            CHECK(maxError(shown, rasterOf(win, ui.popupSurfaces()[1], int(s1.w), int(s1.h))) <= 2);
            // pick "Sub B" in the submenu: the whole cascade closes and reports to the root owner
            const NodeHandle sp2 = ui.popupSurfaces()[1].node;
            const NodeHandle subB = itemNamed(ui, sp2, "Sub B");
            CHECK(ui.alive(subB));
            const Rect br = ui.rect(subB), spr = ui.rect(sp2);
            picked = -1.f;
            CHECK(native.postClick(native.popupWindow(win, 1), {(br.x + br.w * 0.5f - spr.x) * s, (br.y + br.h * 0.5f - spr.y) * s}));
            pumpUntil(a, [&] { return picked >= 0.f; });
            settle(a, 20);
            std::printf("  submenu pick %.0f \"%s\", popup windows now %zu\n", picked, pickedText.c_str(), native.popupCount(win));
            CHECK(pickedText == "Sub B" && native.popupCount(win) == 0);
        }
    }

    // ---- a click on "Item 11" (outside the main window) through the popup surface's input path ----
    {
        picked = -1.f;
        const NodeHandle sc2 = w::popupMenu(ui, btn, items, {40.f, 60.f});
        win.requestRedraw();
        settle(a, 40);
        CHECK(ui.alive(sc2) && native.popupCount(win) == 1);
        const NodeHandle panel = ui.popupSurfaces().empty() ? NodeHandle{} : ui.popupSurfaces()[0].node;
        const NodeHandle item = itemNamed(ui, panel, "Item 11");
        CHECK(ui.alive(item));
        if (ui.alive(item)) {
            const Rect ir = ui.rect(item), pr = ui.rect(panel);
            CHECK(ir.y > win.size().h);
            CHECK(native.postClick(native.popupWindow(win, 0), {(ir.x + ir.w * 0.5f - pr.x) * s, (ir.y + ir.h * 0.5f - pr.y) * s}));
            pumpUntil(a, [&] { return picked >= 0.f; });
            settle(a, 20);
            std::printf("  picked %.0f \"%s\"; popup windows now %zu\n", picked, pickedText.c_str(), native.popupCount(win));
            CHECK(picked == 10.f && pickedText == "Item 11");
            CHECK(native.popupCount(win) == 0 && ui.popupSurfaces().empty());
        }
    }

    // ---- a tool window: an "os-window" overlay node as a child xdg_toplevel ----
    {
        auto f = ui.create("panel", w::overlayLayer(ui), "tool");
        CHECK(f.has_value());
        const NodeHandle frame = f.value_or(NodeHandle{});
        ui.addClass(frame, "os-window");
        ui.setCss(frame, "position", "absolute");
        ui.setCss(frame, "left", "200px");
        ui.setCss(frame, "top", "100px");
        ui.setCss(frame, "width", "220px");
        ui.setCss(frame, "height", "120px");
        ui.setCss(frame, "background-color", "#304050");
        int clicks = 0;
        const NodeHandle tb = w::button(ui, frame, "Tool button", "tool.btn");
        ui.connect(tb, SignalKind::Clicked, [&](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
        win.requestRedraw();
        settle(a, 30);
        std::printf("  tool window: surfaces %zu, OS windows %zu\n", ui.windowSurfaces().size(), native.toolWindowCount(win));
        CHECK(ui.windowSurfaces().size() == 1 && native.toolWindowCount(win) == 1);
        if (native.toolWindowCount(win) == 1) {
            const Rect tr = native.toolWindowScreenRect(win, 0), fr = ui.rect(frame);
            std::printf("  tool frame %.0f,%.0f %.0fx%.0f, window rect %.0f,%.0f %.0fx%.0f\n", fr.x, fr.y, fr.w, fr.h, tr.x, tr.y, tr.w, tr.h);
            CHECK(std::abs(tr.w - std::ceil(fr.w * s)) <= 1.f && std::abs(tr.h - std::ceil(fr.h * s)) <= 1.f);
            Image shown;
            CHECK(native.captureToolWindow(win, 0, shown));
            const int e = maxError(shown, rasterOf(win, ui.windowSurfaces()[0], int(tr.w), int(tr.h)));
            std::printf("  tool window buffer %dx%d, max channel error vs CPU raster %d\n", shown.width, shown.height, e);
            CHECK(e <= 2);
            const Rect br = ui.rect(tb);
            CHECK(native.postClick(native.toolWindow(win, 0), {(br.x + br.w * 0.5f - fr.x) * s, (br.y + br.h * 0.5f - fr.y) * s}));
            settle(a, 10);
            CHECK(clicks == 1);
        }
        ui.destroy(frame);
        win.requestRedraw();
        settle(a, 20);
        CHECK(native.toolWindowCount(win) == 0);
    }

    // ---- title, fullscreen, icon / cursor protocol reports ----
    win.setTitle("fuse_app wayland popups - renamed");
    win.requestRedraw();
    settle(a, 5);
    CHECK(native.setFullscreen(win, true));
    pumpUntil(a, [&] { return native.fullscreen(win); });
    settle(a, 10);
    std::printf("  fullscreen %d, size %.0fx%.0f\n", int(native.fullscreen(win)), win.size().w, win.size().h);
    CHECK(native.fullscreen(win) && win.size().w == 1280.f && win.size().h == 1024.f);
    CHECK(native.setFullscreen(win, false));
    pumpUntil(a, [&] { return !native.fullscreen(win); });
    settle(a, 10);
    CHECK(!native.fullscreen(win));
    {
        Image icon;
        icon.resize(32, 32, 0xFF2080F0u);
        const bool iconOk = native.setIcon(win, std::span<const Image>(&icon, 1));
        CHECK(iconOk == wl::hasGlobal(b, "xdg_toplevel_icon_manager_v1"));
    }

    // ---- custom chrome: maximise through xdg_toplevel.set_maximized, the configure reports it ----
    {
        auto& cw = a.createWindow({.title = "fuse_app wayland chrome", .width = 300, .height = 200, .frame = app::WindowFrame::Custom});
        app::titleBar(cw, cw.ui().root(), "Chrome");
        native.show(cw);
        for (int i = 0; i < 400 && cw.framesPresented() < 1; ++i) a.step(0.01f);
        settle(a);
        cw.command(app::WindowCommand::ToggleMaximize);
        pumpUntil(a, [&] { return cw.chrome().maximized; });
        settle(a, 10);
        std::printf("  custom chrome: maximized %d, size %.0fx%.0f\n", int(cw.chrome().maximized), cw.size().w, cw.size().h);
        CHECK(cw.chrome().maximized && cw.size().w > 300.f);
        cw.command(app::WindowCommand::ToggleMaximize);
        pumpUntil(a, [&] { return !cw.chrome().maximized; });
        CHECK(!cw.chrome().maximized);
        cw.requestClose();
        a.step(0.f);
    }
    win.requestClose();
    a.step(0.f);
    std::printf("fuse_app_wayland_popups: %s\n", g_fail ? "FAIL" : "pass");
    return g_fail ? 1 : 0;
}

// ================================================================================================================ seat
struct SeatRig {
    Sway& sway;
    peer::Peer& pr;
    app::Application& a;
    Rect winRect; ///< the app window in layout coordinates (logical)
    /// Real pointer: moves to window-local logical `p` of the app window and clicks.
    void click(Vec2 p) {
        pr.call(a, [&] {
            pr.moveTo(winRect.x + p.x, winRect.y + p.y);
            pr.button(true);
            pr.button(false);
        });
        settle(a, 10);
    }
    void move(Vec2 p) {
        pr.call(a, [&] { pr.moveTo(winRect.x + p.x, winRect.y + p.y); });
        settle(a, 5);
    }
    void clickLayout(Vec2 p) {
        pr.call(a, [&] {
            pr.moveTo(p.x, p.y);
            pr.button(true);
            pr.button(false);
        });
        settle(a, 10);
    }
    void tap(unsigned code) {
        pr.call(a, [&] { pr.tap(code); });
        settle(a, 5);
    }
};
Vec2 centreOf(UiContext& ui, NodeHandle n) {
    const Rect r = ui.rect(n);
    return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
}

int gateSeat() {
    const std::string exe = findExe("FUSE_TEST_SWAY", "sway",
#ifdef FUSE_TEST_SWAY_PATH
                                    FUSE_TEST_SWAY_PATH
#else
                                    nullptr
#endif
    );
    if (exe.empty()) {
        std::printf("fuse_app_wayland_seat: skipped (no sway; set FUSE_TEST_SWAY)\n");
        return 77;
    }
    Sway sw;
    if (!sw.start(exe, 1.5f)) {
        std::printf("fuse_app_wayland_seat: skipped (sway %s did not start)\n", exe.c_str());
        return 77;
    }
    sw.use();
    app::Application a{{.name = "gate_wayland_seat", .loadSettings = false}};
    app::NativePlatform native(a, {.vulkan = false, .osPopups = true});
    if (!native.available()) {
        std::printf("fuse_app_wayland_seat: no Wayland backend (%.*s)\n", int(native.lastError().size()), native.lastError().data());
        return 1;
    }
    wl::Backend* b = wl::current();
    std::printf("  sway: fractional scale %d, viewporter %d, cursor shape %d, text-input-v3 %d, primary selection %d, toplevel icon %d\n",
                wl::hasGlobal(b, "wp_fractional_scale_manager_v1"), wl::hasGlobal(b, "wp_viewporter"),
                wl::hasGlobal(b, "wp_cursor_shape_manager_v1"), wl::hasGlobal(b, "zwp_text_input_manager_v3"),
                wl::hasGlobal(b, "zwp_primary_selection_device_manager_v1"), wl::hasGlobal(b, "xdg_toplevel_icon_manager_v1"));
    peer::Peer pr;
    std::string perr;
    if (!pr.start(perr)) {
        std::printf("fuse_app_wayland_seat: the second client failed: %s\n", perr.c_str());
        return 1;
    }
    CHECK(pr.pointerReady() && pr.keyboardReady() && pr.imReady());
    f32 lw = 0.f, lh = 0.f;
    CHECK(sw.outputSize(lw, lh));
    pr.extentW = lw;
    pr.extentH = lh;
    std::printf("  layout %gx%g (1024x768 at scale 1.5)\n", double(lw), double(lh));

    constexpr const char* kDoc = R"({ "fuseui": 1, "root": { "class": "panel", "id": "p", "children": [
        { "class": "button", "id": "file", "text": "File" },
        { "class": "lineedit", "id": "e", "text": "" } ] } })";
    auto& win = a.createWindow({.title = "fuse-seat-main", .width = 320, .height = 160});
    CHECK(win.loadText(kDoc));
    UiContext& ui = win.ui();
    std::vector<std::pair<std::string, std::string>> drops;
    native.setHooks({.user = &drops, .dropped = [](void* u, app::AppWindow&, Vec2, std::string_view mime, std::string_view data) {
                         static_cast<std::vector<std::pair<std::string, std::string>>*>(u)->emplace_back(mime, data);
                     }});
    CHECK(native.show(win) == app::PresenterKind::Software);
    for (int i = 0; i < 400 && win.framesPresented() < 1; ++i) a.step(0.01f);
    settle(a, 40);
    auto* ww = reinterpret_cast<wl::Window*>(native.nativeWindow(win));

    // ---- fractional scale ----
    pumpUntil(a, [&] { return win.scale() == 1.5f; });
    Image shown;
    CHECK(native.capture(win, shown));
    std::printf("  scale %.3f (fractional %d), logical %.0fx%.0f, buffer %dx%d\n", double(win.scale()), int(wl::fractional(ww)), win.size().w,
                win.size().h, shown.width, shown.height);
    CHECK(wl::fractional(ww) && win.scale() == 1.5f);
    CHECK(shown.width == int(std::lround(win.size().w * 1.5f)) && shown.height == int(std::lround(win.size().h * 1.5f)));
    {
        const Image& ref = win.rasterize({});
        CHECK(maxError(shown, ref) == 0);
    }
    SeatRig rig{sw, pr, a, {}};
    pumpUntil(a, [&] { return sw.windowRect("fuse-seat-main", rig.winRect); }, 2000);
    CHECK(rig.winRect.w > 0.f);
    std::printf("  app window at %.0f,%.0f %.0fx%.0f (layout)\n", rig.winRect.x, rig.winRect.y, rig.winRect.w, rig.winRect.h);

    // ---- real pointer: a click opens a grabbed xdg_popup; empty desktop ends the grab ----
    const NodeHandle btn = ui.find("file");
    f32 picked = -1.f;
    std::string pickedText;
    ui.connect(btn, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& sa) { picked = sa.value; pickedText = sa.text; });
    ui.connect(btn, SignalKind::Clicked, [&](UiContext& u, NodeHandle n, const SignalArgs&) {
        const Rect r = u.rect(n);
        w::popupMenu(u, n, "Alpha\nBeta\nGamma\nDelta", {r.x, r.bottom()});
    });
    rig.click(centreOf(ui, btn));
    pumpUntil(a, [&] { return native.popupCount(win) == 1; });
    settle(a, 10);
    CHECK(native.popupCount(win) == 1);
    if (native.popupCount(win) == 1) {
        auto* c = reinterpret_cast<wl::Child*>(native.popupWindow(win, 0));
        std::printf("  real click opened the menu: grabbed %d\n", int(wl::childGrabbed(c)));
        CHECK(wl::childGrabbed(c));
        Image ps;
        const Rect pr0 = native.popupScreenRect(win, 0);
        CHECK(native.capturePopup(win, 0, ps) && ps.width == int(pr0.w));
        rig.clickLayout({150.f, 20.f}); // no surface there: the compositor ends the grab
        pumpUntil(a, [&] { return native.popupCount(win) == 0; });
        std::printf("  outside click: popup windows %zu, dismissed by the compositor; picked %.0f\n", native.popupCount(win), picked);
        CHECK(native.popupCount(win) == 0 && picked < 0.f);
    }
    // keys while the grabbed popup has the keyboard: Down, Down, Enter from "Alpha" picks "Gamma"
    rig.click(centreOf(ui, btn));
    pumpUntil(a, [&] { return native.popupCount(win) == 1; });
    settle(a, 10);
    rig.tap(108); // KEY_DOWN
    rig.tap(108);
    rig.tap(28); // KEY_ENTER
    pumpUntil(a, [&] { return picked >= 0.f; });
    settle(a, 10);
    std::printf("  keyboard in the grabbed popup picked %.0f \"%s\"\n", picked, pickedText.c_str());
    CHECK(pickedText == "Gamma"); // the menu opens with "Alpha" focused
    CHECK(native.popupCount(win) == 0);
    // a real click on an item picks it
    picked = -1.f;
    rig.click(centreOf(ui, btn));
    pumpUntil(a, [&] { return native.popupCount(win) == 1; });
    settle(a, 10);
    if (!ui.popupSurfaces().empty()) {
        const NodeHandle panel = ui.popupSurfaces()[0].node;
        const NodeHandle g = itemNamed(ui, panel, "Gamma");
        const Rect pl = wl::childPlacement(reinterpret_cast<wl::Child*>(native.popupWindow(win, 0)));
        const Rect gr = ui.rect(g), prr = ui.rect(panel);
        rig.click({pl.x + gr.x - prr.x + gr.w * 0.5f, pl.y + gr.y - prr.y + gr.h * 0.5f}); // where the compositor shows it
        pumpUntil(a, [&] { return picked >= 0.f; });
        std::printf("  real click on the popup picked \"%s\"\n", pickedText.c_str());
        CHECK(pickedText == "Gamma");
    }

    // ---- keyboard: a real click focuses the line edit, virtual keys type into it; cursor shape over it ----
    const NodeHandle edit = ui.find("e");
    rig.click(centreOf(ui, edit));
    CHECK(ui.focused() == edit);
    rig.tap(30); // a
    rig.tap(48); // b
    std::printf("  line edit after virtual keys: '%.*s'\n", int(ui.text(edit).size()), ui.text(edit).data());
    CHECK(ui.text(edit) == "ab");
    rig.move(centreOf(ui, edit));
    settle(a, 5);
    std::printf("  cursor over the line edit: applied %d, wp_cursor_shape_v1 shape %u\n", int(native.appliedCursor(win)), wl::cursorShapeSent(ww));
    if (wl::hasGlobal(b, "wp_cursor_shape_manager_v1")) CHECK(wl::cursorShapeSent(ww) == 9); // "text"

    // ---- IME: the peer is the input method (zwp_input_method_v2) ----
    {
        CHECK(native.inputMethodAvailable() && native.textInputActive(win));
        const bool active = pr.call(a, [&] { return pr.waitFor([&] { return pr.imActive && pr.imDone > 0; }); });
        std::printf("  input method activated %d (done %d)\n", int(active), pr.imDone);
        CHECK(active);
        pr.call(a, [&] { pr.imPreedit("ni", 2, 2); });
        pumpUntil(a, [&] { return ui.composition() == "ni"; });
        std::printf("  pre-edit '%.*s' caret %d\n", int(ui.composition().size()), ui.composition().data(), ui.compositionCaret());
        CHECK(ui.composition() == "ni" && ui.compositionCaret() == 2);
        pr.call(a, [&] { pr.imCommit("\xE4\xBD\xA0"); }); // 你
        pumpUntil(a, [&] { return ui.text(edit) == "ab\xE4\xBD\xA0"; });
        std::printf("  after commit: '%.*s', pre-edit '%.*s'\n", int(ui.text(edit).size()), ui.text(edit).data(), int(ui.composition().size()),
                    ui.composition().data());
        CHECK(ui.text(edit) == "ab\xE4\xBD\xA0" && ui.composition().empty());
    }

    // ---- clipboard: the app's selection read by the peer (text, image, custom, lazy) ----
    {
        int lazyCalls = 0;
        Image img;
        img.resize(4, 3, 0xFF3366CCu);
        img.pixels[5] = 0x80FF0000u;
        std::vector<app::ClipboardItem> items(4);
        items[0].mime = std::string(app::kClipboardText);
        items[0].text = "fuse \xE2\x86\x92 peer";
        items[1].mime = std::string(app::kClipboardImage);
        items[1].image = img;
        items[2].mime = "application/x-fuse-test";
        items[2].bytes = {1, 2, 3, 0, 255};
        items[3].mime = "application/x-fuse-lazy";
        items[3].provide = [&](app::ClipboardItem& it) { ++lazyCalls; it.bytes = {'l', 'a', 'z', 'y'}; return true; };
        CHECK(app::setClipboard(std::move(items)));
        CHECK(app::clipboardOwned());
        CHECK(pr.call(a, [&] { return pr.openWindow("peer-clip"); })); // the peer takes the focus: the selection reaches it
        std::string text, png, custom, lazy;
        CHECK(pr.call(a, [&] { return pr.readSelection(0, "text/plain;charset=utf-8", text); }));
        CHECK(pr.call(a, [&] { return pr.readSelection(0, "image/png", png); }));
        CHECK(pr.call(a, [&] { return pr.readSelection(0, "application/x-fuse-test", custom); }));
        CHECK(lazyCalls == 0);
        CHECK(pr.call(a, [&] { return pr.readSelection(0, "application/x-fuse-lazy", lazy); }));
        std::printf("  peer read: text '%s', png %zu bytes, custom %zu bytes, lazy '%s' (provider calls %d)\n", text.c_str(), png.size(),
                    custom.size(), lazy.c_str(), lazyCalls);
        CHECK(text == "fuse \xE2\x86\x92 peer");
        CHECK(png.size() > 8 && png.compare(1, 3, "PNG") == 0);
        CHECK(custom == std::string("\x01\x02\x03\x00\xff", 5));
        CHECK(lazy == "lazy" && lazyCalls == 1);
        // the peer's selection, read back by the app once its window has the focus again
        CHECK(pr.call(a, [&] { return pr.setSelection(0, {{"text/plain;charset=utf-8", "peer \xE2\x86\x92 fuse"}, {"application/x-peer", "P1"}}); }));
        CHECK(pr.call(a, [&] { return pr.setSelection(1, {{"text/plain;charset=utf-8", "peer primary"}}); }));
        pr.call(a, [&] { pr.closeWindow(); });
        rig.click(centreOf(ui, edit)); // focus back on the app (sway also refocuses it when the peer window goes)
        pumpUntil(a, [&] { return !app::clipboardOwned() && !app::clipboardFormats().empty(); });
        std::string back;
        CHECK(app::clipboardText(back));
        const auto fm = app::clipboardFormats();
        std::vector<std::uint8_t> pd;
        CHECK(app::clipboardData("application/x-peer", pd));
        std::printf("  app read: '%s', formats %zu, custom '%.*s'\n", back.c_str(), fm.size(), int(pd.size()), reinterpret_cast<const char*>(pd.data()));
        CHECK(back == "peer \xE2\x86\x92 fuse");
        CHECK(std::find(fm.begin(), fm.end(), std::string(app::kClipboardText)) != fm.end() &&
              std::find(fm.begin(), fm.end(), std::string("application/x-peer")) != fm.end());
        CHECK(std::string(pd.begin(), pd.end()) == "P1");
        CHECK(!app::clipboardOwned());
        // primary selection both ways
        std::string prim;
        pumpUntil(a, [&] { return app::primarySelectionText(prim) && prim == "peer primary"; });
        std::printf("  primary selection from the peer: '%s'\n", prim.c_str());
        CHECK(prim == "peer primary");
        CHECK(app::setPrimarySelectionText("fuse primary"));
        CHECK(pr.call(a, [&] { return pr.openWindow("peer-clip2"); }));
        std::string pp;
        CHECK(pr.call(a, [&] { return pr.readSelection(1, "text/plain;charset=utf-8", pp); }));
        std::printf("  peer read the primary selection: '%s'\n", pp.c_str());
        CHECK(pp == "fuse primary");
        // ---- drag and drop: the peer drags a URI list from its window onto the app window ----
        Rect peerRect;
        if (sw.windowRect("peer-clip2", peerRect)) {
            pr.call(a, [&] {
                pr.moveTo(peerRect.x + 50.0, peerRect.y + 50.0);
                pr.button(true);
                pr.startDrag("file:///tmp/fuse-drop.txt\r\n");
                for (int k = 1; k <= 6; ++k)
                    pr.moveTo(peerRect.x + 50.0 + (rig.winRect.x + 160.0 - peerRect.x - 50.0) * k / 6.0,
                              peerRect.y + 50.0 + (rig.winRect.y + 80.0 - peerRect.y - 50.0) * k / 6.0);
            });
            settle(a, 10);
            const bool over = wl::dragOver(ww);
            pr.call(a, [&] { pr.button(false); });
            pumpUntil(a, [&] { return !drops.empty(); });
            std::printf("  drag over the app %d, drops %zu%s%s\n", int(over), drops.size(), drops.empty() ? "" : ": ",
                        drops.empty() ? "" : (drops[0].first + " " + drops[0].second).c_str());
            CHECK(over && drops.size() == 1 && drops[0].first == "text/uri-list" && drops[0].second.find("file:///tmp/fuse-drop.txt") == 0);
        } else {
            CHECK(!"the peer window is not in sway's tree");
        }
        pr.call(a, [&] { pr.closeWindow(); });
    }

    // ---- the title as the compositor has it ----
    win.setTitle("fuse-seat-renamed");
    win.requestRedraw();
    settle(a, 10);
    Rect renamed;
    CHECK(sw.windowRect("fuse-seat-renamed", renamed));

    // ---- per-output scale changes: 2, then 1.25 (wp_fractional_scale_v1 preferred_scale) ----
    for (f32 want : {2.f, 1.25f}) {
        char cmd[96];
        std::snprintf(cmd, sizeof cmd, "output HEADLESS-1 scale %g", double(want));
        sw.msg(cmd);
        pumpUntil(a, [&] { return win.scale() == want; });
        settle(a, 10);
        CHECK(native.capture(win, shown));
        std::printf("  output scale %g: window scale %.3f, buffer %dx%d (logical %.0fx%.0f)\n", double(want), double(win.scale()), shown.width,
                    shown.height, win.size().w, win.size().h);
        CHECK(win.scale() == want);
        CHECK(shown.width == int(std::lround(win.size().w * want)) && shown.height == int(std::lround(win.size().h * want)));
    }
    win.requestClose();
    a.step(0.f);
    std::printf("fuse_app_wayland_seat: %s\n", g_fail ? "FAIL" : "pass");
    return g_fail ? 1 : 0;
}
} // namespace

int main(int argc, char** argv) {
    std::string gate = "popups";
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--gate=", 7) == 0) gate = argv[i] + 7;
    ::setenv("FUSE_APP_BACKEND", "wayland", 1);
    ::unsetenv("FUSE_UI_SCALE");
    std::signal(SIGPIPE, SIG_IGN);
    if (gate == "popups") return gatePopups();
    if (gate == "seat") return gateSeat();
    std::fprintf(stderr, "unknown gate %s\n", gate.c_str());
    return 2;
}
