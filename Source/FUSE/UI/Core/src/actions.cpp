#include "fuse/ui/actions.hpp"

#include <algorithm>
#include <utility>

namespace fuse::ui {

namespace {
struct KeyNameEntry {
    Key key;
    std::string_view name;
};
// clang-format off
constexpr KeyNameEntry kKeyNames[] = {
    {Key::Tab, "Tab"}, {Key::Enter, "Enter"}, {Key::Escape, "Esc"}, {Key::Space, "Space"}, {Key::Backspace, "Backspace"},
    {Key::Delete, "Del"}, {Key::Left, "Left"}, {Key::Right, "Right"}, {Key::Up, "Up"}, {Key::Down, "Down"},
    {Key::Home, "Home"}, {Key::End, "End"}, {Key::PageUp, "PageUp"}, {Key::PageDown, "PageDown"},
    {Key::A, "A"}, {Key::B, "B"}, {Key::C, "C"}, {Key::D, "D"}, {Key::E, "E"}, {Key::F, "F"}, {Key::G, "G"},
    {Key::H, "H"}, {Key::I, "I"}, {Key::J, "J"}, {Key::K, "K"}, {Key::L, "L"}, {Key::M, "M"}, {Key::N, "N"},
    {Key::O, "O"}, {Key::P, "P"}, {Key::Q, "Q"}, {Key::R, "R"}, {Key::S, "S"}, {Key::T, "T"}, {Key::U, "U"},
    {Key::V, "V"}, {Key::W, "W"}, {Key::X, "X"}, {Key::Y, "Y"}, {Key::Z, "Z"},
    {Key::Num0, "0"}, {Key::Num1, "1"}, {Key::Num2, "2"}, {Key::Num3, "3"}, {Key::Num4, "4"}, {Key::Num5, "5"},
    {Key::Num6, "6"}, {Key::Num7, "7"}, {Key::Num8, "8"}, {Key::Num9, "9"},
    {Key::F1, "F1"}, {Key::F2, "F2"}, {Key::F3, "F3"}, {Key::F4, "F4"}, {Key::F5, "F5"}, {Key::F6, "F6"},
    {Key::F7, "F7"}, {Key::F8, "F8"}, {Key::F9, "F9"}, {Key::F10, "F10"}, {Key::F11, "F11"}, {Key::F12, "F12"},
    {Key::BracketLeft, "["}, {Key::BracketRight, "]"},
};
constexpr KeyNameEntry kKeyAliases[] = {
    {Key::Escape, "Escape"}, {Key::Delete, "Delete"}, {Key::Enter, "Return"}, {Key::PageUp, "PgUp"}, {Key::PageDown, "PgDown"},
    {Key::Backspace, "Back"},
};
// clang-format on

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

#if defined(__APPLE__)
constexpr u8 kPlatformPrimary = kModSuper;
constexpr std::string_view kSuperName = "Cmd";
#else
constexpr u8 kPlatformPrimary = kModCtrl;
constexpr std::string_view kSuperName = "Super";
#endif
u8 g_primaryOverride = 0;

/// Keys a text field needs for itself when no Ctrl / Alt / Super is held.
bool typingKey(Key k) {
    switch (k) {
    case Key::Escape: case Key::Tab: case Key::None: case Key::Other:
    case Key::F1: case Key::F2: case Key::F3: case Key::F4: case Key::F5: case Key::F6: case Key::F7: case Key::F8:
    case Key::F9: case Key::F10: case Key::F11: case Key::F12:
        return false;
    default: return true; // letters, digits, punctuation, space, Enter, Backspace / Delete, caret keys
    }
}
bool inside(const UiContext& ui, NodeHandle n, NodeHandle ancestor, u32* depth = nullptr) {
    u32 d = 0;
    for (NodeHandle a = n; ui.alive(a); a = ui.parent(a), ++d)
        if (a == ancestor) {
            if (depth) *depth = d;
            return true;
        }
    return false;
}
bool hookFn(void* user, UiContext&, Key k, u8 mods) { return static_cast<ActionRegistry*>(user)->handleKey(k, mods); }
} // namespace

u8 primaryModifier() { return g_primaryOverride ? g_primaryOverride : kPlatformPrimary; }
void setPrimaryModifier(u8 mod) { g_primaryOverride = mod; }

std::string_view keyName(Key k) {
    for (const auto& e : kKeyNames)
        if (e.key == k) return e.name;
    return {};
}
bool keyFromName(std::string_view name, Key& out) {
    for (const auto& e : kKeyNames)
        if (ieq(e.name, name)) { out = e.key; return true; }
    for (const auto& e : kKeyAliases)
        if (ieq(e.name, name)) { out = e.key; return true; }
    return false;
}

std::string shortcutToString(Shortcut s) {
    std::string out;
    const std::string_view key = keyName(s.key);
    if (key.empty()) return out;
    if (s.mods & kModCtrl) out += "Ctrl+";
    if (s.mods & kModAlt) out += "Alt+";
    if (s.mods & kModShift) out += "Shift+";
    if (s.mods & kModSuper) { out += kSuperName; out += '+'; }
    out += key;
    return out;
}

bool parseShortcut(std::string_view text, Shortcut& out) {
    Shortcut s;
    text = trim(text);
    if (text.empty()) return false;
    for (;;) {
        // the key may itself be "+": not in the key set, so a '+' always separates
        const std::size_t plus = text.find('+');
        const std::string_view tok = trim(text.substr(0, plus));
        if (plus == std::string_view::npos) {
            if (!keyFromName(tok, s.key)) return false;
            break;
        }
        if (ieq(tok, "ctrl") || ieq(tok, "control")) s.mods |= kModCtrl;
        else if (ieq(tok, "shift")) s.mods |= kModShift;
        else if (ieq(tok, "alt") || ieq(tok, "option")) s.mods |= kModAlt;
        else if (ieq(tok, "super") || ieq(tok, "cmd") || ieq(tok, "command") || ieq(tok, "meta") || ieq(tok, "win")) s.mods |= kModSuper;
        else if (ieq(tok, "primary") || ieq(tok, "cmdorctrl")) s.mods |= primaryModifier();
        else return false;
        text.remove_prefix(plus + 1);
    }
    out = s;
    return true;
}

// ---- Registry -------------------------------------------------------------------------------------------------------
ActionRegistry::ActionRegistry(UiContext& ui) : m_ui(ui) { m_ui.setShortcutHook({&hookFn, this}); }
ActionRegistry::~ActionRegistry() {
    if (m_ui.shortcutHook().user == this) m_ui.setShortcutHook({});
    for (const Binding& b : m_bindings) m_ui.disconnect(b.conn);
}

const ActionRegistry::Action* ActionRegistry::get(ActionHandle a) const {
    if (a.index >= m_actions.size()) return nullptr;
    const Action& x = m_actions[a.index];
    return x.alive && x.generation == a.generation ? &x : nullptr;
}

ActionHandle ActionRegistry::add(const ActionDesc& d, ActionFn fn) {
    if (d.id.empty() || find(d.id).valid()) return {};
    u32 i = 0;
    while (i < m_actions.size() && m_actions[i].alive) ++i;
    if (i == m_actions.size()) m_actions.emplace_back();
    Action& x = m_actions[i];
    const u32 gen = x.generation + 1;
    x = Action{};
    x.generation = gen;
    x.alive = true;
    x.id.assign(d.id);
    x.text.assign(d.text);
    x.shortcut = d.shortcut.valid() ? d.shortcut : Shortcut{};
    x.enabled = d.enabled;
    x.checkable = d.checkable;
    x.checked = d.checkable && d.checked;
    x.scope = d.scope;
    x.scopeNode = d.scopeNode;
    x.fn = fn;
    return {i, gen};
}

void ActionRegistry::remove(ActionHandle a) {
    Action* x = get(a);
    if (!x) return;
    x->alive = false;
    for (Binding& b : m_bindings)
        if (b.action == a && !b.observer) {
            m_ui.disconnect(b.conn);
            b.node = {};
        }
    changed(a);
}

ActionHandle ActionRegistry::find(std::string_view id) const {
    for (u32 i = 0; i < m_actions.size(); ++i)
        if (m_actions[i].alive && m_actions[i].id == id) return {i, m_actions[i].generation};
    return {};
}
u32 ActionRegistry::count() const {
    u32 n = 0;
    for (const Action& x : m_actions) n += x.alive;
    return n;
}

std::string_view ActionRegistry::id(ActionHandle a) const { const Action* x = get(a); return x ? std::string_view(x->id) : std::string_view{}; }
std::string_view ActionRegistry::text(ActionHandle a) const { const Action* x = get(a); return x ? std::string_view(x->text) : std::string_view{}; }
Shortcut ActionRegistry::shortcut(ActionHandle a) const { const Action* x = get(a); return x ? x->shortcut : Shortcut{}; }
bool ActionRegistry::enabled(ActionHandle a) const { const Action* x = get(a); return x && x->enabled; }
bool ActionRegistry::checkable(ActionHandle a) const { const Action* x = get(a); return x && x->checkable; }
bool ActionRegistry::checked(ActionHandle a) const { const Action* x = get(a); return x && x->checked; }

void ActionRegistry::setText(ActionHandle a, std::string_view t) {
    Action* x = get(a);
    if (!x || x->text == t) return;
    x->text.assign(t);
    changed(a);
}
void ActionRegistry::setEnabled(ActionHandle a, bool on) {
    Action* x = get(a);
    if (!x || x->enabled == on) return;
    x->enabled = on;
    changed(a);
}
void ActionRegistry::setChecked(ActionHandle a, bool on) {
    Action* x = get(a);
    if (!x || !x->checkable || x->checked == on) return;
    x->checked = on;
    changed(a);
}
void ActionRegistry::setTriggered(ActionHandle a, ActionFn fn) {
    if (Action* x = get(a)) x->fn = fn;
}

bool ActionRegistry::trigger(ActionHandle a) {
    Action* x = get(a);
    if (!x || !x->enabled) {
        if (x) changed(a); // a widget that toggled itself on the click goes back to the action's state
        return false;
    }
    if (x->checkable) x->checked = !x->checked;
    const bool checked = x->checked;
    const ActionFn fn = x->fn; // the callback may add / remove actions
    changed(a);
    if (fn) fn(m_ui, a, checked);
    return true;
}

ActionHandle ActionRegistry::match(Key k, u8 mods) const {
    mods &= u8(kModShift | kModCtrl | kModAlt | kModSuper);
    const NodeHandle focus = m_ui.focused(), modal = m_ui.modal();
    const bool hasFocus = m_ui.alive(focus), isModal = m_ui.alive(modal);
    // a text field keeps plain typing for itself
    if (hasFocus && !(mods & (kModCtrl | kModAlt | kModSuper)) && typingKey(k) &&
        (m_ui.wantsTextInput() || m_ui.role(focus) == A11yRole::TextField))
        return {};
    ActionHandle best;
    u32 bestDepth = 0xFFFFFFFFu;
    for (u32 i = 0; i < m_actions.size(); ++i) {
        const Action& x = m_actions[i];
        if (!x.alive || !x.enabled || x.shortcut.key != k || x.shortcut.mods != mods || !x.shortcut.valid()) continue;
        u32 depth = 0xFFFFFFFEu; // window scope: after every subtree match
        if (x.scope == ActionScope::Subtree) {
            if (!hasFocus || !inside(m_ui, focus, x.scopeNode, &depth)) continue;
        } else if (isModal && !(m_ui.alive(x.scopeNode) && inside(m_ui, x.scopeNode, modal))) {
            continue; // a modal scope blocks the window's actions
        } else if (!isModal && x.scopeNode.valid() && !m_ui.alive(x.scopeNode)) {
            continue; // its owner is gone
        }
        if (depth < bestDepth) {
            bestDepth = depth;
            best = {i, x.generation};
        }
    }
    return best;
}

bool ActionRegistry::handleKey(Key k, u8 mods) {
    const ActionHandle a = match(k, mods);
    return a.valid() && trigger(a);
}

ActionHandle ActionRegistry::conflict(ActionHandle a, Shortcut s) const {
    const Action* x = get(a);
    if (!x || !s.valid()) return {};
    for (u32 i = 0; i < m_actions.size(); ++i) {
        const Action& y = m_actions[i];
        if (!y.alive || i == a.index || !(y.shortcut == s)) continue;
        if (y.scope == x->scope && y.scopeNode == x->scopeNode) return {i, y.generation};
    }
    return {};
}

std::expected<void, ActionHandle> ActionRegistry::rebind(ActionHandle a, Shortcut s, bool steal) {
    Action* x = get(a);
    if (!x) return std::unexpected(ActionHandle{});
    if (!s.valid()) s = {};
    if (const ActionHandle other = conflict(a, s); other.valid()) {
        if (!steal) return std::unexpected(other);
        m_actions[other.index].shortcut = {};
        changed(other);
    }
    if (!(x->shortcut == s)) {
        x->shortcut = s;
        changed(a);
    }
    return {};
}

std::string ActionRegistry::saveShortcuts() const {
    std::string out;
    for (const Action& x : m_actions) {
        if (!x.alive) continue;
        out += x.id;
        out += '=';
        out += shortcutToString(x.shortcut);
        out += '\n';
    }
    return out;
}

u32 ActionRegistry::loadShortcuts(std::string_view text) {
    u32 applied = 0;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        const std::string_view line = trim(text.substr(0, nl));
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const ActionHandle a = find(trim(line.substr(0, eq)));
        const std::string_view value = trim(line.substr(eq + 1));
        Shortcut s;
        if (!a.valid() || (!value.empty() && !parseShortcut(value, s))) continue;
        if (rebind(a, s, true)) ++applied;
    }
    return applied;
}

