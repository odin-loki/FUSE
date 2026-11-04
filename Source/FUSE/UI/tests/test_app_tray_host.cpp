// Tray icon under a real StatusNotifierItem host (plan UI10, §4.13 Shell): the gate starts xfce4-panel with only its
// status tray plugin (the panel is the org.kde.StatusNotifierWatcher and the SNI host) on a private session bus under
// Xvfb, then
//   * TrayIcon::supported() sees the watcher; the created icon is listed in RegisteredStatusNotifierItems
//   * the panel shows it: its tray grows by the icon button
//   * a real left click (XTest) on the icon makes the panel call Activate -> onClick fires
//   * a real right click opens the panel's menu built from our com.canonical.dbusmenu export; keyboard Down Down
//     Return (XTest) picks the second item -> the menu callback gets index 1 "Settings"
//   * removing the icon unregisters it (the watcher drops it)
// The panel comes from $FUSE_TEST_SNI_HOST, else the one found at configure time (also in ~/opt/<pkg>/usr/bin for a
// package unpacked without root: then its compiled-in plugin directory must reach that prefix). xfconfd (the panel's
// settings daemon) is started next to it; the panel's configuration is a temporary XDG_CONFIG_HOME. Exit 77 when no
// panel / xfconfd / XTest / private bus is available.
#include "fuse/app/native.hpp"
#include "fuse/app/shell.hpp"
#include "dbus_internal.hpp"
#include "x11_internal.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

using namespace fuse::ui;
namespace app = fuse::app;
namespace x11 = fuse::app::x11;
namespace dbus = fuse::app::dbus;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
int skip(std::string_view why) {
    std::printf("fuse_app_tray_host: skipped (%.*s)\n", int(why.size()), why.data());
    return 77;
}
bool exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

struct Child {
    pid_t pid = 0;
    ~Child() { stop(); }
    bool start(const std::string& exe, std::vector<std::string> args, std::vector<std::string>& env) {
        args.insert(args.begin(), exe);
        std::vector<char*> argv, envp;
        for (std::string& s : args) argv.push_back(s.data());
        argv.push_back(nullptr);
        for (std::string& s : env) envp.push_back(s.data());
        envp.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        const int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&fa);
        if (rc != 0) pid = 0;
        return pid > 0;
    }
    void stop() {
        if (pid <= 0) return;
        ::kill(pid, SIGTERM);
        for (int i = 0; i < 100 && ::waitpid(pid, nullptr, WNOHANG) == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, WNOHANG);
        pid = 0;
    }
};

std::string findPanel() {
    if (const char* e = std::getenv("FUSE_TEST_SNI_HOST"); e && *e) return e;
#ifdef FUSE_TEST_SNI_HOST_PATH
    if (exists(FUSE_TEST_SNI_HOST_PATH)) return FUSE_TEST_SNI_HOST_PATH;
#endif
    return exists("/usr/bin/xfce4-panel") ? "/usr/bin/xfce4-panel" : "";
}

constexpr const char* kPanelXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfce4-panel" version="1.0">
  <property name="configver" type="int" value="2"/>
  <property name="panels" type="array">
    <value type="int" value="1"/>
    <property name="panel-1" type="empty">
      <property name="position" type="string" value="p=6;x=0;y=0"/>
      <property name="length" type="uint" value="1"/>
      <property name="length-adjust" type="bool" value="true"/>
      <property name="position-locked" type="bool" value="true"/>
      <property name="size" type="uint" value="32"/>
      <property name="plugin-ids" type="array">
        <value type="int" value="1"/>
      </property>
    </property>
  </property>
  <property name="plugins" type="empty">
    <property name="plugin-1" type="string" value="systray"/>
  </property>
</channel>
)";

