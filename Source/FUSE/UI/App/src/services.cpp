// fuse_app platform services, platform-neutral half: window state on AppWindow (the headless window is its own window
// system), close guard, events, the menu model + in-window menu bar, system theme tokens, file-dialog fallback and
// the HiDPI scale maths. See fuse/app/services.hpp.
#include "fuse/app/app.hpp"
#include "fuse/text/from_chars.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fuse::app {
using namespace fuse::ui;

namespace {
std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n' || s.front() == '\'')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n' || s.back() == '\'')) s.remove_suffix(1);
    return s;
}
f32 parsePositive(std::string_view s) {
    s = trimmed(s);
    double v = 0.0;
    if (s.empty()) return 0.f;
    const auto [p, ec] = fuse::text::fromChars(s.data(), s.data() + s.size(), v);
    return ec == std::errc{} && p == s.data() + s.size() && v > 0.0 && v < 1.0e6 ? f32(v) : 0.f;
}
bool containsNoCase(std::string_view hay, std::string_view needle) {
    if (needle.size() > hay.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        std::size_t k = 0;
        while (k < needle.size() && (hay[i + k] | 0x20) == (needle[k] | 0x20)) ++k;
        if (k == needle.size()) return true;
    }
    return false;
}
} // namespace

// ---- HiDPI ---------------------------------------------------------------------------------------------------------
namespace dpi {
f32 scaleFromDpi(f32 d) { return d > 0.f ? std::clamp(d / 96.f, 0.5f, 8.f) : 0.f; }
f32 xftDpi(std::string_view rm) {
    while (!rm.empty()) {
        const std::size_t nl = rm.find('\n');
        std::string_view line = rm.substr(0, nl);
        rm = nl == std::string_view::npos ? std::string_view{} : rm.substr(nl + 1);
        constexpr std::string_view key = "Xft.dpi:";
        if (line.substr(0, key.size()) == key) return parsePositive(line.substr(key.size()));
    }
    return 0.f;
}
f32 x11Scale(std::string_view rm, std::string_view gdkScale, std::string_view fuseScale) {
    if (const f32 s = parsePositive(fuseScale); s > 0.f) return std::clamp(s, 0.5f, 8.f);
    if (const f32 s = scaleFromDpi(xftDpi(rm)); s > 0.f) return s;
    if (const f32 s = parsePositive(gdkScale); s > 0.f) return std::clamp(s, 0.5f, 8.f);
    return 1.f;
}
f32 resolveScale(f32 option, f32 detected) { return option > 0.f ? option : detected > 0.f ? detected : 1.f; }
Size toPixels(Size logical, f32 scale) {
    return {std::max(1.f, std::round(logical.w * scale)), std::max(1.f, std::round(logical.h * scale))};
}
Vec2 toLogical(Vec2 px, f32 scale) { return scale > 0.f ? Vec2{px.x / scale, px.y / scale} : px; }
} // namespace dpi

// ---- system theme --------------------------------------------------------------------------------------------------
ColorScheme schemeFromGtkTheme(std::string_view t) {
    t = trimmed(t);
    if (t.empty()) return ColorScheme::Unknown;
    return containsNoCase(t, "dark") ? ColorScheme::Dark : ColorScheme::Light;
}
ColorScheme schemeFromGsettings(std::string_view out) {
    out = trimmed(out);
    if (out == "prefer-dark") return ColorScheme::Dark;
    if (out == "prefer-light" || out == "default") return ColorScheme::Light;
    return ColorScheme::Unknown;
}
// The token sets live in the widget library (Widgets/src/theme.cpp): the app layer only maps the OS preference on them.
std::string themeTokensCss(const SystemTheme& t) {
    widgets::ThemeDesc d;
    d.variant = t.highContrast ? widgets::ThemeVariant::HighContrast
                               : t.scheme == ColorScheme::Light ? widgets::ThemeVariant::Light : widgets::ThemeVariant::Dark;
    d.hasAccent = t.hasAccent;
    d.accent = t.accent;
    return widgets::themeTokensCss(d);
}

