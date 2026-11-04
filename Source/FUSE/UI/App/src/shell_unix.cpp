// fuse_app shell services on X11 / other Unix (see shell.hpp), over the session D-Bus through a runtime-loaded
// libdbus-1 (no build dependency):
//   * TrayIcon is a StatusNotifierItem (org.kde.StatusNotifierItem, registered with org.kde.StatusNotifierWatcher:
//     KDE, GNOME with the AppIndicator extension, waybar, ...); its context menu is exported as com.canonical.dbusmenu
//     and drawn by the tray host. Calls from the host are dispatched by NativePlatform's event pump (the connection's
//     fd is one of its pump watches) or poll().
//   * notify() calls org.freedesktop.Notifications.Notify; a tray icon's notifications carry a "default" action whose
//     ActionInvoked signal fires onNotificationClick.
#include "fuse/app/shell.hpp"
#include "dbus_internal.hpp"
#include "platform_internal.hpp"

#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fuse::app {
using namespace fuse::ui;

namespace {
constexpr const char* kItemPath = "/StatusNotifierItem";
constexpr const char* kMenuPath = "/MenuBar";
constexpr const char* kItemIface = "org.kde.StatusNotifierItem";
constexpr const char* kMenuIface = "com.canonical.dbusmenu";
constexpr const char* kPropsIface = "org.freedesktop.DBus.Properties";
constexpr const char* kNotifyName = "org.freedesktop.Notifications";
constexpr const char* kNotifyPath = "/org/freedesktop/Notifications";

dbus::Connection* sessionBus() { // shared connection owned by libdbus
    dbus::Lib& d = dbus::lib();
    if (!d.load()) return nullptr;
    static dbus::Connection* conn = [&] {
        dbus::Error err{};
        d.errorInit(&err);
        dbus::Connection* c = d.busGet(dbus::kBusSession, &err);
        d.errorFree(&err);
        if (c) d.exitOnDisconnect(c, 0);
        return c;
    }();
    return conn;
}

/// Notify(app_name, replaces_id, app_icon, summary, body, actions, hints, expire_timeout) -> id. 0 when not shown.
std::uint32_t sendNotify(dbus::Connection* conn, std::string_view appName, std::string_view title, std::string_view body,
                         bool defaultAction, NotifyResult& res) {
    dbus::Lib& d = dbus::lib();
    res = NotifyResult::Failed;
    dbus::Message* msg = d.newCall(kNotifyName, kNotifyPath, kNotifyName, "Notify");
    if (!msg) return 0;
    dbus::Writer w(d, msg);
    w.str(appName).u32(0).str("").str(title).str(body);
    w.container(dbus::TArray, "s", [&](dbus::Writer& a) {
        if (defaultAction) a.str("default").str("Open");
    });
    w.container(dbus::TArray, "{sv}", [](dbus::Writer&) {});
    w.i32(-1);
    std::uint32_t id = 0;
    dbus::Error err{};
    d.errorInit(&err);
    if (dbus::Message* reply = d.sendBlocking(conn, msg, 2000, &err)) {
        dbus::Reader r(d, reply);
        id = std::uint32_t(r.integer());
        d.unref(reply);
        res = NotifyResult::Shown;
    } else if (err.name && std::string_view(err.name) == "org.freedesktop.DBus.Error.ServiceUnknown") {
        res = NotifyResult::Unsupported; // no notification daemon on the bus: nothing could show it
    }
    d.errorFree(&err);
    d.unref(msg);
    return id;
}

unsigned g_trayCount = 0;
} // namespace

struct TrayIcon::Impl {
    dbus::Connection* conn = nullptr;
    std::string service; ///< org.kde.StatusNotifierItem-<pid>-<n>
    Image icon;
    std::string tip;
    std::function<void()> click, dblClick, noteClick;
    std::function<void(unsigned, std::string_view)> menuFn;
    std::vector<std::string> items; ///< "" = separator, leading '!' = disabled
    unsigned menuCount = 0;
    std::uint32_t menuRevision = 1;
    std::vector<std::uint32_t> notes; ///< ids of notifications this icon posted

