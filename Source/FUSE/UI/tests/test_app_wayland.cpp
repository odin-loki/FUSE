// fuse_app Wayland backend gate (fuse_app_wayland, Linux).
//   * without a compositor: libwayland-client resolves, the hand-written xdg-shell / xdg-decoration tables are
//     consistent (one types entry per argument, opcodes as the backend uses them, core opcodes match
//     libwayland-client's own tables), xkbcommon and the no-xkb fallback map evdev codes to the same FUSE keys / text
//   * under a headless weston started by the gate (private XDG_RUNTIME_DIR; $FUSE_TEST_WESTON or the weston found at
//     configure time, e.g. one unpacked rootless into ~/opt/weston): an xdg_toplevel with the wl_shm software
//     presenter (the committed buffer equals the CPU raster), pointer click and key text through the backend's
//     input path, a client-side resize, xdg_toplevel.close, wl_output displays; again with weston --scale=2 (buffer
//     scale 2, a 2x buffer); and the Vulkan presenter on a VK_KHR_wayland_surface (lavapipe / the GPU), read back
//   * $FUSE_TEST_WAYLAND_EXISTING=1 uses the session's $WAYLAND_DISPLAY instead of starting weston
// Exit 77 (skip) when the library checks pass but no compositor can be run.
#include "fuse/app/native.hpp"
#include "fuse/ui/doc/document.hpp"
#include "wayland_internal.hpp"

#include <cerrno>
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

namespace fuse::app::x11 {
Key mapKey(unsigned long s, bool& ascii); // native.cpp
}

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
constexpr const char* kDoc = R"({ "fuseui": 1, "root": { "class": "panel", "id": "p", "children": [
    { "class": "label", "id": "title", "text": "Wayland window" },
    { "class": "checkbox", "id": "c", "text": "Option" },
    { "class": "lineedit", "id": "e", "text": "Type" } ] } })";

void presentAtLeast(app::Application& a, app::AppWindow& w, u64 frames) {
    for (int i = 0; i < 400 && w.framesPresented() < frames; ++i) a.step(0.01f);
}
void settle(app::Application& a, int n = 20) {
    for (int i = 0; i < n; ++i) a.step(0.005f);
}