// ---- menus -----------------------------------------------------------------------------------------------------------
std::string shortcutText(Key key, u8 mods) {
    if (key == Key::None || key == Key::Other) return {};
    struct Name { Key k; const char* n; };
    static constexpr Name kNames[] = {
        {Key::Tab, "Tab"}, {Key::Enter, "Enter"}, {Key::Escape, "Esc"}, {Key::Space, "Space"}, {Key::Backspace, "Backspace"},
        {Key::Delete, "Del"}, {Key::Left, "Left"}, {Key::Right, "Right"}, {Key::Up, "Up"}, {Key::Down, "Down"}, {Key::Home, "Home"},
        {Key::End, "End"}, {Key::PageUp, "PgUp"}, {Key::PageDown, "PgDn"}, {Key::A, "A"}, {Key::B, "B"}, {Key::C, "C"}, {Key::D, "D"},
        {Key::E, "E"}, {Key::F, "F"}, {Key::G, "G"}, {Key::H, "H"}, {Key::I, "I"}, {Key::J, "J"}, {Key::K, "K"}, {Key::L, "L"},
        {Key::M, "M"}, {Key::N, "N"}, {Key::O, "O"}, {Key::P, "P"}, {Key::Q, "Q"}, {Key::R, "R"}, {Key::S, "S"}, {Key::T, "T"},
        {Key::U, "U"}, {Key::V, "V"}, {Key::W, "W"}, {Key::X, "X"}, {Key::Y, "Y"}, {Key::Z, "Z"}, {Key::Num0, "0"}, {Key::Num1, "1"},
        {Key::Num2, "2"}, {Key::Num3, "3"}, {Key::Num4, "4"}, {Key::Num5, "5"}, {Key::Num6, "6"}, {Key::Num7, "7"}, {Key::Num8, "8"},
        {Key::Num9, "9"}, {Key::F1, "F1"}, {Key::F2, "F2"}, {Key::F3, "F3"}, {Key::F4, "F4"}, {Key::F5, "F5"}, {Key::F6, "F6"},
        {Key::F7, "F7"}, {Key::F8, "F8"}, {Key::F9, "F9"}, {Key::F10, "F10"}, {Key::F11, "F11"}, {Key::F12, "F12"},
        {Key::BracketLeft, "["}, {Key::BracketRight, "]"}};
    std::string s;
    if (mods & kModCtrl) s += "Ctrl+";
    if (mods & kModAlt) s += "Alt+";
    if (mods & kModShift) s += "Shift+";
    if (mods & kModSuper) s += "Super+";
    for (const Name& n : kNames)
        if (n.k == key) return s + n.n;
    return {};
}
namespace {
void appendMenuLines(const MenuItem& menu, u32 depth, std::string& out, std::vector<u32>& ids) {
    for (const MenuItem& it : menu.items) {
        if (!ids.empty()) out += '\n';
        out.append(depth, '\t');
        if (it.separator) {
            out += '-';
            ids.push_back(0);
            continue;
        }
        if (it.checkable) out += it.checked ? "[x] " : "[ ] ";
        for (char c : it.title) out += c == '\n' || c == '\t' ? ' ' : c;
        if (const std::string sc = shortcutText(it.key, it.mods); !sc.empty() && it.items.empty()) out += "    " + sc;
        ids.push_back(it.items.empty() ? it.id : 0u);
        if (!it.items.empty()) appendMenuLines(it, depth + 1, out, ids);
    }
}
MenuItem* findIn(std::vector<MenuItem>& v, u32 id) {
    for (MenuItem& m : v) {
        if (m.id == id && !m.separator) return &m;
        if (MenuItem* f = findIn(m.items, id)) return f;
    }
    return nullptr;
}
const MenuItem* findKey(const std::vector<MenuItem>& v, Key key, u8 mods) {
    for (const MenuItem& m : v) {
        if (!m.enabled) continue; // a disabled submenu disables its items
        if (m.items.empty() && !m.separator && m.id && m.key == key && m.mods == mods) return &m;
        if (const MenuItem* f = findKey(m.items, key, mods)) return f;
    }
    return nullptr;
}
} // namespace
std::string menuItemsText(const MenuItem& menu, std::vector<u32>& lineIds) {
    std::string out;
    lineIds.clear();
    appendMenuLines(menu, 0, out, lineIds);
    return out;
}
MenuItem* findMenuItem(MenuDesc& d, u32 id) { return id ? findIn(d.menus, id) : nullptr; }
const MenuItem* findMenuShortcut(const MenuDesc& d, Key key, u8 mods) {
    return key == Key::None || key == Key::Other ? nullptr : findKey(d.menus, key, mods);
}