/// The gate's bus connection: what an SNI-aware tool would query.
struct Bus {
    dbus::Lib& d = dbus::lib();
    dbus::Connection* c = nullptr;
    ~Bus() {
        if (c) { d.connClose(c); d.connUnref(c); }
    }
    bool open() {
        if (!d.load()) return false;
        dbus::Error e{};
        d.errorInit(&e);
        c = d.busGetPrivate(dbus::kBusSession, &e);
        if (e.name) d.errorFree(&e);
        if (c) d.exitOnDisconnect(c, 0);
        return c != nullptr;
    }
    /// Properties.Get(org.kde.StatusNotifierWatcher, prop) -> reader callback on the variant
    bool watcherProp(const char* prop, const std::function<void(dbus::Reader&)>& read) {
        dbus::Message* m = d.newCall("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.freedesktop.DBus.Properties", "Get");
        dbus::Writer(d, m).str("org.kde.StatusNotifierWatcher").str(prop);
        dbus::Error e{};
        d.errorInit(&e);
        dbus::Message* r = d.sendBlocking(c, m, 3000, &e);
        d.unref(m);
        if (e.name) d.errorFree(&e);
        if (!r) return false;
        dbus::Reader rd(d, r);
        dbus::Reader v = rd.sub();
        read(v);
        d.unref(r);
        return true;
    }
    bool hostRegistered() {
        bool on = false;
        watcherProp("IsStatusNotifierHostRegistered", [&](dbus::Reader& v) { on = v.integer() != 0; });
        return on;
    }
    std::vector<std::string> items() {
        std::vector<std::string> out;
        watcherProp("RegisteredStatusNotifierItems", [&](dbus::Reader& v) {
            dbus::Reader a = v.sub();
            for (; a.type() == dbus::TString; a.next()) out.push_back(a.str());
        });
        return out;
    }
};

struct Desk {
    x11::Lib x;
    x11::Display* d = nullptr;
    x11::Window root = 0;
    void* xtst = nullptr;
    int (*fakeMotion)(x11::Display*, int, int, int, unsigned long) = nullptr;
    int (*fakeButton)(x11::Display*, unsigned, int, unsigned long) = nullptr;
    int (*fakeKey)(x11::Display*, unsigned, int, unsigned long) = nullptr;
    unsigned char (*keycode)(x11::Display*, x11::KeySym) = nullptr;
    int (*queryTree)(x11::Display*, x11::Window, x11::Window*, x11::Window*, x11::Window**, unsigned*) = nullptr;
    int (*geometry)(x11::Display*, x11::XID, x11::Window*, int*, int*, unsigned*, unsigned*, unsigned*, unsigned*) = nullptr;
    ~Desk() {
        if (d) x.CloseDisplay(d);
    }
    bool open() {
        if (!x.load() || !(d = x.OpenDisplay(nullptr))) return false;
        root = x.RootWindow(d, x.DefaultScreen(d));
        queryTree = reinterpret_cast<decltype(queryTree)>(dlsym(x.so, "XQueryTree"));
        geometry = reinterpret_cast<decltype(geometry)>(dlsym(x.so, "XGetGeometry"));
        keycode = reinterpret_cast<decltype(keycode)>(dlsym(x.so, "XKeysymToKeycode"));
        if ((xtst = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL))) {
            fakeMotion = reinterpret_cast<decltype(fakeMotion)>(dlsym(xtst, "XTestFakeMotionEvent"));
            fakeButton = reinterpret_cast<decltype(fakeButton)>(dlsym(xtst, "XTestFakeButtonEvent"));
            fakeKey = reinterpret_cast<decltype(fakeKey)>(dlsym(xtst, "XTestFakeKeyEvent"));
        }
        return fakeMotion && fakeButton && fakeKey && queryTree && geometry && keycode;
    }
    /// The panel's toplevel at the screen origin (WM_CLASS xfce4-panel), widest one: x, y, w, h
    bool panelRect(Rect& out) {
        x11::Window r = 0, p = 0, *kids = nullptr;
        unsigned n = 0;
        bool found = false;
        if (!queryTree(d, root, &r, &p, &kids, &n)) return false;
        const x11::Atom wmClass = x.InternAtom(d, "WM_CLASS", 0);
        for (unsigned i = 0; i < n; ++i) {
            std::vector<unsigned char> cls;
            if (!x.property(d, kids[i], wmClass, x11::kXA_STRING, cls) || std::strncmp(reinterpret_cast<const char*>(cls.data()), "xfce4-panel", 11) != 0)
                continue;
            x11::Window gr = 0;
            int gx = 0, gy = 0;
            unsigned gw = 0, gh = 0, bw = 0, depth = 0;
            if (!geometry(d, kids[i], &gr, &gx, &gy, &gw, &gh, &bw, &depth) || gx != 0 || gy != 0 || gh < 20) continue;
            if (!found || f32(gw) > out.w) out = {f32(gx), f32(gy), f32(gw), f32(gh)};
            found = true;
        }
        if (kids) x.Free(kids);
        return found;
    }
    void click(Vec2 p, unsigned button) {
        fakeMotion(d, -1, int(p.x), int(p.y), 0);
        fakeButton(d, button, 1, 0);
        fakeButton(d, button, 0, 30);
        x.Sync(d, 0);
    }
    void key(x11::KeySym sym) {
        const unsigned kc = keycode(d, sym);
        fakeKey(d, kc, 1, 0);
        fakeKey(d, kc, 0, 20);
        x.Sync(d, 0);
    }
};