    ~Impl() { close(); }
    void close() {
        if (!conn) return;
        dbus::Lib& d = dbus::lib();
        detail::removePumpWatch(this);
        dbus::Error err{};
        d.errorInit(&err);
        d.busReleaseName(conn, service.c_str(), &err); // the watcher drops the item when its name goes away
        d.errorFree(&err);
        d.connFlush(conn);
        d.connClose(conn);
        d.connUnref(conn);
        conn = nullptr;
    }
    void signal(const char* path, const char* iface, const char* member, void (*fill)(Impl&, dbus::Writer&) = nullptr) {
        if (!conn) return;
        dbus::Lib& d = dbus::lib();
        if (dbus::Message* s = d.newSignal(path, iface, member)) {
            if (fill) {
                dbus::Writer w(d, s);
                fill(*this, w);
            }
            d.connSend(conn, s, nullptr);
            d.unref(s);
            d.connFlush(conn);
        }
    }

    // ---- StatusNotifierItem properties ----
    void pixmaps(dbus::Writer& w, bool withIcon) const { // a(iiay): ARGB32, network byte order
        w.container(dbus::TArray, "(iiay)", [&](dbus::Writer& a) {
            if (!withIcon || icon.width <= 0) return;
            a.container(dbus::TStruct, nullptr, [&](dbus::Writer& s) {
                std::vector<unsigned char> px;
                px.reserve(icon.pixels.size() * 4);
                for (u32 p : icon.pixels) { // 0xAABBGGRR -> A R G B bytes
                    px.push_back(u8(p >> 24));
                    px.push_back(u8(p));
                    px.push_back(u8(p >> 8));
                    px.push_back(u8(p >> 16));
                }
                s.i32(icon.width).i32(icon.height).bytes(px);
            });
        });
    }
    static constexpr const char* kItemProps[] = {"Category", "Id", "Title", "Status", "WindowId", "IconName", "IconPixmap",
                                                 "OverlayIconName", "AttentionIconName", "AttentionIconPixmap", "ToolTip",
                                                 "ItemIsMenu", "Menu"};
    static constexpr const char* kMenuProps[] = {"Version", "TextDirection", "Status", "IconThemePath"};
    /// One property as a variant; false when unknown.
    bool prop(dbus::Writer& w, std::string_view iface, std::string_view name) const {
        auto s = [&](std::string_view v) { w.variant("s", [&](dbus::Writer& x) { x.str(v); }); return true; };
        if (iface == kItemIface) {
            if (name == "Category") return s("ApplicationStatus");
            if (name == "Id") return s(service);
            if (name == "Title") return s(tip);
            if (name == "Status") return s("Active");
            if (name == "IconName" || name == "OverlayIconName" || name == "AttentionIconName") return s("");
            if (name == "WindowId") { w.variant("i", [](dbus::Writer& x) { x.i32(0); }); return true; }
            if (name == "IconPixmap" || name == "AttentionIconPixmap") {
                w.variant("a(iiay)", [&](dbus::Writer& x) { pixmaps(x, name == "IconPixmap"); });
                return true;
            }
            if (name == "ToolTip") { // (icon name, icon pixmaps, title, description)
                w.variant("(sa(iiay)ss)", [&](dbus::Writer& x) {
                    x.container(dbus::TStruct, nullptr, [&](dbus::Writer& t) {
                        t.str("");
                        pixmaps(t, false);
                        t.str(tip).str("");
                    });
                });
                return true;
            }
            if (name == "ItemIsMenu") { w.variant("b", [](dbus::Writer& x) { x.boolean(false); }); return true; }
            if (name == "Menu") { w.variant("o", [](dbus::Writer& x) { x.path(kMenuPath); }); return true; }
        } else if (iface == kMenuIface) {
            if (name == "Version") { w.variant("u", [](dbus::Writer& x) { x.u32(3); }); return true; }
            if (name == "TextDirection") return s("ltr");
            if (name == "Status") return s("normal");
            if (name == "IconThemePath") { w.variant("as", [](dbus::Writer& x) { x.container(dbus::TArray, "s", [](dbus::Writer&) {}); }); return true; }
        }
        return false;
    }

