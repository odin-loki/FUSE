// AccessKit backend for fuse_ui_a11y (built when FUSE_UI_ACCESSKIT_DIR points at an accesskit-c build).
#include "fuse/ui/a11y/a11y.hpp"

#if FUSE_UI_HAVE_ACCESSKIT
#include "accesskit.h"

// The roles, states and actions added with the accessibility completeness work (menus, trees, tables, expanded /
// selected / live, value text, expand / collapse / scroll actions) use accesskit-c API beyond what the first backend
// called. They were written against accesskit-c 0.22 but could not be compiled where they were written: if your
// accesskit-c lacks one of them, build with -DFUSE_UI_ACCESSKIT_EXTENDED=0 to get the original API surface (the new
// roles then map to the nearest old one and the extra states / actions are not reported).
#ifndef FUSE_UI_ACCESSKIT_EXTENDED
#define FUSE_UI_ACCESSKIT_EXTENDED 1
#endif

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
#if FUSE_UI_ACCESSKIT_EXTENDED
    case A11yRole::Menu: return ACCESSKIT_ROLE_MENU;
    case A11yRole::MenuItem: return ACCESSKIT_ROLE_MENU_ITEM;
    case A11yRole::MenuBar: return ACCESSKIT_ROLE_MENU_BAR;
    case A11yRole::Dialog: return ACCESSKIT_ROLE_DIALOG;
    case A11yRole::Tree: return ACCESSKIT_ROLE_TREE;
    case A11yRole::TreeItem: return ACCESSKIT_ROLE_TREE_ITEM;
    case A11yRole::Table: return ACCESSKIT_ROLE_TABLE;
    case A11yRole::Row: return ACCESSKIT_ROLE_ROW;
    case A11yRole::Cell: return ACCESSKIT_ROLE_CELL;
    case A11yRole::ColumnHeader: return ACCESSKIT_ROLE_COLUMN_HEADER;
    case A11yRole::TabList: return ACCESSKIT_ROLE_TAB_LIST;
    case A11yRole::TabPanel: return ACCESSKIT_ROLE_TAB_PANEL;
    case A11yRole::ToolBar: return ACCESSKIT_ROLE_TOOLBAR;
    case A11yRole::StatusBar: return ACCESSKIT_ROLE_STATUS;
    case A11yRole::Tooltip: return ACCESSKIT_ROLE_TOOLTIP;
    case A11yRole::Alert: return ACCESSKIT_ROLE_ALERT;
#else // the roles the original backend used: the nearest of them
    case A11yRole::Menu: case A11yRole::Tree: case A11yRole::Table: case A11yRole::TabList: return ACCESSKIT_ROLE_LIST;
    case A11yRole::MenuItem: case A11yRole::TreeItem: case A11yRole::Row: return ACCESSKIT_ROLE_LIST_ITEM;
    case A11yRole::Cell: case A11yRole::ColumnHeader: case A11yRole::Tooltip: case A11yRole::Alert: return ACCESSKIT_ROLE_LABEL;
    case A11yRole::Dialog: return ACCESSKIT_ROLE_WINDOW;
    case A11yRole::MenuBar: case A11yRole::TabPanel: case A11yRole::ToolBar: case A11yRole::StatusBar: return ACCESSKIT_ROLE_GROUP;
#endif
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
        accesskit_node* n = accesskit_node_new(roleOf(s.role));
        if (!s.name.empty()) accesskit_node_set_label_with_length(n, s.name.data(), s.name.size());
        accesskit_node_set_bounds(n, {double(s.bounds.x), double(s.bounds.y), double(s.bounds.x + s.bounds.w), double(s.bounds.y + s.bounds.h)});
        if (s.disabled) accesskit_node_set_disabled(n);
        // the actions the UI accepts for this node (UiContext::performA11yAction), as the snapshot lists them
        if (s.hasAction(A11yAction::Focus)) accesskit_node_add_action(n, ACCESSKIT_ACTION_FOCUS);
        if (s.hasAction(A11yAction::Click) || s.hasAction(A11yAction::Select)) accesskit_node_add_action(n, ACCESSKIT_ACTION_CLICK); // (Select arrives as Click)
        if (s.hasAction(A11yAction::Increment)) accesskit_node_add_action(n, ACCESSKIT_ACTION_INCREMENT);
        if (s.hasAction(A11yAction::Decrement)) accesskit_node_add_action(n, ACCESSKIT_ACTION_DECREMENT);
        if (s.hasAction(A11yAction::SetValue)) accesskit_node_add_action(n, ACCESSKIT_ACTION_SET_VALUE);