void AppWindow::setMenu(const MenuDesc& d, MenuHandler handler, NodeHandle parent) {
    m_menu = d;
    m_menuHandler = handler;
    m_menuParent = parent;
    rebuildMenu();
}
void AppWindow::rebuildMenu() {
    // the native bar (macOS) replaces the in-window one
    m_nativeMenu = m_backend.apply && m_backend.apply(m_backend.user, *this, WindowRequest::Menu);
    if (m_nativeMenu || m_menu.empty()) {
        if (m_ui.alive(m_menuBar)) m_ui.destroy(m_menuBar);
        m_menuBar = {};
        m_menuButtons.clear();
        m_menuLines.clear();
        m_forceRedraw = true;
        return;
    }
    const bool fresh = !m_ui.alive(m_menuBar) || m_menuButtons.size() != m_menu.menus.size();
    if (fresh) {
        if (m_ui.alive(m_menuBar)) m_ui.destroy(m_menuBar);
        const NodeHandle parent = m_ui.alive(m_menuParent) ? m_menuParent : m_ui.root();
        m_menuBar = widgets::menuBar(m_ui, parent);
        NodeHandle first = m_ui.firstChild(parent);
        if (first == m_chrome.caption) first = m_ui.nextSibling(first); // below a client-side title bar (chrome.hpp)
        if (first != m_menuBar) m_ui.moveTo(m_menuBar, parent, first);
        m_menuButtons.clear();
    }
    m_menuLines.resize(m_menu.menus.size());
    for (u32 i = 0; i < m_menu.menus.size(); ++i) {
        const MenuItem& top = m_menu.menus[i];
        const std::string items = menuItemsText(top, m_menuLines[i]);
        if (fresh) {
            const NodeHandle b = widgets::menuBarMenu(m_ui, m_menuBar, top.title, items);
            m_menuButtons.push_back(b);
            AppWindow* self = this;
            m_ui.connect(b, SignalKind::Submitted, [self, i](UiContext&, NodeHandle, const SignalArgs& a) {
                const u32 line = a.value >= 0.f ? u32(a.value) : ~0u;
                if (i < self->m_menuLines.size() && line < self->m_menuLines[i].size()) self->activateMenuItem(self->m_menuLines[i][line]);
            });
        } else {
            m_ui.setText(m_menuButtons[i], top.title + "\n" + items);
            m_ui.setA11y(m_menuButtons[i], A11yRole::Button, top.title); // the accessible name follows a renamed menu
        }
        m_ui.setDisabled(m_menuButtons[i], !top.enabled);
    }
    m_forceRedraw = true;
}
bool AppWindow::activateMenuItem(u32 id) {
    MenuItem* it = findMenuItem(m_menu, id);
    if (!it || !it->enabled || !it->items.empty()) return false;
    if (it->checkable) {
        it->checked = !it->checked;
        rebuildMenu();
    }
    if (m_menuHandler.activated) m_menuHandler.activated(m_menuHandler.user, *this, id);
    return true;
}
bool AppWindow::menuShortcut(Key key, u8 mods) {
    const MenuItem* it = findMenuShortcut(m_menu, key, mods);
    return it && activateMenuItem(it->id);
}
bool AppWindow::setMenuItemEnabled(u32 id, bool on) {
    MenuItem* it = findMenuItem(m_menu, id);
    if (!it) return false;
    if (it->enabled != on) { it->enabled = on; rebuildMenu(); }
    return true;
}
bool AppWindow::setMenuItemChecked(u32 id, bool on) {
    MenuItem* it = findMenuItem(m_menu, id);
    if (!it) return false;
    if (it->checked != on) { it->checkable = true; it->checked = on; rebuildMenu(); }
    return true;
}
bool AppWindow::menuItemChecked(u32 id) const {
    const MenuItem* it = findMenuItem(const_cast<MenuDesc&>(m_menu), id);
    return it && it->checked;
}