    // ---- dbusmenu: root 0, items 1..n (menu index + 1), separators 1000 + position ----
    int idOf(std::size_t pos, unsigned& index) const {
        unsigned i = 0;
        for (std::size_t k = 0; k < items.size(); ++k) {
            if (k == pos) { index = i; return items[k].empty() ? int(1000 + k) : int(i + 1); }
            if (!items[k].empty()) ++i;
        }
        return -1;
    }
    void itemProps(dbus::Writer& w, int id) const { // a{sv}
        w.container(dbus::TArray, "{sv}", [&](dbus::Writer& a) {
            if (id == 0) {
                a.entry("children-display", "s", [](dbus::Writer& v) { v.str("submenu"); });
                return;
            }
            if (id >= 1000) {
                a.entry("type", "s", [](dbus::Writer& v) { v.str("separator"); });
                return;
            }
            const std::string* it = itemOf(unsigned(id - 1));
            if (!it) return;
            const bool off = (*it)[0] == '!';
            a.entry("label", "s", [&](dbus::Writer& v) { v.str(off ? std::string_view(*it).substr(1) : std::string_view(*it)); });
            a.entry("enabled", "b", [&](dbus::Writer& v) { v.boolean(!off); });
            a.entry("visible", "b", [](dbus::Writer& v) { v.boolean(true); });
        });
    }
    const std::string* itemOf(unsigned index) const {
        unsigned i = 0;
        for (const std::string& it : items)
            if (!it.empty() && i++ == index) return &it;
        return nullptr;
    }
    void layout(dbus::Writer& w) const { // (ia{sv}av): the root and its items
        w.container(dbus::TStruct, nullptr, [&](dbus::Writer& s) {
            s.i32(0);
            itemProps(s, 0);
            s.container(dbus::TArray, "v", [&](dbus::Writer& kids) {
                for (std::size_t k = 0; k < items.size(); ++k) {
                    unsigned index = 0;
                    const int id = idOf(k, index);
                    kids.variant("(ia{sv}av)", [&](dbus::Writer& v) {
                        v.container(dbus::TStruct, nullptr, [&](dbus::Writer& c) {
                            c.i32(id);
                            itemProps(c, id);
                            c.container(dbus::TArray, "v", [](dbus::Writer&) {});
                        });
                    });
                }
            });
        });
    }