bool waitFor(app::Application& a, const std::function<bool()>& pred, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred() && std::chrono::steady_clock::now() < end) a.step(0.01f);
    return pred();
}
} // namespace

int main(int argc, char** argv) {
    if (!std::getenv("FUSE_TEST_PRIVATE_BUS")) return skip("needs a private session bus (dbus-run-session, FUSE_TEST_PRIVATE_BUS=1)");
    const std::string panel = findPanel();
    if (panel.empty()) return skip("no xfce4-panel (set FUSE_TEST_SNI_HOST)");
    const std::string usr = panel.substr(0, panel.rfind('/', panel.rfind('/') - 1));
    std::string xfconfd;
    for (const std::string& c : {usr + "/lib/x86_64-linux-gnu/xfce4/xfconf/xfconfd", std::string("/usr/lib/x86_64-linux-gnu/xfce4/xfconf/xfconfd"),
                                 usr + "/lib/xfce4/xfconf/xfconfd"})
        if (xfconfd.empty() && exists(c)) xfconfd = c;
    if (xfconfd.empty()) return skip("xfconfd not found next to " + panel);
    Desk desk;
    if (!desk.open()) return skip("no X display or XTest");
    Bus bus;
    if (!bus.open()) return skip("no session bus");

    // the panel's environment: X11 GTK, a throwaway configuration with one panel holding the status tray
    char cfg[] = "/tmp/fuse_tray_host_XXXXXX";
    if (!::mkdtemp(cfg)) return skip("mkdtemp failed");
    const std::string cfgDir = std::string(cfg) + "/xfce4/xfconf/xfce-perchannel-xml";
    for (const std::string& d : {std::string(cfg) + "/xfce4", std::string(cfg) + "/xfce4/xfconf", cfgDir}) ::mkdir(d.c_str(), 0700);
    if (FILE* f = std::fopen((cfgDir + "/xfce4-panel.xml").c_str(), "w")) {
        std::fputs(kPanelXml, f);
        std::fclose(f);
    }
    const bool privatePrefix = usr != "/usr" && exists(usr + "/lib/x86_64-linux-gnu");
    std::vector<std::string> env;
    for (char** e = environ; *e; ++e) {
        const std::string kv = *e;
        if (kv.rfind("WAYLAND_DISPLAY=", 0) == 0 || kv.rfind("XDG_CONFIG_HOME=", 0) == 0 || kv.rfind("GDK_BACKEND=", 0) == 0 ||
            (privatePrefix && (kv.rfind("LD_LIBRARY_PATH=", 0) == 0 || kv.rfind("XDG_DATA_DIRS=", 0) == 0)))
            continue;
        env.push_back(kv);
    }
    env.push_back("GDK_BACKEND=x11");
    env.push_back(std::string("XDG_CONFIG_HOME=") + cfg);
    if (privatePrefix) {
        env.push_back("LD_LIBRARY_PATH=" + usr + "/lib/x86_64-linux-gnu");
        env.push_back("XDG_DATA_DIRS=" + usr + "/share:/usr/share");
    }
    Child conf, host;
    if (!conf.start(xfconfd, {}, env)) return skip("cannot start " + xfconfd);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (!host.start(panel, {"--disable-wm-check"}, env)) return skip("cannot start " + panel);

    app::Application a{{.name = "gate_tray_host", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform np(a, {.vulkan = false}); // its event pump dispatches the icon's D-Bus traffic
    if (!np.available()) return skip(np.lastError());
    auto& win = a.createWindow({.title = "fuse tray host gate", .width = 200, .height = 100}); // keeps the app running
    np.show(win);
    waitFor(a, [&] { return win.framesPresented() >= 1; }, 3000);
    desk.x.MoveResizeWindow(desk.d, x11::Window(np.nativeWindow(win)), 600, 400, 200, 100); // away from the panel (no WM)
    desk.x.Sync(desk.d, 0);
    if (!waitFor(a, [&] { return bus.hostRegistered(); }, 15000)) return skip(panel + " did not register a StatusNotifierItem host");
    Rect before{};
    waitFor(a, [&] { return desk.panelRect(before); }, 5000);
    std::printf("  host: %s (panel %.0fx%.0f at %.0f,%.0f)\n", panel.c_str(), before.w, before.h, before.x, before.y);
    CHECK(app::TrayIcon::supported());

    fuse::ui::Image icon;
    icon.resize(32, 32, 0xFF3080E0u);
    for (int y = 8; y < 24; ++y)
        for (int x = 8; x < 24; ++x) icon.pixels[std::size_t(y) * 32 + std::size_t(x)] = 0xFFFFFFFFu;
    app::TrayIcon tray;
    int clicks = 0;
    int picked = -1;
    std::string pickedText;
    tray.onClick([&] { ++clicks; });
    tray.setMenu("Open\nSettings\n-\nQuit", [&](unsigned i, std::string_view t) { picked = int(i); pickedText = std::string(t); });
    CHECK(tray.create(icon, "FUSE tray host gate"));
    CHECK(tray.shown());

    // registered with the panel's watcher, and the panel shows a button for it
    CHECK(waitFor(a, [&] {
        for (const std::string& s : bus.items())
            if (s.find("org.kde.StatusNotifierItem-" + std::to_string(::getpid())) != std::string::npos) return true;
        return false;
    }, 5000));
    Rect shown{};
    CHECK(waitFor(a, [&] { return desk.panelRect(shown) && shown.w > before.w; }, 5000));
    std::printf("  registered: %zu item(s); panel %.0fx%.0f -> %.0fx%.0f\n", bus.items().size(), before.w, before.h, shown.w, shown.h);
    for (int i = 0; i < 30; ++i) a.step(0.01f);

    // left click on the icon: the panel activates the item
    const Vec2 iconAt{shown.x + shown.w - shown.h * 0.5f, shown.y + shown.h * 0.5f}; // the newest button, at the end
    desk.click(iconAt, 1);
    CHECK(waitFor(a, [&] { return clicks == 1; }, 3000));
    std::printf("  left click: %d activation(s)\n", clicks);
    // right click: the panel's menu from our dbusmenu; Down Down Return picks "Settings"
    desk.click(iconAt, 3);
    for (int i = 0; i < 60; ++i) a.step(0.01f); // the panel fetches the layout and pops the menu up
    desk.key(0xff54 /*Down*/);
    for (int i = 0; i < 10; ++i) a.step(0.01f);
    desk.key(0xff54);
    for (int i = 0; i < 10; ++i) a.step(0.01f);
    desk.key(0xff0d /*Return*/);
    CHECK(waitFor(a, [&] { return picked >= 0; }, 3000));
    std::printf("  menu: picked %d \"%s\"\n", picked, pickedText.c_str());
    CHECK(picked == 1 && pickedText == "Settings");

    // removing the icon unregisters it
    tray.remove();
    CHECK(waitFor(a, [&] {
        for (const std::string& s : bus.items())
            if (s.find("org.kde.StatusNotifierItem-" + std::to_string(::getpid())) != std::string::npos) return false;
        return true;
    }, 5000));
    host.stop();
    conf.stop();
    for (const std::string& f : {cfgDir + "/xfce4-panel.xml"}) ::unlink(f.c_str());
    // xfconfd may have written more; leave the throwaway directory to the system's /tmp cleanup when it is not empty
    for (const std::string& d : {cfgDir, std::string(cfg) + "/xfce4/xfconf", std::string(cfg) + "/xfce4", std::string(cfg)}) ::rmdir(d.c_str());
    if (g_fail) {
        std::printf("fuse_app_tray_host: FAILED (%d checks)\n", g_fail);
        return 1;
    }
    std::printf("fuse_app_tray_host: OK\n");
    return 0;
}