// ---- Bindings -------------------------------------------------------------------------------------------------------
void ActionRegistry::sync(const Binding& b) {
    if (!m_ui.alive(b.node)) return;
    if (b.observer) {
        b.observer(*this, b.node, b.user);
        return;
    }
    const Action* x = get(b.action);
    if (!x) return;
    if (m_ui.text(b.node) != x->text) m_ui.setText(b.node, x->text);
    m_ui.setDisabled(b.node, !x->enabled);
    m_ui.setChecked(b.node, x->checkable && x->checked);
}

void ActionRegistry::changed(ActionHandle a) {
    // index loop: an observer may bind / observe more nodes
    for (std::size_t i = 0; i < m_bindings.size(); ++i) {
        const Binding b = m_bindings[i];
        if (b.observer || b.action == a) sync(b);
    }
}

void ActionRegistry::bind(ActionHandle a, NodeHandle widget) {
    if (!get(a) || !m_ui.alive(widget)) return;
    unbind(widget);
    Binding* slot = nullptr;
    for (Binding& b : m_bindings)
        if (!m_ui.alive(b.node)) { slot = &b; break; }
    if (!slot) slot = &m_bindings.emplace_back();
    *slot = Binding{};
    slot->node = widget;
    slot->action = a;
    ActionRegistry* self = this;
    slot->conn = m_ui.connect(widget, SignalKind::Clicked, [self, a](UiContext&, NodeHandle, const SignalArgs&) { self->trigger(a); });
    sync(*slot);
}

void ActionRegistry::unbind(NodeHandle widget) {
    for (Binding& b : m_bindings)
        if (b.node == widget) {
            m_ui.disconnect(b.conn);
            b = Binding{};
        }
}

u32 ActionRegistry::addList(std::initializer_list<ActionHandle> actions) { return addList(actions.begin(), u32(actions.size())); }
u32 ActionRegistry::addList(const ActionHandle* actions, u32 n) {
    m_lists.emplace_back(actions, actions + n);
    return u32(m_lists.size() - 1);
}
const std::vector<ActionHandle>& ActionRegistry::list(u32 listId) const {
    static const std::vector<ActionHandle> empty;
    return listId < m_lists.size() ? m_lists[listId] : empty;
}

void ActionRegistry::observe(NodeHandle node, ObserverFn fn, u64 user) {
    if (!m_ui.alive(node) || !fn) return;
    Binding* slot = nullptr;
    for (Binding& b : m_bindings)
        if (!m_ui.alive(b.node)) { slot = &b; break; }
    if (!slot) slot = &m_bindings.emplace_back();
    *slot = Binding{};
    slot->node = node;
    slot->observer = fn;
    slot->user = user;
}

} // namespace fuse::ui
