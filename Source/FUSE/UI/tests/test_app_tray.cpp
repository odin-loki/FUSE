// Tray icon + notification gate (plan UI10, §4.13 Shell). Needs no human:
//   * adds a tray icon from an RGBA raster; the shell accepts it (Shell_NotifyIconGetRect finds it, or NIM_MODIFY of
//     the tooltip / icon succeeds when Windows 11 keeps it in the overflow)
//   * a simulated shell callback message (NIN_SELECT, WM_LBUTTONDBLCLK) fires the click / double-click handlers
//   * the context menu built from popupMenu-format items opens as a real OS popup menu (TrackPopupMenu) on a
//     simulated WM_CONTEXTMENU; keyboard input posted to it picks the second item and the menu handler gets it
//   * a notification (NIF_INFO balloon / toast) is accepted, then the icon is removed
// Unix (StatusNotifierItem over D-Bus): with FUSE_TEST_PRIVATE_BUS set (ctest runs it under dbus-run-session) the gate
// plays the desktop on that private session bus: a StatusNotifierWatcher and a notification daemon on a thread, and a
// tray host on its own connection that reads the item's properties (icon pixmap, tooltip), sees NewToolTip / NewIcon,
// activates it, walks the exported dbusmenu (labels, separator, disabled item) and clicks "Settings"; the icon's
// notification is shown and its ActionInvoked "default" fires onNotificationClick; notify() without a tray works;
// removing the icon releases its bus name; without a daemon notify() reports Unsupported. Callbacks are dispatched by
// NativePlatform's event pump (Xvfb).
// Exit 77 where there is no tray (a session without a shell / StatusNotifierWatcher).
#include "fuse/app/shell.hpp"

#include <cstdio>
#include <string>

#if !defined(_WIN32)
#include "dbus_internal.hpp"
#include "fuse/app/native.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

using namespace fuse::ui;
namespace app = fuse::app;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

#if defined(_WIN32)
namespace {
bool g_menuSeen = false;
// Runs inside TrackPopupMenu's modal loop (a thread timer): the menu window (#32768) is up; pick item 2 by keyboard.
void CALLBACK menuTimer(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    HWND menu = FindWindowW(L"#32768", nullptr);
    g_menuSeen = menu && IsWindowVisible(menu);
    if (!menu) { EndMenu(); return; }
    PostMessageW(menu, WM_KEYDOWN, VK_DOWN, 0);
    PostMessageW(menu, WM_KEYDOWN, VK_DOWN, 0);
    PostMessageW(menu, WM_KEYDOWN, VK_RETURN, 0);
}
void CALLBACK menuWatchdog(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    EndMenu(); // never leave a menu open if the keys did not land
}
} // namespace
#endif

#if !defined(_WIN32)
namespace {
namespace dbus = fuse::app::dbus;
/// StatusNotifierWatcher + notification daemon on their own connection and thread.
struct FakeDesktop {
    dbus::Lib& d = dbus::lib();
    dbus::Connection* c = nullptr;
    std::thread t;
    std::atomic<bool> stop{false}, ready{false};
    std::mutex mu;
    std::vector<std::string> registered;
    struct Note { std::string app, summary, body; bool action = false; std::uint32_t id = 0; };
    std::vector<Note> notes;

