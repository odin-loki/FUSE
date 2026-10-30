// AccessKit backend for fuse_ui_a11y (built when FUSE_UI_ACCESSKIT_DIR points at an accesskit-c build).
#include "fuse/ui/a11y/a11y.hpp"

#if FUSE_UI_HAVE_ACCESSKIT
#if defined(_WIN32)
#include <windows.h>
#endif
#include "accesskit.h"

namespace fuse::ui::a11y {

bool accessKitAvailable() { return true; }

namespace {
accesskit_role roleOf(A11yRole r) {
    switch (r) {
    case A11yRole::Window: return ACCESSKIT_ROLE_WINDOW;
    case A11yRole::Group: return ACCESSKIT_ROLE_GROUP;
    case A11yRole::Text: return ACCESSKIT_ROLE_LABEL;
    case A11yRole::Button: return ACCESSKIT_ROLE_BUTTON;
    case A11yRole::CheckBox: return ACCESSKIT_ROLE_CHECK_BOX;
    case A11yRole::RadioButton: return ACCESSKIT_ROLE_RADIO_BUTTON;
    case A11yRole::Switch: return ACCESSKIT_ROLE_SWITCH;
    case A11yRole::Slider: case A11yRole::Dial: return ACCESSKIT_ROLE_SLIDER;
    case A11yRole::ProgressBar: return ACCESSKIT_ROLE_PROGRESS_INDICATOR;
    case A11yRole::TextField: return ACCESSKIT_ROLE_TEXT_INPUT;
    case A11yRole::ScrollArea: return ACCESSKIT_ROLE_SCROLL_VIEW;
    case A11yRole::List: return ACCESSKIT_ROLE_LIST;
    case A11yRole::ListItem: return ACCESSKIT_ROLE_LIST_ITEM;
    case A11yRole::Image: return ACCESSKIT_ROLE_IMAGE;
    case A11yRole::Separator: return ACCESSKIT_ROLE_SPLITTER;
    case A11yRole::Link: return ACCESSKIT_ROLE_LINK;
    case A11yRole::ComboBox: return ACCESSKIT_ROLE_COMBO_BOX;
    case A11yRole::SpinBox: return ACCESSKIT_ROLE_SPIN_BUTTON;
    case A11yRole::ScrollBar: return ACCESSKIT_ROLE_SCROLL_BAR;
    case A11yRole::Tab: return ACCESSKIT_ROLE_TAB;
    case A11yRole::None: break;
    }
    return ACCESSKIT_ROLE_UNKNOWN;
}
bool ranged(A11yRole r) {
    return r == A11yRole::Slider || r == A11yRole::Dial || r == A11yRole::ProgressBar || r == A11yRole::SpinBox || r == A11yRole::ScrollBar;
}
bool toggles(A11yRole r) { return r == A11yRole::CheckBox || r == A11yRole::RadioButton || r == A11yRole::Switch; }
} // namespace

void* buildAccessKitUpdate(const Bridge& b, bool includeTree) {
    const auto& v = b.nodes();
    accesskit_tree_update* u = accesskit_tree_update_with_capacity_and_focus(v.size(), b.focusId());
    std::vector<accesskit_node_id> kids;
    for (u32 i = 0; i < v.size(); ++i) {
        const A11ySnapshotNode& s = v[i];
        // protected fields: PasswordInput (UIA IsPassword = true, AT-SPI ROLE_PASSWORD_TEXT, NSSecureTextField)
        accesskit_node* n = accesskit_node_new(s.protectedText && s.role == A11yRole::TextField ? ACCESSKIT_ROLE_PASSWORD_INPUT : roleOf(s.role));
        if (!s.name.empty()) accesskit_node_set_label_with_length(n, s.name.data(), s.name.size());
        accesskit_node_set_bounds(n, {double(s.bounds.x), double(s.bounds.y), double(s.bounds.x + s.bounds.w), double(s.bounds.y + s.bounds.h)});
        if (s.disabled) accesskit_node_set_disabled(n);
        if (s.focusable) accesskit_node_add_action(n, ACCESSKIT_ACTION_FOCUS);
        if (s.focusable && !ranged(s.role) && s.role != A11yRole::TextField) accesskit_node_add_action(n, ACCESSKIT_ACTION_CLICK);
        if (s.state.expanded >= 0) accesskit_node_set_expanded(n, s.state.expanded > 0);
        if (s.role == A11yRole::ListItem) accesskit_node_set_selected(n, s.state.selected);
        if (s.state.autoComplete != A11yAutoComplete::None)
            accesskit_node_set_auto_complete(n, s.state.autoComplete == A11yAutoComplete::Inline ? ACCESSKIT_AUTO_COMPLETE_INLINE
                                                : s.state.autoComplete == A11yAutoComplete::List ? ACCESSKIT_AUTO_COMPLETE_LIST
                                                                                                  : ACCESSKIT_AUTO_COMPLETE_BOTH);
        if (s.state.activeDescendant.valid()) accesskit_node_set_active_descendant(n, idOf(s.state.activeDescendant));
        if (s.state.controls.valid()) { const accesskit_node_id c = idOf(s.state.controls); accesskit_node_set_controls(n, 1, &c); }
        if (toggles(s.role)) accesskit_node_set_toggled(n, s.checked ? ACCESSKIT_TOGGLED_TRUE : ACCESSKIT_TOGGLED_FALSE);
        if (ranged(s.role)) {
            accesskit_node_set_numeric_value(n, double(s.value));
            accesskit_node_set_min_numeric_value(n, double(s.lo));
            accesskit_node_set_max_numeric_value(n, double(s.hi));
            if (s.role != A11yRole::ProgressBar) {
                accesskit_node_add_action(n, ACCESSKIT_ACTION_INCREMENT);
                accesskit_node_add_action(n, ACCESSKIT_ACTION_DECREMENT);
                accesskit_node_add_action(n, ACCESSKIT_ACTION_SET_VALUE);
            }
        }
        kids.clear();
        for (u32 c : b.children(i)) kids.push_back(idOf(v[c].node));
        if (!kids.empty()) accesskit_node_set_children(n, kids.size(), kids.data());
        accesskit_tree_update_push_node(u, idOf(s.node), n); // takes ownership
    }
    if (includeTree && !v.empty()) {
        accesskit_tree* t = accesskit_tree_new(idOf(v[0].node));
        accesskit_tree_set_toolkit_name(t, "FUSE UI");
        accesskit_tree_set_toolkit_version(t, "0.1");
        accesskit_tree_update_set_tree(u, t);
    }
    return u;
}

std::string accessKitDebug(const void* update) {
    char* d = accesskit_tree_update_debug(static_cast<const accesskit_tree_update*>(update));
    std::string s = d ? d : "";
    accesskit_string_free(d);
    return s;
}
void freeAccessKitUpdate(void* update) { accesskit_tree_update_free(static_cast<accesskit_tree_update*>(update)); }

#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
namespace {
accesskit_tree_update* activate(void* user) {
    Bridge* b = static_cast<Bridge*>(user);
    // called on AccessKit's thread: build from the last snapshot (the UI thread refreshes it in update())
    auto lock = b->lockSnapshot();
    return static_cast<accesskit_tree_update*>(buildAccessKitUpdate(*b, true));
}
void onAction(accesskit_action_request* req, void* user) {
    Bridge* b = static_cast<Bridge*>(user);
    ActionRequest r;
    r.target = req->target_node;
    switch (req->action) {
    case ACCESSKIT_ACTION_FOCUS: r.action = Action::Focus; break;
    case ACCESSKIT_ACTION_CLICK: r.action = Action::Click; break;
    case ACCESSKIT_ACTION_INCREMENT: r.action = Action::Increment; break;
    case ACCESSKIT_ACTION_DECREMENT: r.action = Action::Decrement; break;
    case ACCESSKIT_ACTION_SET_VALUE:
        r.action = Action::SetValue;
        if (req->data.has_value && req->data.value.tag == ACCESSKIT_ACTION_DATA_NUMERIC_VALUE) r.value = f32(req->data.value.numeric_value);
        break;
    default: accesskit_action_request_free(req); return;
    }
    accesskit_action_request_free(req);
    b->enqueue(r);
}
void onDeactivate(void*) {}
accesskit_tree_update* factory(void* user) { return static_cast<accesskit_tree_update*>(buildAccessKitUpdate(*static_cast<Bridge*>(user), false)); }
} // namespace
#endif

#if defined(_WIN32)
const char* PlatformAdapter::backendName() { return "uia"; }
PlatformAdapter::PlatformAdapter(Bridge& b, void* hwnd, bool) : m_bridge(b) {
    m_bridge.sync();
    if (hwnd) m_adapter = accesskit_windows_subclassing_adapter_new(static_cast<HWND>(hwnd), activate, &m_bridge, onAction, &m_bridge);
}
PlatformAdapter::~PlatformAdapter() {
    if (m_adapter) accesskit_windows_subclassing_adapter_free(static_cast<accesskit_windows_subclassing_adapter*>(m_adapter));
}
void PlatformAdapter::update() {
    m_bridge.drain();
    if (m_bridge.sync() && m_adapter)
        if (auto* ev = accesskit_windows_subclassing_adapter_update_if_active(static_cast<accesskit_windows_subclassing_adapter*>(m_adapter), factory, &m_bridge))
            accesskit_windows_queued_events_raise(ev);
}
void PlatformAdapter::setWindowFocused(bool) {} // UIA tracks focus through the subclassed HWND
#elif defined(__APPLE__)
const char* PlatformAdapter::backendName() { return "nsaccessibility"; }
PlatformAdapter::PlatformAdapter(Bridge& b, void* w, bool isView) : m_bridge(b) {
    m_bridge.sync();
    if (w) m_adapter = isView ? accesskit_macos_subclassing_adapter_new(w, activate, &m_bridge, onAction, &m_bridge)
                              : accesskit_macos_subclassing_adapter_for_window(w, activate, &m_bridge, onAction, &m_bridge);
}
PlatformAdapter::~PlatformAdapter() {
    if (m_adapter) accesskit_macos_subclassing_adapter_free(static_cast<accesskit_macos_subclassing_adapter*>(m_adapter));
}
void PlatformAdapter::update() {
    m_bridge.drain();
    if (m_bridge.sync() && m_adapter)
        if (auto* ev = accesskit_macos_subclassing_adapter_update_if_active(static_cast<accesskit_macos_subclassing_adapter*>(m_adapter), factory, &m_bridge))
            accesskit_macos_queued_events_raise(ev);
}
void PlatformAdapter::setWindowFocused(bool f) {
    if (m_adapter)
        if (auto* ev = accesskit_macos_subclassing_adapter_update_view_focus_state(static_cast<accesskit_macos_subclassing_adapter*>(m_adapter), f))
            accesskit_macos_queued_events_raise(ev);
}
#elif defined(__linux__)
const char* PlatformAdapter::backendName() { return "atspi"; }
PlatformAdapter::PlatformAdapter(Bridge& b, void*, bool) : m_bridge(b) {
    m_bridge.sync();
    m_adapter = accesskit_unix_adapter_new(activate, &m_bridge, onAction, &m_bridge, onDeactivate, &m_bridge);
}
PlatformAdapter::~PlatformAdapter() {
    if (m_adapter) accesskit_unix_adapter_free(static_cast<accesskit_unix_adapter*>(m_adapter));
}
void PlatformAdapter::update() {
    m_bridge.drain();
    if (m_bridge.sync() && m_adapter)
        accesskit_unix_adapter_update_if_active(static_cast<accesskit_unix_adapter*>(m_adapter), factory, &m_bridge);
}
void PlatformAdapter::setWindowFocused(bool f) {
    if (m_adapter) accesskit_unix_adapter_update_window_focus_state(static_cast<accesskit_unix_adapter*>(m_adapter), f);
}
#endif

#if defined(__linux__)

UnixAdapter::UnixAdapter(Bridge& b) : m_bridge(b) {
    m_bridge.sync();
    m_adapter = accesskit_unix_adapter_new(activate, &m_bridge, onAction, &m_bridge, onDeactivate, &m_bridge);
}
UnixAdapter::~UnixAdapter() {
    if (m_adapter) accesskit_unix_adapter_free(static_cast<accesskit_unix_adapter*>(m_adapter));
}
void UnixAdapter::update() {
    m_bridge.drain();
    if (m_bridge.sync() && m_adapter)
        accesskit_unix_adapter_update_if_active(static_cast<accesskit_unix_adapter*>(m_adapter), factory, &m_bridge);
}
void UnixAdapter::setWindowFocused(bool focused) {
    if (m_adapter) accesskit_unix_adapter_update_window_focus_state(static_cast<accesskit_unix_adapter*>(m_adapter), focused);
}
#endif

} // namespace fuse::ui::a11y

#else
namespace fuse::ui::a11y {
bool accessKitAvailable() { return false; }
const char* PlatformAdapter::backendName() { return "none"; }
PlatformAdapter::PlatformAdapter(Bridge& b, void*, bool) : m_bridge(b) {}
PlatformAdapter::~PlatformAdapter() = default;
void PlatformAdapter::update() { m_bridge.drain(); m_bridge.sync(); }
void PlatformAdapter::setWindowFocused(bool) {}
} // namespace fuse::ui::a11y
#endif