    // ---- dispatch ----
    static void onPump(void* user) { static_cast<Impl*>(user)->dispatch(); }
    void dispatch() {
        if (!conn) return;
        dbus::Lib& d = dbus::lib();
        d.readWrite(conn, 0);
        while (conn) {
            dbus::Message* msg = d.popMessage(conn);
            if (!msg) break;
            std::function<void()> after; // callbacks run after the reply is sent (they may remove the icon)
            handle(msg, after);
            d.unref(msg);
            d.connFlush(conn);
            if (after) after();
        }
    }
    void handle(dbus::Message* msg, std::function<void()>& after) {
        dbus::Lib& d = dbus::lib();
        const int type = d.msgType(msg);
        const std::string_view member = d.msgMember(msg) ? d.msgMember(msg) : "";
        const std::string_view iface = d.msgInterface(msg) ? d.msgInterface(msg) : "";
        if (type == dbus::Signal) {
            if (iface == kNotifyName && member == "ActionInvoked") {
                dbus::Reader r(d, msg);
                const auto id = std::uint32_t(r.integer());
                r.next();
                const std::string key = r.str();
                for (std::uint32_t n : notes)
                    if (n == id && key == "default" && noteClick) after = noteClick;
            }
            return;
        }
        if (type != dbus::MethodCall) return;
        const std::string_view path = d.msgPath(msg) ? d.msgPath(msg) : "";
        dbus::Message* reply = d.newReturn(msg);
        bool known = true;
        if (iface == kPropsIface && (member == "Get" || member == "GetAll")) {
            dbus::Reader r(d, msg);
            const std::string ifc = r.str();
            r.next();
            const std::string name = r.str();
            const bool menu = path == kMenuPath;
            if (menu ? ifc != kMenuIface : ifc != kItemIface) known = false;
            else if (member == "Get") {
                dbus::Writer w(d, reply);
                known = prop(w, ifc, name);
            } else {
                dbus::Writer w(d, reply);
                w.container(dbus::TArray, "{sv}", [&](dbus::Writer& a) {
                    auto add = [&](const char* n) { a.container(dbus::TDictEntry, nullptr, [&](dbus::Writer& e) { e.str(n); prop(e, ifc, n); }); };
                    if (menu)
                        for (const char* n : kMenuProps) add(n);
                    else
                        for (const char* n : kItemProps) add(n);
                });
            }
        } else if (path == kItemPath && (member == "Activate" || member == "SecondaryActivate" || member == "ContextMenu" ||
                                         member == "Scroll")) {
            if (member == "Activate" && click) after = click;
            // SNI has no double click; SecondaryActivate (middle click) and ContextMenu (the host shows the dbusmenu) do
            // nothing here
        } else if (path == kMenuPath && member == "GetLayout") {
            dbus::Writer w(d, reply);
            w.u32(menuRevision);
            layout(w);
        } else if (path == kMenuPath && member == "GetGroupProperties") {
            dbus::Reader r(d, msg);
            std::vector<int> ids;
            for (dbus::Reader a = r.sub(); a.type() == dbus::TInt32; a.next()) ids.push_back(int(a.integer()));
            dbus::Writer w(d, reply);
            w.container(dbus::TArray, "(ia{sv})", [&](dbus::Writer& a) {
                auto one = [&](int id) { a.container(dbus::TStruct, nullptr, [&](dbus::Writer& s) { s.i32(id); itemProps(s, id); }); };
                if (ids.empty()) { // all
                    one(0);
                    for (std::size_t k = 0; k < items.size(); ++k) {
                        unsigned index = 0;
                        one(idOf(k, index));
                    }
                }
                for (int id : ids) one(id);
            });
        } else if (path == kMenuPath && member == "Event") {
            dbus::Reader r(d, msg);
            const int id = int(r.integer());
            r.next();
            const std::string ev = r.str();
            if (ev == "clicked" && id > 0 && id < 1000)
                if (const std::string* it = itemOf(unsigned(id - 1)); it && (*it)[0] != '!' && menuFn) {
                    const std::string text = (*it)[0] == '!' ? it->substr(1) : *it;
                    auto fn = menuFn;
                    const unsigned index = unsigned(id - 1);
                    after = [fn, index, text] { fn(index, text); };
                }
        } else if (path == kMenuPath && member == "AboutToShow") {
            dbus::Writer(d, reply).boolean(false);
        } else if (path == kMenuPath && (member == "EventGroup" || member == "AboutToShowGroup")) {
            dbus::Writer w(d, reply);
            w.container(dbus::TArray, "i", [](dbus::Writer&) {});
            if (member == "AboutToShowGroup") w.container(dbus::TArray, "i", [](dbus::Writer&) {});
        } else if (member == "Introspect") {
            dbus::Writer(d, reply).str(path == kMenuPath ? "<node><interface name=\"com.canonical.dbusmenu\"/></node>"
                                                         : "<node><interface name=\"org.kde.StatusNotifierItem\"/></node>");
        } else {
            known = false;
        }
        if (!known) {
            d.unref(reply);
            reply = d.newError(msg, "org.freedesktop.DBus.Error.UnknownMethod", "not supported by fuse_app");
        }
        if (reply) {
            d.connSend(conn, reply, nullptr);
            d.unref(reply);
        }
    }
};

TrayIcon::TrayIcon() : m(std::make_unique<Impl>()) {}
TrayIcon::~TrayIcon() = default;