// ---- window state ----------------------------------------------------------------------------------------------------
void AppWindow::emitWindowEvent(const WindowEvent& e) {
    if (m_events.fn) m_events.fn(m_events.user, *this, e);
}
bool AppWindow::ask(WindowRequest r) { return m_backend.apply && m_backend.apply(m_backend.user, *this, r); }
void AppWindow::setWindowBackend(const WindowBackend& b) {
    m_backend = b;
    if (!m_menu.empty()) rebuildMenu(); // a native menu bar takes over from the in-window one (and back)
}
void AppWindow::setTitle(std::string_view t) {
    if (m_title == t) return;
    m_title = t;
    ask(WindowRequest::Title);
}
void AppWindow::setMinimumSize(Size s) {
    m_minSize = {std::max(0.f, s.w), std::max(0.f, s.h)};
    if (!ask(WindowRequest::SizeLimits) && !m_backend.apply) clampToLimits();
}
void AppWindow::setMaximumSize(Size s) {
    m_maxSize = {std::max(0.f, s.w), std::max(0.f, s.h)};
    if (!ask(WindowRequest::SizeLimits) && !m_backend.apply) clampToLimits();
}
Size AppWindow::clampSize(Size s) const {
    if (m_maxSize.w > 0.f) s.w = std::min(s.w, m_maxSize.w);
    if (m_maxSize.h > 0.f) s.h = std::min(s.h, m_maxSize.h);
    return {std::max(s.w, std::max(1.f, m_minSize.w)), std::max(s.h, std::max(1.f, m_minSize.h))};
}
void AppWindow::clampToLimits() {
    const Size now = size(), want = clampSize(now);
    if (want.w != now.w || want.h != now.h) resize(u32(want.w), u32(want.h));
}
void AppWindow::setMode(WindowMode m) {
    m_wantMode = m;
    if (m_backend.apply) { // the OS reports the new state back (notifyMode); false = it cannot
        ask(WindowRequest::Mode);
        return;
    }
    notifyMode(m);
}
void AppWindow::setVisible(bool on) {
    m_wantVisible = on;
    if (m_backend.apply) {
        ask(WindowRequest::Visible);
        return;
    }
    notifyVisible(on);
}
void AppWindow::setPosition(Vec2 p) {
    m_wantPosition = p;
    m_positionAsked = true;
    if (m_backend.apply) {
        ask(WindowRequest::Position);
        return;
    }
    notifyMoved(p);
}
void AppWindow::activate() {
    if (m_backend.apply) {
        ask(WindowRequest::Activate);
        return;
    }
    notifyActive(true);
}
bool AppWindow::tryClose() {
    if (m_closing) return true;
    if (m_closeGuard.allow && !m_closeGuard.allow(m_closeGuard.user, *this)) {
        emitWindowEvent({.type = WindowEvent::Type::CloseVetoed});
        return false;
    }
    m_closing = true;
    emitWindowEvent({.type = WindowEvent::Type::CloseRequested});
    return true;
}
void AppWindow::notifyMode(WindowMode m) {
    if (m_mode == m) return;
    // leaving Minimized returns to what the window was before (Qt::WindowNoState / Maximized / FullScreen)
    if (m != WindowMode::Minimized) m_restoreMode = m;
    m_mode = m;
    m_forceRedraw = true;
    emitWindowEvent({.type = WindowEvent::Type::ModeChanged, .mode = m});
}
void AppWindow::notifyActive(bool on) {
    if (m_active == on) return;
    m_active = on;
    emitWindowEvent({.type = on ? WindowEvent::Type::FocusGained : WindowEvent::Type::FocusLost});
}
void AppWindow::notifyMoved(Vec2 p) {
    if (m_position.x == p.x && m_position.y == p.y) return;
    m_position = p;
    emitWindowEvent({.type = WindowEvent::Type::Moved, .position = p});
}
void AppWindow::notifyVisible(bool on) {
    if (m_visible == on) return;
    m_visible = on;
    if (on) m_forceRedraw = true;
    emitWindowEvent({.type = on ? WindowEvent::Type::Shown : WindowEvent::Type::Hidden});
}
void AppWindow::notifyScale(f32 before) {
    const f32 now = scale();
    if (now != before) emitWindowEvent({.type = WindowEvent::Type::ScaleChanged, .scale = now});
}
void AppWindow::applyTheme(const SystemTheme& t) {
    if (auto sheet = StyleSheet::parse(themeTokensCss(t))) {
        // one override sheet, replaced in place on every later call (the sheet list does not grow)
        if (m_ui.styles().hasSheet(m_themeOverride)) (void)m_ui.replaceStyleSheet(m_themeOverride, std::move(*sheet));
        else m_themeOverride = m_ui.addStyleSheetTracked(std::move(*sheet));
        m_ui.markAllDirty();
        m_forceRedraw = true;
    }
}
void AppWindow::notifyTheme(const SystemTheme& t) {
    if (m_followTheme && t.scheme != ColorScheme::Unknown) applyTheme(t);
    emitWindowEvent({.type = WindowEvent::Type::ThemeChanged, .theme = t});
}
void AppWindow::setFollowSystemTheme(bool on) {
    m_followTheme = on;
    if (on && m_app.systemTheme().scheme != ColorScheme::Unknown) applyTheme(m_app.systemTheme());
}
void Application::setSystemTheme(const SystemTheme& t) {
    if (m_theme == t) return;
    m_theme = t;
    for (auto& w : m_windows) w->notifyTheme(t);
}

