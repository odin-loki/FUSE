// Platform-independent halves of TrayIcon / GlobalMenuBar (state + change routing to the backend).
#include <algorithm>

#include "tray_internal.hpp"

namespace fuse::ui {
using namespace tray_detail;

struct TrayIcon::Impl {
    TrayState st;
    std::unique_ptr<TrayBackend> be;
};
TrayIcon::TrayIcon() : m(std::make_unique<Impl>()) {
    m->st.owner = this;
    m->be = makeTrayBackend(m->st);
}
TrayIcon::~TrayIcon() { if (m->be) m->be->hide(); }
bool TrayIcon::show() { return m->be && m->be->show(); }
void TrayIcon::hide() { if (m->be) m->be->hide(); }
bool TrayIcon::visible() const { return m->be && m->be->visible(); }
bool TrayIcon::available() { return trayAvailable(); }
const char* TrayIcon::backendName() { return trayBackendName(); }
void TrayIcon::setId(std::string id) { m->st.id = std::move(id); if (m->be) m->be->changed(kId); }
void TrayIcon::setTitle(std::string t) { m->st.title = std::move(t); if (m->be) m->be->changed(kTitle); }
void TrayIcon::setToolTip(std::string t, std::string b) {
    m->st.ttTitle = std::move(t);
    m->st.ttBody = std::move(b);
    if (m->be) m->be->changed(kToolTip);
}
void TrayIcon::setIcon(const Image& img) { m->st.icon = img; if (m->be) m->be->changed(kIcon); }
void TrayIcon::setIconName(std::string n) { m->st.iconName = std::move(n); if (m->be) m->be->changed(kIcon); }
void TrayIcon::setStatus(TrayStatus s) { m->st.status = s; if (m->be) m->be->changed(kStatus); }
void TrayIcon::setMenu(std::vector<MenuItem> menu) { m->st.menu = std::move(menu); menuChanged(); }
std::vector<MenuItem>& TrayIcon::menu() { return m->st.menu; }
void TrayIcon::menuChanged() { if (m->be) m->be->changed(kMenu); }
void TrayIcon::poll() { if (m->be) m->be->poll(); }
std::string TrayIcon::serviceName() const { return m->be ? m->be->serviceName() : std::string{}; }

struct GlobalMenuBar::Impl {
    std::vector<MenuItem> menus;
    std::unique_ptr<MenuBackend> be;
    bool ok = false;
};
GlobalMenuBar::GlobalMenuBar(u64 w) : m(std::make_unique<Impl>()) { m->be = makeMenuBackend(w); }
GlobalMenuBar::~GlobalMenuBar() = default;
bool GlobalMenuBar::setMenus(std::vector<MenuItem> menus) {
    m->menus = std::move(menus);
    m->ok = m->be && m->be->setMenus(m->menus);
    return m->ok;
}
bool GlobalMenuBar::active() const { return m->be && m->be->active(); }
void GlobalMenuBar::poll() { if (m->be) m->be->poll(); }
std::string GlobalMenuBar::objectPath() const { return m->be ? m->be->objectPath() : std::string{}; }
const char* GlobalMenuBar::backendName() { return menuBackendName(); }

namespace tray_detail {
std::string stripMnemonic(std::string_view l, char* mn) {
    std::string out;
    if (mn) *mn = 0;
    for (std::size_t i = 0; i < l.size(); ++i) {
        if (l[i] == '&' && i + 1 < l.size()) {
            if (l[i + 1] == '&') { out += '&'; ++i; continue; }
            if (mn && !*mn) *mn = l[i + 1];
            continue;
        }
        out += l[i];
    }
    return out;
}
std::string argb32BigEndian(const Image& img) {
    std::string out;
    out.reserve(std::size_t(img.width) * img.height * 4);
    for (u32 p : img.pixels) {
        const u32 r = p & 255, g = (p >> 8) & 255, b = (p >> 16) & 255, a = p >> 24;
        auto un = [a](u32 c) { return a ? char(std::min<u32>(255, (c * 255 + a / 2) / a)) : char(0); };
        out += char(a);
        out += un(r);
        out += un(g);
        out += un(b);
    }
    return out;
}
void FlatMenu::build(std::vector<MenuItem>& menu) {
    nodes.clear();
    nodes.push_back({});
    auto walk = [&](auto& self, std::vector<MenuItem>& items, i32 parent) -> void {
        for (MenuItem& it : items) {
            const i32 id = i32(nodes.size());
            nodes.push_back({&it, parent, {}});
            nodes[std::size_t(parent)].kids.push_back(id);
            self(self, it.children, id);
        }
    };
    walk(walk, menu, 0);
}
} // namespace tray_detail

namespace dbusmenu_detail {
const MenuItem* itemById(const std::vector<MenuItem>& menu, i32 id) {
    FlatMenu f;
    f.build(const_cast<std::vector<MenuItem>&>(menu));
    return id > 0 && id < i32(f.nodes.size()) ? f.nodes[std::size_t(id)].item : nullptr;
}
} // namespace dbusmenu_detail
} // namespace fuse::ui