    bool start() {
        dbus::Error err{};
        d.errorInit(&err);
        c = d.busGetPrivate(dbus::kBusSession, &err);
        d.errorFree(&err);
        if (!c) return false;
        d.exitOnDisconnect(c, 0);
        for (const char* n : {"org.kde.StatusNotifierWatcher", "org.freedesktop.Notifications"}) {
            d.errorInit(&err);
            const int r = d.busRequestName(c, n, dbus::kNameFlagDoNotQueue, &err);
            d.errorFree(&err);
            if (r != 1) return false;
        }
        t = std::thread([this] { loop(); });
        return true;
    }
    void shutdown() {
        stop = true;
        if (t.joinable()) t.join();
        if (c) {
            d.connClose(c);
            d.connUnref(c);
            c = nullptr;
        }
    }
    void loop() {
        std::uint32_t nextId = 41;
        while (!stop) {
            d.readWrite(c, 20);
            while (dbus::Message* m = d.popMessage(c)) {
                if (d.msgType(m) == dbus::MethodCall) {
                    const std::string member = d.msgMember(m) ? d.msgMember(m) : "";
                    dbus::Message* reply = d.newReturn(m);
                    dbus::Reader r(d, m);
                    std::uint32_t emitAction = 0;
                    if (member == "RegisterStatusNotifierItem") {
                        std::lock_guard<std::mutex> l(mu);
                        registered.push_back(r.str());
                    } else if (member == "Notify") {
                        Note n;
                        n.app = r.str(); r.next(); r.next(); r.next();
                        n.summary = r.str(); r.next();
                        n.body = r.str(); r.next();
                        for (dbus::Reader a = r.sub(); a.type() == dbus::TString; a.next()) n.action |= a.str() == "default";
                        n.id = nextId++;
                        dbus::Writer(d, reply).u32(n.id);
                        if (n.action) emitAction = n.id;
                        std::lock_guard<std::mutex> l(mu);
                        notes.push_back(n);
                    } else if (member == "Get") { // watcher properties
                        dbus::Writer(d, reply).variant("b", [](dbus::Writer& v) { v.boolean(true); });
                    }
                    d.connSend(c, reply, nullptr);
                    d.unref(reply);
                    if (emitAction) { // the user clicks the notification
                        dbus::Message* s = d.newSignal("/org/freedesktop/Notifications", "org.freedesktop.Notifications", "ActionInvoked");
                        dbus::Writer(d, s).u32(emitAction).str("default");
                        d.connSend(c, s, nullptr);
                        d.unref(s);
                    }
                    d.connFlush(c);
                }
                d.unref(m);
            }
        }
    }
};
/// The tray host (panel) side: calls into the item without blocking the UI thread (the pump answers them).
struct Host {
    dbus::Lib& d = dbus::lib();
    dbus::Connection* c = nullptr;
    std::vector<std::string> signals;
    bool open() {
        dbus::Error err{};
        d.errorInit(&err);
        c = d.busGetPrivate(dbus::kBusSession, &err);
        d.errorFree(&err);
        if (!c) return false;
        d.exitOnDisconnect(c, 0);
        d.errorInit(&err);
        d.busAddMatch(c, "type='signal',interface='org.kde.StatusNotifierItem'", &err);
        d.busAddMatch(c, "type='signal',interface='com.canonical.dbusmenu'", &err);
        d.errorFree(&err);
        return true;
    }
    ~Host() {
        if (c) {
            d.connClose(c);
            d.connUnref(c);
        }
    }
    void drain(app::Application& a, int steps) {
        for (int i = 0; i < steps; ++i) {
            a.step(0.005f);
            d.readWrite(c, 1);
            while (dbus::Message* m = d.popMessage(c)) {
                if (d.msgType(m) == dbus::Signal && d.msgMember(m)) signals.push_back(d.msgMember(m));
                d.unref(m);
            }
        }
    }
    /// Sends a call and pumps the app until its reply (or 2 s). Caller unrefs the reply.
    template <class F>
    dbus::Message* call(app::Application& a, const std::string& dest, const char* path, const char* iface, const char* member, F&& args) {
        dbus::Message* m = d.newCall(dest.c_str(), path, iface, member);
        {
            dbus::Writer w(d, m);
            args(w);
        }
        std::uint32_t serial = 0;
        d.connSend(c, m, &serial);
        d.unref(m);
        d.connFlush(c);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < end) {
            a.step(0.002f);
            d.readWrite(c, 1);
            while (dbus::Message* r = d.popMessage(c)) {
                if ((d.msgType(r) == dbus::MethodReturn || d.msgType(r) == dbus::ErrorReply) && d.msgReplySerial(r) == serial) {
                    if (d.msgType(r) == dbus::ErrorReply) {
                        std::fprintf(stderr, "  %s: error %s\n", member, d.msgErrorName(r) ? d.msgErrorName(r) : "?");
                        d.unref(r);
                        return nullptr;
                    }
                    return r;
                }
                if (d.msgType(r) == dbus::Signal && d.msgMember(r)) signals.push_back(d.msgMember(r));
                d.unref(r);
            }
        }
        return nullptr;
    }
    dbus::Message* get(app::Application& a, const std::string& dest, const char* path, const char* iface, const char* prop) {
        return call(a, dest, path, "org.freedesktop.DBus.Properties", "Get", [&](dbus::Writer& w) { w.str(iface).str(prop); });
    }
    bool hasOwner(const std::string& name) {
        dbus::Error err{};
        d.errorInit(&err);
        const bool r = d.busNameHasOwner(c, name.c_str(), &err) != 0;
        d.errorFree(&err);
        return r;
    }
};
std::string propsLabel(dbus::Reader props, bool& enabled, std::string& type) { // a{sv}
    std::string label;
    enabled = true;
    for (; props.type() == dbus::TDictEntry; props.next()) {
        dbus::Reader e = props.sub();
        const std::string k = e.str();
        e.next();
        dbus::Reader v = e.sub();
        if (k == "label") label = v.str();
        else if (k == "enabled") enabled = v.integer() != 0;
        else if (k == "type") type = v.str();
    }
    return label;
}

