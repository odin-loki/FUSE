// fuse_app shell services on X11 / other Unix (see shell.hpp). Tray icons are not done yet (XEmbed system tray), so
// TrayIcon reports unsupported; notify() calls org.freedesktop.Notifications.Notify over the session bus through a
// runtime-loaded libdbus-1 (no build dependency), else reports Unsupported.
#include "fuse/app/shell.hpp"

#include <dlfcn.h>

#include <cstdint>
#include <string>

namespace fuse::app {
using namespace fuse::ui;

struct TrayIcon::Impl {};

TrayIcon::TrayIcon() : m(std::make_unique<Impl>()) {}
TrayIcon::~TrayIcon() = default;
bool TrayIcon::supported() { return false; }
bool TrayIcon::create(const Image&, std::string_view) { return false; }
void TrayIcon::remove() {}
bool TrayIcon::shown() const { return false; }
bool TrayIcon::setIcon(const Image&) { return false; }
bool TrayIcon::setTooltip(std::string_view) { return false; }
void TrayIcon::onClick(std::function<void()>) {}
void TrayIcon::onDoubleClick(std::function<void()>) {}
void TrayIcon::onNotificationClick(std::function<void()>) {}
void TrayIcon::setMenu(std::string_view, std::function<void(unsigned, std::string_view)>) {}
unsigned TrayIcon::menuItemCount() const { return 0; }
void TrayIcon::openMenu(Vec2) {}
NotifyResult TrayIcon::notify(std::string_view title, std::string_view body) { return fuse::app::notify(title, body); }
bool TrayIcon::screenRect(Rect&) const { return false; }
void TrayIcon::poll() {}
unsigned long long TrayIcon::nativeWindow() const { return 0; }
unsigned TrayIcon::callbackMessage() const { return 0; }
unsigned TrayIcon::iconId() const { return 0; }

namespace {
// The few libdbus-1 entry points used (stable C ABI). DBusMessageIter is caller-allocated; 128 bytes covers its
// 14-field layout on every ABI with room to spare.
struct DBusConnection;
struct DBusMessage;
struct DBusMessageIter { alignas(8) unsigned char opaque[128]; };
struct DBusError { const char* name; const char* message; unsigned dummy; void* padding1; };
constexpr int kBusSession = 0;
constexpr int kTypeString = 's', kTypeUint32 = 'u', kTypeInt32 = 'i', kTypeArray = 'a';

struct DBus {
    void* lib = nullptr;
    void (*errorInit)(DBusError*) = nullptr;
    void (*errorFree)(DBusError*) = nullptr;
    DBusConnection* (*busGet)(int, DBusError*) = nullptr;
    DBusMessage* (*newCall)(const char*, const char*, const char*, const char*) = nullptr;
    void (*iterInitAppend)(DBusMessage*, DBusMessageIter*) = nullptr;
    unsigned (*appendBasic)(DBusMessageIter*, int, const void*) = nullptr;
    unsigned (*openContainer)(DBusMessageIter*, int, const char*, DBusMessageIter*) = nullptr;
    unsigned (*closeContainer)(DBusMessageIter*, DBusMessageIter*) = nullptr;
    DBusMessage* (*sendBlocking)(DBusConnection*, DBusMessage*, int, DBusError*) = nullptr;
    void (*unref)(DBusMessage*) = nullptr;

    template <class F>
    bool sym(F& f, const char* name) {
        f = reinterpret_cast<F>(dlsym(lib, name));
        return f != nullptr;
    }
    bool load() {
        if (lib) return true;
        lib = dlopen("libdbus-1.so.3", RTLD_LAZY | RTLD_LOCAL);
        if (!lib) return false;
        const bool ok = sym(errorInit, "dbus_error_init") && sym(errorFree, "dbus_error_free") && sym(busGet, "dbus_bus_get") &&
                        sym(newCall, "dbus_message_new_method_call") && sym(iterInitAppend, "dbus_message_iter_init_append") &&
                        sym(appendBasic, "dbus_message_iter_append_basic") &&
                        sym(openContainer, "dbus_message_iter_open_container") &&
                        sym(closeContainer, "dbus_message_iter_close_container") &&
                        sym(sendBlocking, "dbus_connection_send_with_reply_and_block") && sym(unref, "dbus_message_unref");
        if (!ok) {
            dlclose(lib);
            lib = nullptr;
        }
        return ok;
    }
};
DBus& dbus() {
    static DBus d;
    return d;
}
DBusConnection* sessionBus() {
    DBus& d = dbus();
    if (!d.load()) return nullptr;
    static DBusConnection* conn = nullptr; // shared connection owned by libdbus
    if (!conn) {
        DBusError err{};
        d.errorInit(&err);
        conn = d.busGet(kBusSession, &err);
        d.errorFree(&err);
    }
    return conn;
}
} // namespace

NotifyResult notify(std::string_view title, std::string_view body, std::string_view appName) {
    DBusConnection* conn = sessionBus();
    if (!conn) return NotifyResult::Unsupported;
    DBus& d = dbus();
    DBusMessage* msg = d.newCall("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                 "org.freedesktop.Notifications", "Notify");
    if (!msg) return NotifyResult::Failed;
    const std::string app(appName), summary(title), text(body);
    const char* appC = app.c_str();
    const char* iconC = "";
    const char* summaryC = summary.c_str();
    const char* bodyC = text.c_str();
    const std::uint32_t replaces = 0;
    const std::int32_t timeout = -1;
    DBusMessageIter it{}, actions{}, hints{};
    d.iterInitAppend(msg, &it);
    bool ok = d.appendBasic(&it, kTypeString, &appC) && d.appendBasic(&it, kTypeUint32, &replaces) &&
              d.appendBasic(&it, kTypeString, &iconC) && d.appendBasic(&it, kTypeString, &summaryC) &&
              d.appendBasic(&it, kTypeString, &bodyC);
    ok = ok && d.openContainer(&it, kTypeArray, "s", &actions) && d.closeContainer(&it, &actions);
    ok = ok && d.openContainer(&it, kTypeArray, "{sv}", &hints) && d.closeContainer(&it, &hints);
    ok = ok && d.appendBasic(&it, kTypeInt32, &timeout);
    NotifyResult res = NotifyResult::Failed;
    if (ok) {
        DBusError err{};
        d.errorInit(&err);
        if (DBusMessage* reply = d.sendBlocking(conn, msg, 2000, &err)) {
            d.unref(reply);
            res = NotifyResult::Shown;
        } else {
            // no notification daemon on the bus: nothing could show it
            const std::string_view name = err.name ? err.name : "";
            if (name == "org.freedesktop.DBus.Error.ServiceUnknown") res = NotifyResult::Unsupported;
        }
        d.errorFree(&err);
    }
    d.unref(msg);
    return res;
}
bool notificationsSupported() { return sessionBus() != nullptr; }

} // namespace fuse::app
