// Linux / BSD tray (KDE/freedesktop StatusNotifierItem) and global menu (com.canonical.AppMenu.Registrar), both
// with menus exported as com.canonical.dbusmenu. All D-Bus traffic goes through the runtime-loaded libdbus wrapper.
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
#include <atomic>

#include "fuse/ui/extras/dbus.hpp"
#include "tray_internal.hpp"

namespace fuse::ui::tray_detail {
using dbus::Connection;
using dbus::Message;
using dbus::Reader;
using dbus::Writer;

namespace {
constexpr const char* kWatcher = "org.kde.StatusNotifierWatcher";
constexpr const char* kSni = "org.kde.StatusNotifierItem";
constexpr const char* kMenuIface = "com.canonical.dbusmenu";
constexpr const char* kProps = "org.freedesktop.DBus.Properties";
constexpr const char* kIntro = "org.freedesktop.DBus.Introspectable";

void replyEmpty(Connection& c, const Message& m) { Message r = m.reply(); c.send(r); }
void replyError(Connection& c, const Message& m, const char* text) {
    Message r = m.error("org.freedesktop.DBus.Error.UnknownMethod", text);
    c.send(r);
}

/// "Ctrl+Shift+S" -> ["Control", "Shift", "S"] (dbusmenu shortcut format).
std::vector<std::string> shortcutParts(std::string_view s) {
    std::vector<std::string> out;
    std::size_t b = 0;
    while (b <= s.size() && !s.empty()) {
        std::size_t e = s.find('+', b + 1);
        if (e == std::string_view::npos) e = s.size();
        std::string p(s.substr(b, e - b));
        if (p == "Ctrl") p = "Control";
        if (p == "Meta" || p == "Cmd") p = "Super";
        if (!p.empty()) out.push_back(p);
        b = e + 1;
        if (e >= s.size()) break;
    }
    return out;
}
} // namespace

/// com.canonical.dbusmenu exporter over a menu tree owned elsewhere.
class DbusMenu {
public:
    DbusMenu(Connection& c, std::string path, std::vector<MenuItem>* menu) : m_c(c), m_path(std::move(path)), m_menu(menu) {
        rebuild();
        m_c.exportObject(m_path, [this](Connection& c, const Message& m) { return handle(c, m); });
    }
    void changed() {
        rebuild();
        ++m_rev;
        Message s = Message::signal(m_path, kMenuIface, "LayoutUpdated");
        s.args().u(m_rev).i(0);
        m_c.send(s);
        m_c.flush();
    }
    const std::string& path() const { return m_path; }
    u32 revision() const { return m_rev; }

private:
    void rebuild() { m_flat.build(*m_menu); }
    MenuItem* item(i32 id) { return id > 0 && id < i32(m_flat.nodes.size()) ? m_flat.nodes[std::size_t(id)].item : nullptr; }
    void props(Writer& w, i32 id) {
        w.open('a', "{sv}");
        if (id == 0) w.entryS("children-display", "submenu");
        else if (MenuItem* it = item(id)) {
            if (it->separator) w.entryS("type", "separator");
            else {
                // dbusmenu mnemonics use '_' (and "__" for a literal underscore)
                std::string l;
                for (std::size_t i = 0; i < it->label.size(); ++i) {
                    const char ch = it->label[i];
                    if (ch == '_') l += "__";
                    else if (ch == '&' && i + 1 < it->label.size() && it->label[i + 1] == '&') { l += '&'; ++i; }
                    else if (ch == '&') l += '_';
                    else l += ch;
                }
                w.entryS("label", l);
            }
            if (!it->enabled) w.entryB("enabled", false);
            if (!it->visible) w.entryB("visible", false);
            if (it->checkable) {
                w.entryS("toggle-type", it->radio ? "radio" : "checkmark");
                w.entryI("toggle-state", it->checked ? 1 : 0);
            }
            if (!it->iconName.empty()) w.entryS("icon-name", it->iconName);
            if (!it->children.empty()) w.entryS("children-display", "submenu");
            if (!it->shortcut.empty()) {
                w.open('e').s("shortcut").open('v', "aas").open('a', "as").open('a', "s");
                for (const auto& p : shortcutParts(it->shortcut)) w.s(p);
                w.close().close().close().close();
            }
        }
        w.close();
    }
    void node(Writer& w, i32 id, i32 depth) {
        w.i(id);
        props(w, id);
        w.open('a', "v");
        if (depth != 0 && id < i32(m_flat.nodes.size()))
            for (i32 k : m_flat.nodes[std::size_t(id)].kids) {
                w.open('v', "(ia{sv}av)").open('r');
                node(w, k, depth - 1);
                w.close().close();
            }
        w.close();
    }
    void trigger(i32 id) {
        MenuItem* it = item(id);
        if (!it || !it->enabled || it->separator) return;
        if (it->checkable) {
            if (it->radio) {
                const i32 parent = m_flat.nodes[std::size_t(id)].parent;
                for (i32 s : m_flat.nodes[std::size_t(parent)].kids)
                    if (MenuItem* o = item(s); o && o->radio) o->checked = false;
                it->checked = true;
            } else it->checked = !it->checked;
        }
        const auto fn = it->onTriggered; // the callback may rebuild the menu
        if (it->checkable) changed();
        if (fn) fn();
    }
    bool handle(Connection& c, const Message& m) {
        if (m.isCall(kIntro, "Introspect")) {
            Message r = m.reply();
            r.args().s("<node><interface name=\"com.canonical.dbusmenu\"><method name=\"GetLayout\"><arg type=\"i\" direction=\"in\"/>"
                       "<arg type=\"i\" direction=\"in\"/><arg type=\"as\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/>"
                       "<arg type=\"(ia{sv}av)\" direction=\"out\"/></method><method name=\"Event\"><arg type=\"i\" direction=\"in\"/>"
                       "<arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/></method>"
                       "<method name=\"AboutToShow\"><arg type=\"i\" direction=\"in\"/><arg type=\"b\" direction=\"out\"/></method>"
                       "<signal name=\"LayoutUpdated\"><arg type=\"u\"/><arg type=\"i\"/></signal>"
                       "<property name=\"Version\" type=\"u\" access=\"read\"/></interface></node>");
            c.send(r);
            return true;
        }
        if (m.isCall(kProps, "Get") || m.isCall(kProps, "GetAll")) {
            Reader rd = m.read();
            rd.next();
            const std::string name = m.member() == "Get" ? rd.str() : "";
            Message r = m.reply();
            Writer w = r.args();
            const u32 ver = 3;
            if (m.member() == "Get") {
                if (name == "Version") w.open('v', "u").u(ver).close();
                else if (name == "TextDirection") w.open('v', "s").s("ltr").close();
                else if (name == "Status") w.open('v', "s").s("normal").close();
                else if (name == "IconThemePath") w.open('v', "as").open('a', "s").close().close();
                else { replyError(c, m, "no such property"); return true; }
            } else {
                w.open('a', "{sv}").entry("Version", 'u', &ver).entryS("TextDirection", "ltr").entryS("Status", "normal").close();
            }
            c.send(r);
            return true;
        }
        if (m.interface() != kMenuIface && !m.interface().empty()) return false;
        const std::string mb = m.member();
        if (mb == "GetLayout") {
            Reader rd = m.read();
            const i32 parent = rd.i32v();
            rd.next();
            const i32 depth = rd.i32v();
            Message r = m.reply();
            Writer w = r.args();
            w.u(m_rev).open('r');
            node(w, parent < i32(m_flat.nodes.size()) ? parent : 0, depth);
            w.close();
            c.send(r);
            return true;
        }
        if (mb == "GetGroupProperties") {
            std::vector<i32> ids;
            Reader a = m.read().recurse();
            while (a.type() == 'i') { ids.push_back(a.i32v()); a.next(); }
            if (ids.empty()) for (i32 i = 0; i < i32(m_flat.nodes.size()); ++i) ids.push_back(i);
            Message r = m.reply();
            Writer w = r.args();
            w.open('a', "(ia{sv})");
            for (i32 id : ids) {
                if (id < 0 || id >= i32(m_flat.nodes.size())) continue;
                w.open('r').i(id);
                props(w, id);
                w.close();
            }
            w.close();
            c.send(r);
            return true;
        }
        if (mb == "GetProperty") {
            Reader rd = m.read();
            const i32 id = rd.i32v();
            rd.next();
            const std::string prop = rd.str();
            MenuItem* it = item(id);
            Message r = m.reply();
            Writer w = r.args();
            if (prop == "label") w.open('v', "s").s(it ? it->label : "").close();
            else if (prop == "enabled") w.open('v', "b").b(it ? it->enabled : true).close();
            else { replyError(c, m, "no such property"); return true; }
            c.send(r);
            return true;
        }
        if (mb == "Event") {
            Reader rd = m.read();
            const i32 id = rd.i32v();
            rd.next();
            const std::string ev = rd.str();
            replyEmpty(c, m);
            if (ev == "clicked") trigger(id);
            return true;
        }
        if (mb == "EventGroup") {
            std::vector<i32> fire;
            Reader a = m.read().recurse();
            while (a.type() == 'r') {
                Reader e = a.recurse();
                const i32 id = e.i32v();
                e.next();
                if (e.str() == "clicked") fire.push_back(id);
                a.next();
            }
            Message r = m.reply();
            r.args().open('a', "i").close();
            c.send(r);
            for (i32 id : fire) trigger(id);
            return true;
        }
        if (mb == "AboutToShow") {
            Message r = m.reply();
            r.args().b(false);
            c.send(r);
            return true;
        }
        if (mb == "AboutToShowGroup") {
            Message r = m.reply();
            r.args().open('a', "i").close().open('a', "i").close();
            c.send(r);
            return true;
        }
        return false;
    }