// ---- file dialog fallback --------------------------------------------------------------------------------------------
namespace {
struct DialogSlot {
    AppWindow* win;
    FileDialogCallback cb;
};
} // namespace
NodeHandle openToolkitFileDialog(AppWindow& w, const FileDialogOptions& o, FileDialogCallback cb) {
    UiContext& ui = w.ui();
    const NodeHandle dlg = widgets::fileDialog(ui, o.title, o.directory.empty() ? std::string_view(".") : std::string_view(o.directory),
                                               o.filter, o.save);
    if (!ui.alive(dlg)) {
        if (cb.done) cb.done(cb.user, w, {});
        return {};
    }
    const DialogSlot slot{&w, cb};
    ui.connect(dlg, SignalKind::Submitted, [slot](UiContext&, NodeHandle, const SignalArgs& a) {
        if (slot.cb.done) slot.cb.done(slot.cb.user, *slot.win, a.text);
    });
    widgets::openDialog(ui, dlg);
    w.requestRedraw();
    return dlg;
}
namespace {
/// "*.png;*.jpg" -> the patterns, in order.
std::vector<std::string> patterns(std::string_view filter) {
    std::vector<std::string> out;
    while (!filter.empty()) {
        const std::size_t semi = filter.find(';');
        const std::string_view p = trimmed(filter.substr(0, semi));
        if (!p.empty()) out.emplace_back(p);
        filter = semi == std::string_view::npos ? std::string_view{} : filter.substr(semi + 1);
    }
    return out;
}
std::string startPath(const FileDialogOptions& o) {
    std::string p = o.directory;
    if (!p.empty() && p.back() != '/') p += '/';
    if (o.save) p += o.defaultName;
    return p;
}
} // namespace
std::vector<std::string> zenityArgs(const FileDialogOptions& o) {
    std::vector<std::string> a{"zenity", "--file-selection", "--title=" + o.title};
    if (o.save) { a.emplace_back("--save"); a.emplace_back("--confirm-overwrite"); }
    if (o.pickDirectory) a.emplace_back("--directory");
    if (const std::string p = startPath(o); !p.empty()) a.push_back("--filename=" + p);
    if (const auto pats = patterns(o.filter); !pats.empty() && !o.pickDirectory) {
        std::string f = "--file-filter=" + (o.filterName.empty() ? std::string("Files") : o.filterName) + " |";
        for (const std::string& p : pats) f += " " + p;
        a.push_back(std::move(f));
    }
    return a;
}
std::vector<std::string> kdialogArgs(const FileDialogOptions& o) {
    std::vector<std::string> a{"kdialog", "--title", o.title};
    a.emplace_back(o.pickDirectory ? "--getexistingdirectory" : o.save ? "--getsavefilename" : "--getopenfilename");
    const std::string p = startPath(o);
    a.push_back(p.empty() ? std::string(".") : p);
    if (const auto pats = patterns(o.filter); !pats.empty() && !o.pickDirectory) {
        std::string f;
        for (const std::string& q : pats) f += (f.empty() ? "" : " ") + q;
        if (!o.filterName.empty()) f = o.filterName + " (" + f + ")";
        a.push_back(std::move(f));
    }
    return a;
}

} // namespace fuse::app