struct Weston {
    pid_t pid = -1;
    std::string dir, socket = "fuse-wl-gate";
    bool start(const std::string& exe, int scale) {
        char tmpl[] = "/tmp/fuse-wl-XXXXXX";
        if (!::mkdtemp(tmpl)) return false;
        dir = tmpl;
        ::chmod(dir.c_str(), 0700);
        // a rootless unpack (…/usr/bin/weston): its modules through WESTON_MODULE_MAP, its libraries on the path
        std::string root = exe.substr(0, exe.rfind("/usr/bin/"));
        std::string lib = root + "/usr/lib/x86_64-linux-gnu";
        const bool unpacked = exe.find("/usr/bin/") != std::string::npos && root != "" && ::access((lib + "/libweston-13.so.0").c_str(), R_OK) == 0;
        std::fflush(nullptr); // the child must not repeat buffered output
        pid = ::fork();
        if (pid < 0) return false;
        if (pid == 0) {
            ::setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
            ::unsetenv("WAYLAND_DISPLAY");
            ::unsetenv("DISPLAY");
            if (unpacked) {
                const std::string ld = lib + ":" + lib + "/weston:" + lib + "/libweston-13";
                ::setenv("LD_LIBRARY_PATH", ld.c_str(), 1);
                const std::string map = "headless-backend.so=" + lib + "/libweston-13/headless-backend.so;kiosk-shell.so=" + lib +
                                        "/weston/kiosk-shell.so;desktop-shell.so=" + lib + "/weston/desktop-shell.so";
                ::setenv("WESTON_MODULE_MAP", map.c_str(), 1);
            }
            const std::string sock = "--socket=" + socket, sc = "--scale=" + std::to_string(scale), log = "--log=" + dir + "/weston.log";
            std::freopen("/dev/null", "w", stdout);
            std::freopen("/dev/null", "w", stderr);
            ::execl(exe.c_str(), exe.c_str(), "--backend=headless", "--shell=kiosk", "--renderer=pixman", "--no-config", "--width=1024",
                    "--height=768", sc.c_str(), sock.c_str(), log.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        const std::string path = dir + "/" + socket;
        for (int i = 0; i < 300; ++i) { // up to 6 s for the socket
            struct stat st{};
            if (::stat(path.c_str(), &st) == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100)); // listening shortly after the bind
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
            for (int i = 0; i < 100 && ::waitpid(pid, &status, WNOHANG) != pid; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, WNOHANG);
            pid = -1;
        }
        if (!dir.empty()) {
            const std::string cmd = "rm -rf '" + dir + "'";
            [[maybe_unused]] int rc = std::system(cmd.c_str());
        }
    }
    ~Weston() { stop(); }
};

std::string westonPath() {
    if (const char* e = std::getenv("FUSE_TEST_WESTON"); e && *e) return e;
#ifdef FUSE_TEST_WESTON_PATH
    return FUSE_TEST_WESTON_PATH;
#else
    return {};
#endif
}

Vec2 centre(app::AppWindow& w, std::string_view id) {
    UiContext& ui = w.ui();
    const Rect r = ui.rect(ui.find(id));
    const f32 s = w.scale();
    return {(r.x + r.w * 0.5f) * s, (r.y + r.h * 0.5f) * s}; // device pixels
}

/// One run on the current compositor: software window, input, resize, close; `scale` the expected buffer scale.
void softwareRun(int scale) {
    app::Application a{{.name = "gate_wayland", .loadSettings = false}};
    app::NativePlatform native(a, {.vulkan = false});
    CHECK(native.available());
    if (!native.available()) {
        std::fprintf(stderr, "  no Wayland backend: %.*s\n", int(native.lastError().size()), native.lastError().data());
        return;
    }
    wl::Backend* b = wl::current();
    CHECK(b != nullptr);
    const auto disp = native.displays();
    CHECK(disp.size() == 1);
    if (!disp.empty()) {
        std::printf("  output %s %gx%g scale %g\n", disp[0].name.c_str(), double(disp[0].bounds.w), double(disp[0].bounds.h), double(disp[0].scale));
        CHECK(disp[0].bounds.w == 1024.f * f32(scale) || disp[0].bounds.w == 1024.f);
        CHECK(disp[0].scale == f32(scale));
    }
    auto& w = a.createWindow({.title = "fuse_app wayland gate", .width = 320, .height = 200});
    CHECK(w.loadText(kDoc));
    CHECK(native.show(w) == app::PresenterKind::Software);
    auto* win = reinterpret_cast<wl::Window*>(native.nativeWindow(w));
    CHECK(win != nullptr);
    if (!win) return;
    CHECK(wl::serialOf(win) >= 1); // the first configure was acknowledged
    presentAtLeast(a, w, 1);
    settle(a);
    CHECK(wl::bufferScale(win) == scale);
    CHECK(w.scale() == f32(scale));
    Image shown;
    CHECK(native.capture(w, shown));
    // the compositor may pick the size (kiosk-shell: the output's); the buffer is that logical size * scale
    const Size sz = w.size();
    std::printf("  configured size %gx%g\n", double(sz.w), double(sz.h));
    CHECK((sz.w == 320.f && sz.h == 200.f) || (sz.w == 1024.f && sz.h == 768.f));
    CHECK(shown.width == int(sz.w) * scale && shown.height == int(sz.h) * scale);
    const Image& ref = w.rasterize({});
    CHECK(shown.width == ref.width && shown.height == ref.height);
    if (shown.width == ref.width && shown.height == ref.height) {
        std::size_t differ = 0;
        for (std::size_t i = 0; i < ref.pixels.size(); ++i) differ += (shown.pixels[i] & 0xFFFFFFu) != (ref.pixels[i] & 0xFFFFFFu);
        std::printf("  scale %d: committed buffer %dx%d, %zu pixels differ from the CPU raster\n", scale, shown.width, shown.height, differ);
        CHECK(differ == 0);
    }
    // pointer: a click on the checkbox toggles it (left button through the backend's wl_pointer path)
    UiContext& ui = w.ui();
    const NodeHandle cb = ui.find("c");
    const bool before = ui.checked(cb);
    const unsigned long long h = native.nativeWindow(w);
    const Vec2 at = centre(w, "c");
    CHECK(native.postPointerMove(h, at));
    CHECK(native.postPointerButton(h, at, true));
    CHECK(native.postPointerButton(h, at, false));
    settle(a);
    CHECK(ui.checked(cb) != before);
    // keys: focus the line edit, type "a" (evdev KEY_A) through xkbcommon (or the US fallback)
    const NodeHandle e = ui.find("e");
    CHECK(native.postClick(h, centre(w, "e")));
    settle(a);
    const std::string t0(ui.text(e));
    wl::injectKey(b, win, 30);
    settle(a);
    const std::string t1(ui.text(e));
    std::printf("  line edit '%s' -> '%s'\n", t0.c_str(), t1.c_str());
    CHECK(t1.size() == t0.size() + 1 && t1.find('a') != std::string::npos);
    // client-side resize: the next committed buffer has the new size
    CHECK(native.resizeWindow(w, 400, 260));
    settle(a);
    CHECK(native.capture(w, shown));
    CHECK(shown.width == 400 * scale && shown.height == 260 * scale);
    // the compositor's close (xdg_toplevel.close) asks the window to close
    CHECK(native.postClose(h));
    CHECK(w.closing());
    CHECK(native.windowFocused(w) || true); // headless weston has no seat: focus is not asserted
}

void vulkanRun() {
    app::Application a{{.name = "gate_wayland_vk", .loadSettings = false}};
    app::NativePlatform native(a, {.asyncVulkan = false});
    CHECK(native.available());
    auto& w = a.createWindow({.title = "fuse_app wayland vulkan", .width = 256, .height = 160});
    CHECK(w.loadText(kDoc));
    const app::PresenterKind k = native.show(w);
    if (k != app::PresenterKind::Vulkan) {
        std::fprintf(stderr, "  no Vulkan presenter on the wayland surface: %.*s\n", int(native.lastError().size()), native.lastError().data());
        if (std::getenv("FUSE_TEST_WAYLAND_NO_VK")) return;
        CHECK(k == app::PresenterKind::Vulkan);
        return;
    }
    presentAtLeast(a, w, 3);
    settle(a);
    std::printf("  vulkan on %.*s (%s)\n", int(native.gpuName().size()), native.gpuName().data(), native.gpuDraws(w) ? "GPU draw" : "CPU upload");
    Image shown;
    CHECK(native.capture(w, shown));
    CHECK(shown.width == int(w.pixelSize().w) && shown.height == int(w.pixelSize().h));
    const Image& ref = w.rasterize({});
    if (shown.width == ref.width && shown.height == ref.height) {
        std::size_t far = 0;
        for (std::size_t i = 0; i < ref.pixels.size(); ++i) {
            int e = 0;
            for (int c = 0; c < 3; ++c) e = std::max(e, std::abs(int((shown.pixels[i] >> (8 * c)) & 0xFF) - int((ref.pixels[i] >> (8 * c)) & 0xFF)));
            far += e > 2;
        }
        std::printf("  vulkan frame: %zu pixels off the CPU raster by more than 2/255\n", far);
        CHECK(far * 200 < ref.pixels.size()); // < 0.5 %
    }
    CHECK(native.resizeWindow(w, 300, 180)); // swapchain follows the client-side size
    settle(a, 40);
    CHECK(native.capture(w, shown));
    CHECK(shown.width == 300 * int(w.scale()) && shown.height == 180 * int(w.scale()));
}

/// asyncVulkan (the default): software frames while a worker creates the device and the swapchain on the shared
/// wl_display (the WSI's own event queue), then the switch to Vulkan.
void asyncVulkanRun() {
    app::Application a{{.name = "gate_wayland_async", .loadSettings = false}};
    app::NativePlatform native(a);
    auto& w = a.createWindow({.title = "fuse_app wayland async", .width = 256, .height = 160});
    CHECK(w.loadText(kDoc));
    const app::PresenterKind first = native.show(w);
    presentAtLeast(a, w, 1);
    const app::PresenterKind k = native.waitForPresenter(w);
    std::printf("  async start: %s, then %s\n", first == app::PresenterKind::Software ? "software" : "vulkan",
                k == app::PresenterKind::Vulkan ? "vulkan" : "software");
    if (k != app::PresenterKind::Vulkan && std::getenv("FUSE_TEST_WAYLAND_NO_VK")) return;
    CHECK(k == app::PresenterKind::Vulkan);
    const u64 before = w.framesPresented();
    presentAtLeast(a, w, before + 1);
    settle(a);
    Image shown;
    CHECK(native.capture(w, shown));
    CHECK(shown.width == int(w.pixelSize().w) && shown.height == int(w.pixelSize().h));
}
} // namespace