bool TrayIcon::supported() {
    dbus::Connection* c = sessionBus();
    if (!c) return false;
    dbus::Lib& d = dbus::lib();
    dbus::Error err{};
    d.errorInit(&err);
    const bool has = d.busNameHasOwner(c, "org.kde.StatusNotifierWatcher", &err) != 0;
    d.errorFree(&err);
    return has;
}
bool TrayIcon::create(const Image& icon, std::string_view tooltip) {
    if (m->conn) return true;
    if (!supported()) return false;
    dbus::Lib& d = dbus::lib();
    dbus::Error err{};
    d.errorInit(&err);
    m->conn = d.busGetPrivate(dbus::kBusSession, &err); // its own connection: the pump dispatches it
    d.errorFree(&err);
    if (!m->conn) return false;
    d.exitOnDisconnect(m->conn, 0);
    m->icon = icon;
    m->tip = std::string(tooltip);
    m->service = "org.kde.StatusNotifierItem-" + std::to_string(::getpid()) + "-" + std::to_string(++g_trayCount);
    d.errorInit(&err);
    const int owner = d.busRequestName(m->conn, m->service.c_str(), dbus::kNameFlagDoNotQueue, &err);
    d.errorFree(&err);
    d.errorInit(&err);
    d.busAddMatch(m->conn, "type='signal',interface='org.freedesktop.Notifications',member='ActionInvoked'", &err);
    d.errorFree(&err);
    bool ok = owner == 1; // DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER
    if (ok) {
        dbus::Message* reg = d.newCall("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                                       "RegisterStatusNotifierItem");
        dbus::Writer(d, reg).str(m->service);
        d.errorInit(&err);
        dbus::Message* reply = d.sendBlocking(m->conn, reg, 2000, &err);
        d.errorFree(&err);
        d.unref(reg);
        ok = reply != nullptr;
        if (reply) d.unref(reply);
    }
    if (!ok) {
        m->close();
        return false;
    }
    int fd = -1;
    d.unixFd(m->conn, &fd);
    detail::addPumpWatch(fd, &Impl::onPump, m.get());
    return true;
}
void TrayIcon::remove() { m->close(); }
bool TrayIcon::shown() const { return m->conn != nullptr; }
bool TrayIcon::setIcon(const Image& icon) {
    m->icon = icon;
    if (!m->conn) return false;
    m->signal(kItemPath, kItemIface, "NewIcon");
    return true;
}
bool TrayIcon::setTooltip(std::string_view tooltip) {
    m->tip = std::string(tooltip);
    if (!m->conn) return false;
    m->signal(kItemPath, kItemIface, "NewToolTip");
    m->signal(kItemPath, kItemIface, "NewTitle");
    return true;
}
void TrayIcon::onClick(std::function<void()> fn) { m->click = std::move(fn); }
void TrayIcon::onDoubleClick(std::function<void()> fn) { m->dblClick = std::move(fn); }
void TrayIcon::onNotificationClick(std::function<void()> fn) { m->noteClick = std::move(fn); }
void TrayIcon::setMenu(std::string_view items, std::function<void(unsigned, std::string_view)> fn) {
    m->items.clear();
    m->menuCount = 0;
    while (!items.empty()) {
        const std::size_t nl = items.find('\n');
        const std::string_view line = items.substr(0, nl);
        m->items.emplace_back(line == "-" ? std::string_view{} : line);
        if (line != "-") ++m->menuCount;
        if (nl == std::string_view::npos) break;
        items.remove_prefix(nl + 1);
    }
    m->menuFn = std::move(fn);
    ++m->menuRevision;
    m->signal(kMenuPath, kMenuIface, "LayoutUpdated", [](Impl& i, dbus::Writer& w) { w.u32(i.menuRevision).i32(0); });
}
unsigned TrayIcon::menuItemCount() const { return m->menuCount; }
void TrayIcon::openMenu(Vec2) {} // the tray host draws the exported menu
NotifyResult TrayIcon::notify(std::string_view title, std::string_view body) {
    if (!m->conn) return NotifyResult::Unsupported;
    NotifyResult res = NotifyResult::Failed;
    if (const std::uint32_t id = sendNotify(m->conn, m->service, title, body, true, res)) m->notes.push_back(id);
    return res;
}
bool TrayIcon::screenRect(Rect&) const { return false; } // SNI does not tell where the host shows the icon
void TrayIcon::poll() { m->dispatch(); }
unsigned long long TrayIcon::nativeWindow() const { return 0; }
unsigned TrayIcon::callbackMessage() const { return 0; }
unsigned TrayIcon::iconId() const { return 0; }

NotifyResult notify(std::string_view title, std::string_view body, std::string_view appName) {
    dbus::Connection* conn = sessionBus();
    if (!conn) return NotifyResult::Unsupported;
    NotifyResult res = NotifyResult::Failed;
    sendNotify(conn, appName, title, body, false, res);
    return res;
}
bool notificationsSupported() { return sessionBus() != nullptr; }

} // namespace fuse::app
