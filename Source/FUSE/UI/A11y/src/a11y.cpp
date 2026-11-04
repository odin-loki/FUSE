#include "fuse/ui/a11y/a11y.hpp"

#include <algorithm>
#include <unordered_map>

namespace fuse::ui::a11y {

namespace {
bool same(const A11ySnapshotNode& a, const A11ySnapshotNode& b, u64 pa, u64 pb) {
    return a.role == b.role && a.name == b.name && a.bounds.x == b.bounds.x && a.bounds.y == b.bounds.y &&
           a.bounds.w == b.bounds.w && a.bounds.h == b.bounds.h && a.focusable == b.focusable && a.focused == b.focused &&
           a.checked == b.checked && a.disabled == b.disabled && a.value == b.value && a.lo == b.lo && a.hi == b.hi && pa == pb;
}
u32 classId(const UiContext& ui, std::string_view name) { return ui.registry().find(name); }
} // namespace

// Menu / tree kinds and popup ownership come from the widget classes (fuse_ui_widgets Tier B): a "popup" scope keeps
// its owner as user data (generation << 32 | index), menu items and menu buttons keep submenu lines after a '\n'.
void Bridge::refine() {
    const u32 cMenu = classId(m_ui, "menu"), cMenuItem = classId(m_ui, "menuitem"), cMenuBar = classId(m_ui, "menubar"),
              cMenuButton = classId(m_ui, "menubutton"), cTree = classId(m_ui, "treeview"), cPopup = classId(m_ui, "popup"),
              cCombo = classId(m_ui, "combobox");
    std::vector<NodeHandle> owners; // owners of the open popups
    for (const auto& s : m_cur)
        if (m_ui.classOf(s.node) == cPopup && m_ui.computed(s.node).visible) {
            const u64 ud = m_ui.userData(s.node);
            owners.push_back({u32(ud & 0xFFFFFFFFu), u32(ud >> 32)});
        }
    m_curExtra.assign(m_cur.size(), {});
    for (u32 i = 0; i < m_cur.size(); ++i) {
        A11ySnapshotNode& s = m_cur[i];
        NodeExtra& x = m_curExtra[i];
        const u32 c = m_ui.classOf(s.node);
        const bool hasSub = m_ui.text(s.node).find('\n') != std::string_view::npos;
        if (c == cMenu) x.kind = Kind::Menu;
        else if (c == cMenuBar) x.kind = Kind::MenuBar;
        else if (c == cMenuItem || c == cMenuButton) x.kind = Kind::MenuItem;
        else if (c == cTree) x.kind = Kind::Tree;
        if (x.kind == Kind::MenuItem) s.name.resize(std::min(s.name.size(), s.name.find('\n'))); // label line only
        if (s.role == A11yRole::TextField) {
            x.text = std::string(m_ui.text(s.node));
            x.labelled = s.name != x.text;
        }
        if ((x.kind == Kind::MenuItem && hasSub) || c == cCombo)
            x.expanded = std::find(owners.begin(), owners.end(), s.node) != owners.end() ? 1 : 0;
        x.selected = (m_ui.states(s.node) & kStateSelected) != 0;
    }
}

u32 Bridge::sync() {
    std::lock_guard<std::mutex> lock(m_snapMutex);
    m_prev.swap(m_cur);
    m_prevExtra.swap(m_curExtra);
    m_ui.snapshotA11y(m_cur);
    refine();
    std::unordered_map<u64, u32> prevIdx;
    prevIdx.reserve(m_prev.size());
    for (u32 i = 0; i < m_prev.size(); ++i) prevIdx.emplace(idOf(m_prev[i].node), i);
    auto parentId = [](const std::vector<A11ySnapshotNode>& v, u32 i) { return v[i].parent == ~0u ? 0ull : idOf(v[v[i].parent].node); };
    u32 changed = 0, matched = 0;
    for (u32 i = 0; i < m_cur.size(); ++i) {
        auto it = prevIdx.find(idOf(m_cur[i].node));
        if (it == prevIdx.end()) { ++changed; continue; }
        ++matched;
        if (!same(m_cur[i], m_prev[it->second], parentId(m_cur, i), parentId(m_prev, it->second)) ||
            !(m_curExtra[i] == m_prevExtra[it->second]))
            ++changed;
    }
    changed += u32(m_prev.size()) - matched; // removed
    if (m_scale != m_syncedScale && !m_cur.empty()) {
        m_syncedScale = m_scale;
        ++changed; // the root transform
    }
    return changed;
}

u64 Bridge::focusId() const {
    for (const auto& n : m_cur)
        if (n.focused) return idOf(n.node);
    return m_cur.empty() ? 0 : idOf(m_cur[0].node);
}

std::vector<u32> Bridge::children(u32 i) const {
    std::vector<u32> c;
    for (u32 k = i + 1; k < m_cur.size(); ++k)
        if (m_cur[k].parent == i) c.push_back(k);
    return c;
}

bool Bridge::perform(const ActionRequest& r) {
    const NodeHandle n = nodeOf(r.target);
    if (!m_ui.alive(n)) return false;
    const NodeExtra* x = nullptr;
    for (u32 i = 0; i < m_cur.size(); ++i)
        if (m_cur[i].node == n) x = &m_curExtra[i];
    auto click = [&] {
        const Rect b = m_ui.rect(n);
        const Vec2 p{b.x + b.w * 0.5f, b.y + b.h * 0.5f};
        m_ui.pointerMove(p);
        m_ui.pointerDown(p);
        m_ui.pointerUp(p);
    };
    switch (r.action) {
    case Action::Focus: m_ui.setFocus(n, FocusReason::Keyboard); return true;
    case Action::Click: click(); return true;
    case Action::Increment:
    case Action::Decrement: {
        f32 lo = 0.f, hi = 0.f;
        for (const auto& s : m_cur)
            if (s.node == n) { lo = s.lo; hi = s.hi; }
        f32 step = (hi - lo) * 0.01f;
        if (step <= 0.f) step = 1.f;
        const f32 v = m_ui.value(n) + (r.action == Action::Increment ? step : -step);
        m_ui.setValue(n, hi > lo ? std::clamp(v, lo, hi) : v, true);
        return true;
    }
    case Action::SetValue: m_ui.setValue(n, r.value, true); return true;
    case Action::SetText: {
        if (m_ui.role(n) != A11yRole::TextField) return false;
        auto& eb = m_ui.editBuffer(n);
        eb.setText(r.text);
        eb.end(false);
        m_ui.setText(n, r.text);
        m_ui.emit(n, SignalKind::TextChanged, {.text = r.text});
        return true;
    }
    case Action::Expand:
        if (!x || x->expanded < 0) return false;
        if (x->expanded == 0) click();
        return true;
    case Action::Collapse:
        if (!x || x->expanded < 0) return false;
        if (x->expanded == 1) { // focus sits in the open popup: Escape closes it
            m_ui.keyDown(Key::Escape);
            m_ui.keyUp(Key::Escape);
        }
        return true;
    case Action::ScrollIntoView: {
        const Rect t = m_ui.rect(n);
        for (NodeHandle p = m_ui.parent(n); m_ui.alive(p); p = m_ui.parent(p)) {
            const Rect c = m_ui.contentRect(p);
            const Size ext = m_ui.scrollExtent(p);
            if (ext.w <= c.w && ext.h <= c.h) continue;
            Vec2 s = m_ui.scroll(p);
            if (t.y < c.y) s.y -= c.y - t.y;
            else if (t.y + t.h > c.y + c.h) s.y += std::min(t.y - c.y, t.y + t.h - (c.y + c.h));
            if (t.x < c.x) s.x -= c.x - t.x;
            else if (t.x + t.w > c.x + c.w) s.x += std::min(t.x - c.x, t.x + t.w - (c.x + c.w));
            m_ui.scrollTo(p, s);
        }
        return true;
    }
    }
    return false;
}

void Bridge::enqueue(const ActionRequest& r) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back(r);
}

u32 Bridge::drain() {
    std::vector<ActionRequest> q;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        q.swap(m_queue);
    }
    u32 done = 0;
    for (const auto& r : q) done += perform(r) ? 1u : 0u;
    return done;
}

} // namespace fuse::ui::a11y