    Connection& m_c;
    std::string m_path;
    std::vector<MenuItem>* m_menu;
    FlatMenu m_flat;
    u32 m_rev = 1;
};

namespace {
std::atomic<int> g_counter{0};

class SniBackend final : public TrayBackend {
public:
    explicit SniBackend(TrayState& st) : m_st(st) {}
    bool show() override {
        if (m_visible) return true;
        m_c = Connection::session();
        if (!m_c) return false;
        std::string u = m_c->uniqueName();
        for (char& ch : u) if (ch == ':' || ch == '.') ch = '_';
        m_service = std::string(kSni) + "-" + u.substr(u.find_first_not_of('_')) + "-" + std::to_string(++g_counter);
        if (!m_c->requestName(m_service)) { m_c.reset(); return false; }
        m_c->exportObject("/StatusNotifierItem", [this](Connection& c, const Message& m) { return handle(c, m); });
        m_menu = std::make_unique<DbusMenu>(*m_c, "/MenuBar", &m_st.menu);
        Message reg = Message::call(kWatcher, "/StatusNotifierWatcher", kWatcher, "RegisterStatusNotifierItem");
        reg.args().s(m_service);
        // the watcher may call back into us (property reads) before replying: answer from a pumping loop
        m_c->send(reg);
        m_c->flush();
        m_visible = m_c->nameHasOwner(kWatcher);
        if (!m_visible) { m_menu.reset(); m_c.reset(); }
        return m_visible;
    }
    void hide() override {
        m_menu.reset();
        m_c.reset(); // closing the connection drops the bus name; the watcher unregisters the item
        m_visible = false;
    }
    bool visible() const override { return m_visible; }
    void changed(u32 what) override {
        if (!m_c) return;
        auto sig = [&](const char* name) { Message s = Message::signal("/StatusNotifierItem", kSni, name); m_c->send(s); };
        if (what & kTitle) sig("NewTitle");
        if (what & kIcon) sig("NewIcon");
        if (what & kToolTip) sig("NewToolTip");
        if (what & kStatus) {
            Message s = Message::signal("/StatusNotifierItem", kSni, "NewStatus");
            s.args().s(status());
            m_c->send(s);
        }
        if ((what & kMenu) && m_menu) m_menu->changed();
        m_c->flush();
    }
    void poll() override { if (m_c) m_c->poll(0); }
    std::string serviceName() const override { return m_service; }

private:
    const char* status() const {
        return m_st.status == TrayStatus::Passive ? "Passive" : m_st.status == TrayStatus::NeedsAttention ? "NeedsAttention" : "Active";
    }
    void pixmaps(Writer& w, const Image& img) {
        w.open('a', "(iiay)");
        if (img.width > 0 && img.height > 0) {
            const std::string px = argb32BigEndian(img);
            w.open('r').i(img.width).i(img.height).bytes(px.data(), px.size()).close();
        }
        w.close();
    }
    bool prop(Writer& w, std::string_view n) {
        if (n == "Category") w.open('v', "s").s("ApplicationStatus").close();
        else if (n == "Id") w.open('v', "s").s(m_st.id).close();
        else if (n == "Title") w.open('v', "s").s(m_st.title).close();
        else if (n == "Status") w.open('v', "s").s(status()).close();
        else if (n == "WindowId") w.open('v', "i").i(0).close();
        else if (n == "IconName") w.open('v', "s").s(m_st.iconName).close();
        else if (n == "IconPixmap") { w.open('v', "a(iiay)"); pixmaps(w, m_st.icon); w.close(); }
        else if (n == "OverlayIconName" || n == "AttentionIconName" || n == "IconThemePath") w.open('v', "s").s("").close();
        else if (n == "OverlayIconPixmap" || n == "AttentionIconPixmap") { w.open('v', "a(iiay)"); pixmaps(w, Image{}); w.close(); }
        else if (n == "AttentionMovieName") w.open('v', "s").s("").close();
        else if (n == "ToolTip") {
            w.open('v', "(sa(iiay)ss)").open('r').s(m_st.iconName);
            pixmaps(w, Image{});
            w.s(m_st.ttTitle.empty() ? m_st.title : m_st.ttTitle).s(m_st.ttBody).close().close();
        } else if (n == "ItemIsMenu") w.open('v', "b").b(false).close();
        else if (n == "Menu") w.open('v', "o").o("/MenuBar").close();
        else return false;
        return true;
    }
    bool handle(Connection& c, const Message& m) {
        static constexpr const char* kAll[] = {"Category", "Id", "Title", "Status", "WindowId", "IconName", "IconPixmap",
                                               "OverlayIconName", "OverlayIconPixmap", "AttentionIconName", "AttentionIconPixmap",
                                               "AttentionMovieName", "IconThemePath", "ToolTip", "ItemIsMenu", "Menu"};
        if (m.isCall(kProps, "Get")) {
            Reader rd = m.read();
            rd.next();
            Message r = m.reply();
            Writer w = r.args();
            if (!prop(w, rd.str())) { replyError(c, m, "no such property"); return true; }
            c.send(r);
            return true;
        }
        if (m.isCall(kProps, "GetAll")) {
            Message r = m.reply();
            Writer w = r.args();
            w.open('a', "{sv}");
            for (const char* n : kAll) { w.open('e').s(n); prop(w, n); w.close(); }
            w.close();
            c.send(r);
            return true;
        }
        if (m.isCall(kIntro, "Introspect")) {
            Message r = m.reply();
            r.args().s("<node><interface name=\"org.kde.StatusNotifierItem\">"
                       "<method name=\"Activate\"><arg type=\"i\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/></method>"
                       "<method name=\"SecondaryActivate\"><arg type=\"i\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/></method>"
                       "<method name=\"ContextMenu\"><arg type=\"i\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/></method>"
                       "<method name=\"Scroll\"><arg type=\"i\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/></method>"
                       "<property name=\"Title\" type=\"s\" access=\"read\"/><property name=\"Id\" type=\"s\" access=\"read\"/>"
                       "<property name=\"Status\" type=\"s\" access=\"read\"/><property name=\"IconPixmap\" type=\"a(iiay)\" access=\"read\"/>"
                       "<property name=\"Menu\" type=\"o\" access=\"read\"/><signal name=\"NewIcon\"/><signal name=\"NewTitle\"/>"
                       "<signal name=\"NewToolTip\"/><signal name=\"NewStatus\"><arg type=\"s\"/></signal></interface></node>");
            c.send(r);
            return true;
        }
        if (m.interface() != kSni && !m.interface().empty()) return false;
        Reader rd = m.read();
        const std::string mb = m.member();
        if (mb == "Activate" || mb == "SecondaryActivate" || mb == "ContextMenu") {
            const i32 x = rd.i32v();
            rd.next();
            const i32 y = rd.i32v();
            replyEmpty(c, m);
            TrayIcon* o = m_st.owner;
            if (mb == "Activate" && o->onActivated) o->onActivated(x, y);
            if (mb == "SecondaryActivate" && o->onSecondaryActivated) o->onSecondaryActivated(x, y);
            return true;
        }
        if (mb == "Scroll") {
            const i32 d = rd.i32v();
            rd.next();
            const bool h = rd.str() == "horizontal";
            replyEmpty(c, m);
            if (m_st.owner->onScroll) m_st.owner->onScroll(d, h);
            return true;
        }
        return false;
    }

