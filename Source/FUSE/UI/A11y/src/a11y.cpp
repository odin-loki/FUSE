#include "fuse/ui/a11y/a11y.hpp"

#include <algorithm>
#include <unordered_map>

namespace fuse::ui::a11y {

namespace {
bool same(const A11ySnapshotNode& a, const A11ySnapshotNode& b, u64 pa, u64 pb) {
    return a.role == b.role && a.name == b.name && a.bounds.x == b.bounds.x && a.bounds.y == b.bounds.y &&
           a.bounds.w == b.bounds.w && a.bounds.h == b.bounds.h && a.focusable == b.focusable && a.focused == b.focused &&
           a.checked == b.checked && a.disabled == b.disabled && a.value == b.value && a.lo == b.lo && a.hi == b.hi && pa == pb &&
           a.valueText == b.valueText && a.expandable == b.expandable && a.expanded == b.expanded &&
           a.selectable == b.selectable && a.selected == b.selected && a.modal == b.modal && a.live == b.live &&
           a.actions == b.actions && a.level == b.level && a.posInSet == b.posInSet && a.setSize == b.setSize;
}
/// What a live node says: its name, else its value text.
const std::string& spoken(const A11ySnapshotNode& s) { return s.name.empty() ? s.valueText : s.name; }
} // namespace

u32 Bridge::sync() {
    std::lock_guard<std::mutex> lock(m_snapMutex);
    m_prev.swap(m_cur);
    m_ui.snapshotA11y(m_cur);
    // items whose id cannot be represented (see idOf) are left out, with anything below them
    for (u32 i = 0; i < m_cur.size();) {
        if (idOf(m_cur[i]) != 0) { ++i; continue; }
        u32 end = i + 1;
        while (end < m_cur.size() && m_cur[end].parent != ~0u && m_cur[end].parent >= i) ++end;
        for (u32 k = end; k < m_cur.size(); ++k)
            if (m_cur[k].parent != ~0u && m_cur[k].parent >= end) m_cur[k].parent -= end - i;
        m_cur.erase(m_cur.begin() + i, m_cur.begin() + end);
    }
    std::unordered_map<u64, u32> prevIdx;
    prevIdx.reserve(m_prev.size());
    for (u32 i = 0; i < m_prev.size(); ++i) prevIdx.emplace(idOf(m_prev[i]), i);
    auto parentId = [](const std::vector<A11ySnapshotNode>& v, u32 i) { return v[i].parent == ~0u ? 0ull : idOf(v[v[i].parent]); };
    u32 changed = 0, matched = 0;
    for (u32 i = 0; i < m_cur.size(); ++i) {
        const A11ySnapshotNode& c = m_cur[i];
        auto it = prevIdx.find(idOf(c));
        // live regions: announced when they appear with text (not on the very first snapshot of an existing UI) or
        // when their text changes
        if (c.live != A11yLive::Off && !spoken(c).empty() &&
            (it == prevIdx.end() ? m_synced : spoken(m_prev[it->second]) != spoken(c)))
            m_announcements.push_back({idOf(c), spoken(c), c.live == A11yLive::Assertive});
        if (it == prevIdx.end()) { ++changed; continue; }
        ++matched;
        if (!same(c, m_prev[it->second], parentId(m_cur, i), parentId(m_prev, it->second))) ++changed;
    }
    changed += u32(m_prev.size()) - matched; // removed
    m_synced = true;
    return changed;
}

std::vector<Announcement> Bridge::takeAnnouncements() {
    std::lock_guard<std::mutex> lock(m_snapMutex);
    std::vector<Announcement> out;
    out.swap(m_announcements);
    return out;
}

u64 Bridge::focusId() const {
    for (const auto& n : m_cur)
        if (n.focused && !n.child) return idOf(n.node);
    return m_cur.empty() ? 0 : idOf(m_cur[0].node);
}

std::vector<u32> Bridge::children(u32 i) const {
    std::vector<u32> c;
    for (u32 k = i + 1; k < m_cur.size(); ++k)
        if (m_cur[k].parent == i) c.push_back(k);
    return c;
}

u32 Bridge::indexOf(u64 id) const {
    for (u32 i = 0; i < m_cur.size(); ++i)
        if (idOf(m_cur[i]) == id) return i;
    return ~0u;
}

bool Bridge::resolve(u64 id, NodeHandle& node, u32& child) const {
    if (!id) return false;
    if (!isVirtual(id)) {
        node = nodeOf(id);
        child = 0;
        return m_ui.alive(node);
    }
    node = m_ui.handleOf(u32((id >> 31) & 0xFFFFFu));
    child = u32(id & 0x7FFFFFFFu);
    return m_ui.alive(node) && (node.generation & 0xFFFu) == u32((id >> 51) & 0xFFFu) && child != 0;
}

bool Bridge::perform(const ActionRequest& r) {
    NodeHandle n;
    u32 child = 0;
    if (!resolve(r.target, n, child)) return false;
    return m_ui.performA11yAction(n, r.action, r.value, child, r.text);
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