int main() {
    const std::string self = wl::selfTest(&app::x11::mapKey);
    std::printf("fuse_app_wayland: library / protocol / keymap checks: %s\n", self.empty() ? "pass" : self.c_str());
    if (!self.empty()) return 1;
    ::setenv("FUSE_APP_BACKEND", "wayland", 1);
    ::unsetenv("FUSE_UI_SCALE");
    if (const char* e = std::getenv("FUSE_TEST_WAYLAND_EXISTING"); e && *e == '1') {
        std::printf("fuse_app_wayland: the session compositor ($WAYLAND_DISPLAY)\n");
        softwareRun(1);
        vulkanRun();
        asyncVulkanRun();
        std::printf("fuse_app_wayland: %s\n", g_fail ? "FAIL" : "pass");
        return g_fail ? 1 : 0;
    }
    const std::string exe = westonPath();
    if (exe.empty() || ::access(exe.c_str(), X_OK) != 0) {
        std::printf("fuse_app_wayland: skipped the compositor part (no weston; set FUSE_TEST_WESTON)\n");
        return 77;
    }
    for (int scale : {1, 2}) {
        Weston ws;
        if (!ws.start(exe, scale)) {
            std::printf("fuse_app_wayland: skipped (weston %s did not start)\n", exe.c_str());
            return g_fail ? 1 : 77;
        }
        ws.use();
        std::printf("fuse_app_wayland: headless weston, scale %d\n", scale);
        softwareRun(scale);
        if (scale == 1) {
            vulkanRun();
            asyncVulkanRun();
        }
    }
    std::printf("fuse_app_wayland: %s\n", g_fail ? "FAIL" : "pass");
    return g_fail ? 1 : 0;
}