#if FUSE_UI_ACCESSKIT_EXTENDED
        if (s.hasAction(A11yAction::Expand)) accesskit_node_add_action(n, ACCESSKIT_ACTION_EXPAND);
        if (s.hasAction(A11yAction::Collapse)) accesskit_node_add_action(n, ACCESSKIT_ACTION_COLLAPSE);
        if (s.hasAction(A11yAction::ScrollUp)) accesskit_node_add_action(n, ACCESSKIT_ACTION_SCROLL_UP);
        if (s.hasAction(A11yAction::ScrollDown)) accesskit_node_add_action(n, ACCESSKIT_ACTION_SCROLL_DOWN);
        if (s.hasAction(A11yAction::ScrollLeft)) accesskit_node_add_action(n, ACCESSKIT_ACTION_SCROLL_LEFT);
        if (s.hasAction(A11yAction::ScrollRight)) accesskit_node_add_action(n, ACCESSKIT_ACTION_SCROLL_RIGHT);
        if (s.hasAction(A11yAction::ScrollIntoView)) accesskit_node_add_action(n, ACCESSKIT_ACTION_SCROLL_INTO_VIEW);
#endif
        if (toggles(s.role)) accesskit_node_set_toggled(n, s.checked ? ACCESSKIT_TOGGLED_TRUE : ACCESSKIT_TOGGLED_FALSE);
        if (ranged(s.role) && !s.child) {
            accesskit_node_set_numeric_value(n, double(s.value));
            accesskit_node_set_min_numeric_value(n, double(s.lo));
            accesskit_node_set_max_numeric_value(n, double(s.hi));
        }
#if FUSE_UI_ACCESSKIT_EXTENDED
        if (!s.valueText.empty()) accesskit_node_set_value_with_length(n, s.valueText.data(), s.valueText.size());
        if (s.expandable) accesskit_node_set_expanded(n, s.expanded);
        if (s.selectable) accesskit_node_set_selected(n, s.selected);
        if (s.modal) accesskit_node_set_modal(n);
        if (s.live != A11yLive::Off) accesskit_node_set_live(n, s.live == A11yLive::Assertive ? ACCESSKIT_LIVE_ASSERTIVE : ACCESSKIT_LIVE_POLITE);
        if (s.level) accesskit_node_set_level(n, s.level);
        if (s.posInSet) {
            accesskit_node_set_position_in_set(n, s.posInSet);
            accesskit_node_set_size_of_set(n, s.setSize);
        }
#endif
        kids.clear();
        for (u32 c : b.children(i)) kids.push_back(idOf(v[c]));
        if (!kids.empty()) accesskit_node_set_children(n, kids.size(), kids.data());
        accesskit_tree_update_push_node(u, idOf(s), n); // takes ownership
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

#if defined(__linux__)
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
#if FUSE_UI_ACCESSKIT_EXTENDED
    case ACCESSKIT_ACTION_EXPAND: r.action = Action::Expand; break;
    case ACCESSKIT_ACTION_COLLAPSE: r.action = Action::Collapse; break;
    case ACCESSKIT_ACTION_SCROLL_UP: r.action = Action::ScrollUp; break;
    case ACCESSKIT_ACTION_SCROLL_DOWN: r.action = Action::ScrollDown; break;
    case ACCESSKIT_ACTION_SCROLL_LEFT: r.action = Action::ScrollLeft; break;
    case ACCESSKIT_ACTION_SCROLL_RIGHT: r.action = Action::ScrollRight; break;
    case ACCESSKIT_ACTION_SCROLL_INTO_VIEW: r.action = Action::ScrollIntoView; break;
#endif
    case ACCESSKIT_ACTION_SET_VALUE:
        r.action = Action::SetValue;
        if (req->data.has_value && req->data.value.tag == ACCESSKIT_ACTION_DATA_NUMERIC_VALUE) r.value = f32(req->data.value.numeric_value);
#if FUSE_UI_ACCESSKIT_EXTENDED
        else if (req->data.has_value && req->data.value.tag == ACCESSKIT_ACTION_DATA_VALUE && req->data.value.value) r.text = req->data.value.value;
#endif
        break;
    default: accesskit_action_request_free(req); return;
    }
    accesskit_action_request_free(req);
    b->enqueue(r);
}
void onDeactivate(void*) {}
accesskit_tree_update* factory(void* user) { return static_cast<accesskit_tree_update*>(buildAccessKitUpdate(*static_cast<Bridge*>(user), false)); }
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

} // namespace fuse::ui::a11y

#else
namespace fuse::ui::a11y {
bool accessKitAvailable() { return false; }
} // namespace fuse::ui::a11y
#endif