int unixGate() {
    if (!std::getenv("FUSE_TEST_PRIVATE_BUS")) {
        std::printf("fuse_app_tray: skipped (needs a private session bus: run under dbus-run-session with FUSE_TEST_PRIVATE_BUS=1)\n");
        return 77;
    }
    dbus::Lib& d = dbus::lib();
    if (!d.load()) {
        std::printf("fuse_app_tray: skipped (libdbus-1.so.3 not found)\n");
        return 77;
    }
    FakeDesktop desk;
    if (!desk.start()) {
        std::printf("fuse_app_tray: skipped (no session bus / names taken)\n");
        return 77;
    }
    CHECK(app::TrayIcon::supported());
    app::Application a{{.name = "gate_tray", .loadSettings = false}};
    app::NativePlatform np(a); // its pump dispatches the tray's D-Bus connection
    auto& win = a.createWindow({.title = "fuse tray gate", .width = 200, .height = 100}); // keeps the app running
    np.show(win);
    Host host;
    CHECK(host.open());

    Image icon;
    icon.resize(32, 32, 0xFF000000u);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) icon.pixels[std::size_t(y * 32 + x)] = (x - 16) * (x - 16) + (y - 16) * (y - 16) < 196 ? 0xFF30A0FFu : 0x00000000u;
    app::TrayIcon tray;
    int clicks = 0, noteClicks = 0;
    unsigned picked = ~0u;
    std::string pickedText;
    tray.onClick([&] { ++clicks; });
    tray.onNotificationClick([&] { ++noteClicks; });
    tray.setMenu("Open\nSettings\n-\n!Disabled\nQuit", [&](unsigned i, std::string_view t) { picked = i; pickedText = t; });
    CHECK(tray.menuItemCount() == 4);
    CHECK(tray.create(icon, "fuse_app_tray gate"));
    CHECK(tray.shown());
    std::string service;
    {
        std::lock_guard<std::mutex> l(desk.mu);
        if (!desk.registered.empty()) service = desk.registered.back();
    }
    std::printf("  registered with the watcher: %s\n", service.c_str());
    CHECK(service.starts_with("org.kde.StatusNotifierItem-") && host.hasOwner(service));
    if (service.empty()) { desk.shutdown(); return 1; }

    // the host reads the icon (a(iiay) ARGB32, network order) and the tooltip
    int iw = 0, ih = 0;
    std::vector<unsigned char> px;
    if (dbus::Message* r = host.get(a, service, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "IconPixmap")) {
        dbus::Reader rd(d, r);
        dbus::Reader arr = rd.sub().sub(); // variant -> array -> first struct
        if (arr.type() == dbus::TStruct) {
            dbus::Reader s = arr.sub();
            iw = int(s.integer()); s.next();
            ih = int(s.integer()); s.next();
            for (dbus::Reader b = s.sub(); b.type() == dbus::TByte; b.next()) px.push_back(u8(b.integer()));
        }
        d.unref(r);
    }
    const std::size_t mid = (16u * 32u + 16u) * 4u;
    std::printf("  IconPixmap: %dx%d, %zu bytes, centre ARGB %02x %02x %02x %02x\n", iw, ih, px.size(), px.size() > mid + 3 ? px[mid] : 0,
                px.size() > mid + 3 ? px[mid + 1] : 0, px.size() > mid + 3 ? px[mid + 2] : 0, px.size() > mid + 3 ? px[mid + 3] : 0);
    CHECK(iw == 32 && ih == 32 && px.size() == 32u * 32u * 4u);
    CHECK(px.size() > mid + 3 && px[mid] == 0xFF && px[mid + 1] == 0xFF && px[mid + 2] == 0xA0 && px[mid + 3] == 0x30);
    CHECK(px.size() > 3 && px[0] == 0); // transparent corner
    CHECK(tray.setTooltip("fuse_app_tray gate (modified)") && tray.setIcon(icon));
    host.drain(a, 20);
    std::string tipTitle;
    if (dbus::Message* r = host.get(a, service, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "ToolTip")) {
        dbus::Reader rd(d, r);
        dbus::Reader s = rd.sub().sub(); // variant -> (sa(iiay)ss)
        s.next();
        s.next();
        tipTitle = s.str();
        d.unref(r);
    }
    auto seen = [&](const char* sig) { return std::find(host.signals.begin(), host.signals.end(), sig) != host.signals.end(); };
    std::printf("  tooltip \"%s\", signals NewToolTip %d NewIcon %d\n", tipTitle.c_str(), seen("NewToolTip"), seen("NewIcon"));
    CHECK(tipTitle == "fuse_app_tray gate (modified)" && seen("NewToolTip") && seen("NewIcon"));

    // left click: Activate
    if (dbus::Message* r = host.call(a, service, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "Activate",
                                     [](dbus::Writer& w) { w.i32(100).i32(100); }))
        d.unref(r);
    host.drain(a, 5);
    CHECK(clicks == 1);

    // the menu: Menu property -> dbusmenu layout
    std::string menuPath;
    if (dbus::Message* r = host.get(a, service, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "Menu")) {
        dbus::Reader rd(d, r);
        menuPath = rd.sub().str();
        d.unref(r);
    }
    CHECK(menuPath.starts_with("/"));
    if (!menuPath.starts_with("/")) menuPath = "/MenuBar";
    std::vector<std::pair<int, std::string>> kids;
    std::string kinds;
    if (dbus::Message* r = host.call(a, service, menuPath.c_str(), "com.canonical.dbusmenu", "GetLayout", [](dbus::Writer& w) {
            w.i32(0).i32(-1).container(dbus::TArray, "s", [](dbus::Writer&) {});
        })) {
        dbus::Reader rd(d, r);
        rd.next(); // revision
        dbus::Reader root = rd.sub();
        root.next();
        root.next(); // id, props
        for (dbus::Reader ch = root.sub(); ch.type() == dbus::TVariant; ch.next()) {
            dbus::Reader item = ch.sub().sub();
            const int id = int(item.integer());
            item.next();
            bool enabled = true;
            std::string type;
            const std::string label = propsLabel(item.sub(), enabled, type);
            kids.push_back({id, label});
            kinds += type == "separator" ? '-' : enabled ? 'E' : 'D';
        }
        d.unref(r);
    }
    std::printf("  dbusmenu %s: %zu entries (%s):", menuPath.c_str(), kids.size(), kinds.c_str());
    for (const auto& [id, l] : kids) std::printf(" %d:\"%s\"", id, l.c_str());
    std::printf("\n");
    CHECK(kids.size() == 5 && kinds == "EE-DE");
    CHECK(kids.size() == 5 && kids[1].second == "Settings" && kids[3].second == "Disabled" && kids[4].second == "Quit");
    auto clickItem = [&](int id) {
        if (dbus::Message* r = host.call(a, service, menuPath.c_str(), "com.canonical.dbusmenu", "Event", [&](dbus::Writer& w) {
                w.i32(id).str("clicked").variant("i", [](dbus::Writer& v) { v.i32(0); }).u32(0);
            }))
            d.unref(r);
        host.drain(a, 5);
    };
    if (kids.size() == 5) {
        clickItem(kids[3].first); // disabled: nothing
        CHECK(picked == ~0u);
        clickItem(kids[1].first);
    }
    std::printf("  menu click: picked %d \"%s\"\n", int(picked), pickedText.c_str());
    CHECK(picked == 1 && pickedText == "Settings");
    // libdbusmenu hosts (GTK panels) send clicks as EventGroup a(isvu)
    if (kids.size() == 5) {
        picked = ~0u;
        if (dbus::Message* r = host.call(a, service, menuPath.c_str(), "com.canonical.dbusmenu", "EventGroup", [&](dbus::Writer& w) {
                w.container(dbus::TArray, "(isvu)", [&](dbus::Writer& arr) {
                    arr.container(dbus::TStruct, nullptr, [&](dbus::Writer& e) {
                        e.i32(kids[4].first).str("clicked").variant("i", [](dbus::Writer& v) { v.i32(0); }).u32(0);
                    });
                });
            }))
            d.unref(r);
        host.drain(a, 5);
        std::printf("  EventGroup click: picked %d \"%s\"\n", int(picked), pickedText.c_str());
        CHECK(picked == 3 && pickedText == "Quit"); // separators are not counted, the disabled item is
    }
    // setMenu again announces a new layout
    tray.setMenu("Only", [&](unsigned i, std::string_view t) { picked = i + 100; pickedText = t; });
    host.drain(a, 20);
    CHECK(seen("LayoutUpdated") && tray.menuItemCount() == 1);

    // notifications: the icon's carries a default action (clicked -> onNotificationClick); a plain one too
    const app::NotifyResult n = tray.notify("FUSE UI", "fuse_app_tray gate notification");
    for (int i = 0; i < 200 && !noteClicks; ++i) a.step(0.005f);
    const app::NotifyResult n2 = app::notify("FUSE UI", "plain notification", "fuse_app_tray");
    std::size_t noteCount = 0;
    bool firstHasAction = false, secondHasAction = true;
    std::string secondApp;
    {
        std::lock_guard<std::mutex> l(desk.mu);
        noteCount = desk.notes.size();
        if (noteCount >= 2) {
            firstHasAction = desk.notes[0].action;
            secondHasAction = desk.notes[1].action;
            secondApp = desk.notes[1].app;
        }
    }
    std::printf("  notifications: icon %s (clicked %d), plain %s (app \"%s\"), daemon saw %zu\n",
                n == app::NotifyResult::Shown ? "shown" : "not shown", noteClicks, n2 == app::NotifyResult::Shown ? "shown" : "not shown",
                secondApp.c_str(), noteCount);
    CHECK(n == app::NotifyResult::Shown && n2 == app::NotifyResult::Shown && app::notificationsSupported());
    CHECK(noteCount == 2 && firstHasAction && !secondHasAction && secondApp == "fuse_app_tray");
    CHECK(noteClicks == 1);

    tray.remove();
    host.drain(a, 10);
    CHECK(!tray.shown() && !host.hasOwner(service));
    // the desktop goes away: no tray, notifications unsupported
    desk.shutdown();
    host.drain(a, 10);
    CHECK(!app::TrayIcon::supported());
    CHECK(app::notify("FUSE UI", "nobody listens") == app::NotifyResult::Unsupported);
    std::printf("  removed: name released; without the daemons: tray unsupported, notify unsupported\n");
    return 0;
}
} // namespace
#endif

