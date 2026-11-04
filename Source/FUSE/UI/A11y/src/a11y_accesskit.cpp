// AccessKit backend for fuse_ui_a11y (built when FUSE_UI_ACCESSKIT_DIR / FUSE_UI_ACCESSKIT_BUILD provide accesskit-c).
#include "fuse/ui/a11y/a11y.hpp"

#if FUSE_UI_HAVE_ACCESSKIT
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "accesskit.h"

namespace fuse::ui::a11y {

bool accessKitAvailable() { return true; }

namespace {
accesskit_role roleOf(A11yRole r, Kind k) {
    switch (k) {
    case Kind::Menu: return ACCESSKIT_ROLE_MENU;
    case Kind::MenuBar: return ACCESSKIT_ROLE_MENU_BAR;
    case Kind::MenuItem: return ACCESSKIT_ROLE_MENU_ITEM;
    case Kind::Tree: return ACCESSKIT_ROLE_TREE;
    case Kind::Plain: break;
    }
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
bool selectable(A11yRole r) { return r == A11yRole::ListItem || r == A11yRole::Tab; }

/// AccessKit action request -> bridge request (false: unsupported). Frees `req`.
bool toRequest(accesskit_action_request* req, ActionRequest& r) {
    r.target = req->target_node;
    bool ok = true;
    switch (req->action) {
    case ACCESSKIT_ACTION_FOCUS: r.action = Action::Focus; break;
    case ACCESSKIT_ACTION_CLICK: r.action = Action::Click; break;
    case ACCESSKIT_ACTION_INCREMENT: r.action = Action::Increment; break;
    case ACCESSKIT_ACTION_DECREMENT: r.action = Action::Decrement; break;
    case ACCESSKIT_ACTION_EXPAND: r.action = Action::Expand; break;
    case ACCESSKIT_ACTION_COLLAPSE: r.action = Action::Collapse; break;
    case ACCESSKIT_ACTION_SCROLL_INTO_VIEW: r.action = Action::ScrollIntoView; break;
    case ACCESSKIT_ACTION_SET_VALUE:
        r.action = Action::SetValue;
        if (req->data.has_value && req->data.value.tag == ACCESSKIT_ACTION_DATA_NUMERIC_VALUE) {
            r.value = f32(req->data.value.numeric_value);
        } else if (req->data.has_value && req->data.value.tag == ACCESSKIT_ACTION_DATA_VALUE) {
            r.action = Action::SetText;
            r.text = req->data.value.value ? req->data.value.value : "";
        } else {
            ok = false;
        }
        break;
    default: ok = false; break;
    }
    accesskit_action_request_free(req);
    return ok;
}
} // namespace

void* buildAccessKitUpdate(const Bridge& b, bool includeTree) {
    const auto& v = b.nodes();
    const auto& ex = b.extras();
    accesskit_tree_update* u = accesskit_tree_update_with_capacity_and_focus(v.size(), b.focusId());
    std::vector<accesskit_node_id> kids;
    for (u32 i = 0; i < v.size(); ++i) {
        const A11ySnapshotNode& s = v[i];
        const NodeExtra x = i < ex.size() ? ex[i] : NodeExtra{};
        accesskit_node* n = accesskit_node_new(roleOf(s.role, x.kind));
        const bool text = s.role == A11yRole::TextField;
        if (!s.name.empty() && (!text || x.labelled)) accesskit_node_set_label_with_length(n, s.name.data(), s.name.size());
        accesskit_node_set_bounds(n, {double(s.bounds.x), double(s.bounds.y), double(s.bounds.x + s.bounds.w), double(s.bounds.y + s.bounds.h)});
        if (i == 0 && b.scale() != 1.f) {
            const double k = double(b.scale());
            accesskit_node_set_transform(n, accesskit_affine{{k, 0.0, 0.0, k, 0.0, 0.0}});
        }
        if (i != 0) accesskit_node_add_action(n, ACCESSKIT_ACTION_SCROLL_INTO_VIEW);
        if (s.disabled) accesskit_node_set_disabled(n);
        if (s.focusable) accesskit_node_add_action(n, ACCESSKIT_ACTION_FOCUS);
        if (s.focusable && !ranged(s.role) && !text) accesskit_node_add_action(n, ACCESSKIT_ACTION_CLICK);
        if (toggles(s.role)) accesskit_node_set_toggled(n, s.checked ? ACCESSKIT_TOGGLED_TRUE : ACCESSKIT_TOGGLED_FALSE);
        if (x.kind == Kind::Plain && selectable(s.role)) accesskit_node_set_selected(n, x.selected);
        if (text) {
            accesskit_node_set_value_with_length(n, x.text.data(), x.text.size());
            if (!s.disabled) accesskit_node_add_action(n, ACCESSKIT_ACTION_SET_VALUE);
        }
        if (x.expanded >= 0) {
            accesskit_node_set_expanded(n, x.expanded == 1);
            accesskit_node_set_has_popup(n, s.role == A11yRole::ComboBox ? ACCESSKIT_HAS_POPUP_LISTBOX : ACCESSKIT_HAS_POPUP_MENU);
            accesskit_node_add_action(n, ACCESSKIT_ACTION_EXPAND);
            accesskit_node_add_action(n, ACCESSKIT_ACTION_COLLAPSE);
        }
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

namespace {
accesskit_tree_update* activate(void* user) {
    Bridge* b = static_cast<Bridge*>(user);
    // may run on an adapter thread (AT-SPI): build from the last snapshot (the UI thread refreshes it in update())
    auto lock = b->lockSnapshot();
    return static_cast<accesskit_tree_update*>(buildAccessKitUpdate(*b, true));
}
accesskit_tree_update* factory(void* user) { return static_cast<accesskit_tree_update*>(buildAccessKitUpdate(*static_cast<Bridge*>(user), false)); }
} // namespace

#if defined(__linux__)
namespace {
void onAction(accesskit_action_request* req, void* user) {
    ActionRequest r;
    if (toRequest(req, r)) static_cast<Bridge*>(user)->enqueue(r);
}
void onDeactivate(void*) {}
} // namespace

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

#if defined(_WIN32)
namespace {
// UIA calls providers on its own threads: queue the action and wake the window's message loop.
void onWinAction(accesskit_action_request* req, void* user) {
    auto* self = static_cast<WindowsAdapter*>(user);
    ActionRequest r;
    if (!toRequest(req, r)) return;
    self->bridge().enqueue(r);
    PostMessageW(static_cast<HWND>(self->hwnd()), WM_NULL, 0, 0);
}
void raise(accesskit_windows_queued_events* e) {
    if (e) accesskit_windows_queued_events_raise(e); // frees
}
} // namespace

WindowsAdapter::WindowsAdapter(Bridge& b, void* hwnd) : m_bridge(b), m_hwnd(hwnd) {
    m_bridge.sync();
    m_focused = GetFocus() == static_cast<HWND>(hwnd);
    m_adapter = accesskit_windows_adapter_new(static_cast<HWND>(hwnd), m_focused, onWinAction, this);
}
WindowsAdapter::~WindowsAdapter() {
    if (m_adapter) accesskit_windows_adapter_free(static_cast<accesskit_windows_adapter*>(m_adapter));
}
bool WindowsAdapter::handleMessage(u32 msg, std::uintptr_t wp, std::intptr_t lp, std::intptr_t& result) {
    if (!m_adapter) return false;
    auto* ad = static_cast<accesskit_windows_adapter*>(m_adapter);
    switch (msg) {
    case WM_GETOBJECT: {
        const accesskit_opt_lresult r = accesskit_windows_adapter_handle_wm_getobject(ad, WPARAM(wp), LPARAM(lp), activate, &m_bridge);
        if (!r.has_value) return false;
        result = std::intptr_t(r.value);
        return true;
    }
    case WM_SETFOCUS: setWindowFocused(true); return false;
    case WM_KILLFOCUS: setWindowFocused(false); return false;
    default: return false;
    }
}
void WindowsAdapter::update() {
    m_bridge.drain();
    RECT rc{};
    const Size vp = m_bridge.ui().viewport();
    if (m_bridge.ui().scale() != 1.f) m_bridge.setScale(m_bridge.ui().scale()); // DPI-scaled context: exact
    else if (GetClientRect(static_cast<HWND>(m_hwnd), &rc) && rc.right > 0 && vp.w > 0.f) m_bridge.setScale(f32(rc.right) / vp.w);
    if (m_bridge.sync() && m_adapter)
        raise(accesskit_windows_adapter_update_if_active(static_cast<accesskit_windows_adapter*>(m_adapter), factory, &m_bridge));
}
void WindowsAdapter::setWindowFocused(bool focused) {
    if (!m_adapter || focused == m_focused) return;
    m_focused = focused;
    raise(accesskit_windows_adapter_update_window_focus_state(static_cast<accesskit_windows_adapter*>(m_adapter), focused));
}
#endif

} // namespace fuse::ui::a11y

#else
namespace fuse::ui::a11y {
bool accessKitAvailable() { return false; }
} // namespace fuse::ui::a11y
#endif