    TrayState& m_st;
    std::unique_ptr<Connection> m_c;
    std::unique_ptr<DbusMenu> m_menu;
    std::string m_service;
    bool m_visible = false;
};

class AppMenuBackend final : public MenuBackend {
public:
    explicit AppMenuBackend(u64 w) : m_window(w) {}
    bool setMenus(std::vector<MenuItem>& menus) override {
        if (!m_c) {
            m_c = Connection::session();
            if (!m_c) return false;
            m_path = "/MenuBar/" + std::to_string(++g_counter);
            m_menu = std::make_unique<DbusMenu>(*m_c, m_path, &menus);
        } else m_menu->changed();
        if (!m_registered) {
            Message reg = Message::call("com.canonical.AppMenu.Registrar", "/com/canonical/AppMenu/Registrar",
                                        "com.canonical.AppMenu.Registrar", "RegisterWindow");
            reg.args().u(u32(m_window)).o(m_path);
            m_registered = m_c->nameHasOwner("com.canonical.AppMenu.Registrar") && m_c->send(reg);
            m_c->flush();
        }
        return m_registered;
    }
    bool active() const override { return m_registered; }
    void poll() override { if (m_c) m_c->poll(0); }
    std::string objectPath() const override { return m_path; }

private:
    u64 m_window;
    std::unique_ptr<Connection> m_c;
    std::unique_ptr<DbusMenu> m_menu;
    std::string m_path;
    bool m_registered = false;
};
} // namespace

std::unique_ptr<TrayBackend> makeTrayBackend(TrayState& st) { return std::make_unique<SniBackend>(st); }
bool trayAvailable() {
    auto c = Connection::session();
    return c && c->nameHasOwner(kWatcher);
}
const char* trayBackendName() { return dbus::available() ? "sni" : "none"; }
std::unique_ptr<MenuBackend> makeMenuBackend(u64 w) { return std::make_unique<AppMenuBackend>(w); }
const char* menuBackendName() { return dbus::available() ? "appmenu" : "none"; }

} // namespace fuse::ui::tray_detail
#endif