int main() {
#if !defined(_WIN32)
    const int r = unixGate();
    if (g_fail) return 1;
    if (r == 77) return 77;
    std::printf("fuse_app_tray: OK\n");
    return 0;
#endif
    if (!app::TrayIcon::supported()) {
        std::printf("fuse_app_tray: skipped (no system tray on this platform / session)\n");
        return 77;
    }
#if defined(_WIN32)
    Image icon;
    icon.resize(32, 32, 0xFF000000u);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) icon.pixels[std::size_t(y * 32 + x)] = (x - 16) * (x - 16) + (y - 16) * (y - 16) < 196 ? 0xFF30A0FFu : 0x00000000u;

    app::TrayIcon tray;
    int clicks = 0, doubles = 0;
    unsigned picked = ~0u;
    std::string pickedText;
    tray.onClick([&] { ++clicks; });
    tray.onDoubleClick([&] { ++doubles; });
    tray.setMenu("Open\nSettings\n-\n!Disabled\nQuit", [&](unsigned i, std::string_view t) { picked = i; pickedText = t; });
    CHECK(tray.menuItemCount() == 4);
    if (!tray.create(icon, "fuse_app_tray gate")) {
        std::printf("fuse_app_tray: skipped (the shell refused Shell_NotifyIcon NIM_ADD)\n");
        return 77;
    }
    CHECK(tray.shown());
    Rect r;
    const bool hasRect = tray.screenRect(r);
    const bool modTip = tray.setTooltip("fuse_app_tray gate (modified)");
    const bool modIcon = tray.setIcon(icon);
    std::printf("  icon added: GetRect %s (%.0f,%.0f %.0fx%.0f), NIM_MODIFY tooltip %d icon %d\n", hasRect ? "ok" : "none",
                r.x, r.y, r.w, r.h, modTip, modIcon);
    CHECK(hasRect || (modTip && modIcon));
    CHECK(modTip && modIcon);

    // simulated shell callbacks (NOTIFYICON_VERSION_4 layout: LOWORD(lParam) = event, HIWORD = icon id)
    HWND hwnd = reinterpret_cast<HWND>(tray.nativeWindow());
    const UINT cb = tray.callbackMessage();
    const LPARAM sel = MAKELPARAM(NIN_SELECT, tray.iconId()), dbl = MAKELPARAM(WM_LBUTTONDBLCLK, tray.iconId());
    const WPARAM at = MAKEWPARAM(100, 100);
    SendMessageW(hwnd, cb, at, sel);
    CHECK(clicks == 1);
    PostMessageW(hwnd, cb, at, sel); // posted: delivered by poll()
    PostMessageW(hwnd, cb, at, dbl);
    tray.poll();
    CHECK(clicks == 2 && doubles == 1);
    std::printf("  callbacks: %d clicks, %d double clicks\n", clicks, doubles);

    // context menu: a real TrackPopupMenu, driven by posted keys from a thread timer inside its modal loop
    POINT pt;
    GetCursorPos(&pt);
    SetTimer(nullptr, 0, 150, &menuTimer);
    const UINT_PTR watchdog = SetTimer(nullptr, 0, 3000, &menuWatchdog);
    SendMessageW(hwnd, cb, MAKEWPARAM(pt.x, pt.y), MAKELPARAM(WM_CONTEXTMENU, tray.iconId()));
    KillTimer(nullptr, watchdog);
    std::printf("  context menu: shown %d, picked %d \"%s\"\n", g_menuSeen, int(picked), pickedText.c_str());
    CHECK(g_menuSeen);
    CHECK(picked == 1 && pickedText == "Settings");

    const app::NotifyResult n = tray.notify("FUSE UI", "fuse_app_tray gate notification");
    std::printf("  notification: %s\n", n == app::NotifyResult::Shown ? "shown" : n == app::NotifyResult::Failed ? "failed" : "unsupported");
    CHECK(n == app::NotifyResult::Shown);
    tray.remove();
    CHECK(!tray.shown());
    CHECK(!tray.screenRect(r));
#endif
    if (g_fail) return 1;
    std::printf("fuse_app_tray: OK\n");
    return 0;
}
