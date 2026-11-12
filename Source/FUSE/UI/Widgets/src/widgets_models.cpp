// FUSE UI model / view, the model side (plan §4.11): change notifier, selection model, StringTreeModel, tree filter
// proxy, asynchronous model. The views are in widgets_modelview.cpp.
#include "fuse/ui/widgets/modelview.hpp"

#include <algorithm>
#include <cctype>

namespace fuse::ui::widgets {
namespace {

template <class F>
void eachTab(std::string_view s, F&& f) {
    std::size_t at = 0;
    for (;;) {
        const std::size_t t = s.find('\t', at);
        f(s.substr(at, t == std::string_view::npos ? std::string_view::npos : t - at));
        if (t == std::string_view::npos) return;
        at = t + 1;
    }
}
void lower(std::string& s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
}
bool fullSpan(const SelectionRange& r) { return r.c0 == 0 && r.c1 == kAllColumns; }
bool rangeLess(const SelectionRange& a, const SelectionRange& b) { return a.r0 != b.r0 ? a.r0 < b.r0 : a.c0 < b.c0; }

} // namespace

// ---- ModelNotifier -------------------------------------------------------------------------------------------------
u32 ModelNotifier::subscribe(Fn fn, void* user, u64 tag) {
    if (!fn) return 0;
    m_subs.push_back({fn, user, tag, m_nextId});
    return m_nextId++;
}
void ModelNotifier::unsubscribe(u32 id) {
    for (Sub& s : m_subs)
        if (s.id == id) s.fn = nullptr; // compacted by the next notify (safe during one)
    if (m_depth == 0) std::erase_if(m_subs, [](const Sub& s) { return !s.fn; });
}
u32 ModelNotifier::subscribers() const {
    return u32(std::count_if(m_subs.begin(), m_subs.end(), [](const Sub& s) { return s.fn != nullptr; }));
}
void ModelNotifier::notify(const ModelChange& change) {
    ++m_depth;
    for (std::size_t i = 0; i < m_subs.size(); ++i) { // by index: a callback may subscribe
        const Sub s = m_subs[i];
        if (s.fn && !s.fn(s.user, s.tag, change) && i < m_subs.size() && m_subs[i].id == s.id) m_subs[i].fn = nullptr;
    }
    if (--m_depth == 0) std::erase_if(m_subs, [](const Sub& s) { return !s.fn; });
}

// ---- SelectionModel ------------------------------------------------------------------------------------------------
SelectionModel::SelectionModel() = default;
SelectionModel::~SelectionModel() { track(nullptr); }

bool SelectionModel::isSelected(u32 row, u32 col) const {
    if (m_ranges.empty()) return false;
    if (m_partial == 0) { // row ranges only: sorted and disjoint
        auto it = std::upper_bound(m_ranges.begin(), m_ranges.end(), row, [](u32 r, const SelectionRange& g) { return r < g.r0; });
        return it != m_ranges.begin() && (it - 1)->r1 >= row;
    }
    for (const SelectionRange& g : m_ranges) {
        if (g.r0 > row) break;
        if (row <= g.r1 && col >= g.c0 && col <= g.c1) return true;
    }
    return false;
}
u64 SelectionModel::selectedRows() const {
    u64 n = 0;
    if (m_partial == 0) {
        for (const SelectionRange& g : m_ranges) n += u64(g.r1) - g.r0 + 1;
        return n;
    }
    // cell ranges may share rows: union of the row intervals (sorted by r0 already)
    i64 end = -1;
    for (const SelectionRange& g : m_ranges) {
        const i64 a = std::max<i64>(g.r0, end + 1);
        if (i64(g.r1) >= a) { n += u64(i64(g.r1) - a + 1); end = g.r1; }
    }
    return n;
}
u64 SelectionModel::selectedCells(u32 columns) const {
    u64 n = 0;
    for (const SelectionRange& g : m_ranges) {
        const u64 c1 = std::min<u64>(g.c1, columns ? columns - 1 : 0);
        if (columns && c1 >= g.c0) n += (u64(g.r1) - g.r0 + 1) * (c1 - g.c0 + 1);
    }
    return n;
}
void SelectionModel::changed() {
    if (m_batch) { m_dirty = true; return; }
    m_dirty = false;
    ++m_version;
    ++m_depth;
    for (std::size_t i = 0; i < m_subs.size(); ++i) {
        const Sub s = m_subs[i];
        if (s.fn && !s.fn(s.user, s.tag, *this) && i < m_subs.size() && m_subs[i].id == s.id) m_subs[i].fn = nullptr;
    }
    if (--m_depth == 0) std::erase_if(m_subs, [](const Sub& s) { return !s.fn; });
}
u32 SelectionModel::subscribe(Fn fn, void* user, u64 tag) {
    if (!fn) return 0;
    m_subs.push_back({fn, user, tag, m_nextId});
    return m_nextId++;
}
void SelectionModel::unsubscribe(u32 id) {
    for (Sub& s : m_subs)
        if (s.id == id) s.fn = nullptr;
    if (m_depth == 0) std::erase_if(m_subs, [](const Sub& s) { return !s.fn; });
}
void SelectionModel::removeNoNotify(const SelectionRange& r) {
    bool hit = false;
    for (const SelectionRange& g : m_ranges)
        if (g.r0 <= r.r1 && g.r1 >= r.r0 && g.c0 <= r.c1 && g.c1 >= r.c0) { hit = true; break; }
    if (!hit) return;
    m_scratch.clear();
    for (const SelectionRange& g : m_ranges) {
        if (!(g.r0 <= r.r1 && g.r1 >= r.r0 && g.c0 <= r.c1 && g.c1 >= r.c0)) { m_scratch.push_back(g); continue; }
        const u32 a = std::max(g.r0, r.r0), b = std::min(g.r1, r.r1); // overlapping rows
        if (g.r0 < a) m_scratch.push_back({g.r0, a - 1, g.c0, g.c1});
        if (g.c0 < r.c0) m_scratch.push_back({a, b, g.c0, r.c0 - 1});
        if (g.c1 > r.c1) m_scratch.push_back({a, b, r.c1 + 1, g.c1});
        if (g.r1 > b) m_scratch.push_back({b + 1, g.r1, g.c0, g.c1});
    }
    m_ranges.swap(m_scratch);
    if (!std::is_sorted(m_ranges.begin(), m_ranges.end(), rangeLess)) std::sort(m_ranges.begin(), m_ranges.end(), rangeLess);
    m_partial = u32(std::count_if(m_ranges.begin(), m_ranges.end(), [](const SelectionRange& g) { return !fullSpan(g); }));
}
void SelectionModel::addNoNotify(SelectionRange r) {
    if (r.r1 < r.r0) std::swap(r.r0, r.r1);
    if (r.c1 < r.c0) std::swap(r.c0, r.c1);
    removeNoNotify(r);
    auto it = std::lower_bound(m_ranges.begin(), m_ranges.end(), r, rangeLess);
    if (fullSpan(r)) { // merge with row-adjacent row ranges
        if (it != m_ranges.begin() && fullSpan(*(it - 1)) && (it - 1)->r1 + 1 == r.r0) {
            --it;
            r.r0 = it->r0;
            it = m_ranges.erase(it);
        }
        if (it != m_ranges.end() && fullSpan(*it) && r.r1 != 0xFFFFFFFFu && it->r0 == r.r1 + 1) {
            r.r1 = it->r1;
            it = m_ranges.erase(it);
        }
    } else {
        ++m_partial;
    }
    m_ranges.insert(it, r);
}
void SelectionModel::clear() {
    if (m_ranges.empty()) return;
    m_ranges.clear();
    m_partial = 0;
    changed();
}
void SelectionModel::select(const SelectionRange& r) { addNoNotify(r); changed(); }
void SelectionModel::deselect(const SelectionRange& r) { removeNoNotify(r); changed(); }
void SelectionModel::setSelection(const SelectionRange& r) {
    if (m_ranges.size() == 1 && m_ranges[0] == r) return;
    m_ranges.clear();
    m_partial = 0;
    addNoNotify(r);
    changed();
}
void SelectionModel::toggle(u32 row, u32 col) {
    const SelectionRange r = col == kAllColumns ? SelectionRange{row, row} : SelectionRange{row, row, col, col};
    if (isSelected(row, col == kAllColumns ? 0 : col)) removeNoNotify(r);
    else addNoNotify(r);
    changed();
}
void SelectionModel::setCurrent(i64 row, u32 col) {
    if (row == m_curRow && col == m_curCol) return;
    m_curRow = row;
    m_curCol = col;
    changed();
}
void SelectionModel::pointerSelect(u32 row, u32 col, u8 mods, SelectionMode mode, SelectionUnit unit) {
    const bool cells = unit == SelectionUnit::Cells, shift = mods & kModShift, ctrl = mods & kModCtrl;
    const SelectionRange one = cells ? SelectionRange{row, row, col, col} : SelectionRange{row, row};
    ++m_batch;
    bool keepAnchor = false;
    switch (mode) {
    case SelectionMode::None: break;
    case SelectionMode::Single: setSelection(one); break;
    case SelectionMode::Multi: toggle(row, cells ? col : kAllColumns); break;
    case SelectionMode::Extended:
        if (shift && m_anchorRow >= 0) {
            SelectionRange r{u32(std::min<i64>(m_anchorRow, row)), u32(std::max<i64>(m_anchorRow, row))};
            if (cells) { r.c0 = std::min(m_anchorCol, col); r.c1 = std::max(m_anchorCol, col); }
            if (ctrl) select(r);
            else setSelection(r);
            keepAnchor = true;
        } else if (ctrl) {
            toggle(row, cells ? col : kAllColumns);
        } else {
            setSelection(one);
        }
        break;
    }
    if (!keepAnchor) { m_anchorRow = row; m_anchorCol = col; }
    setCurrent(row, col);
    --m_batch;
    if (m_dirty) changed();
}
void SelectionModel::keySelect(u32 row, u32 col, u8 mods, SelectionMode mode, SelectionUnit unit) {
    const bool cells = unit == SelectionUnit::Cells, shift = mods & kModShift, ctrl = mods & kModCtrl;
    const SelectionRange one = cells ? SelectionRange{row, row, col, col} : SelectionRange{row, row};
    ++m_batch;
    bool keepAnchor = false;
    switch (mode) {
    case SelectionMode::None:
    case SelectionMode::Multi: keepAnchor = true; break; // arrows only move the current item
    case SelectionMode::Single: setSelection(one); break;
    case SelectionMode::Extended:
        if (shift) {
            if (m_anchorRow < 0) { m_anchorRow = m_curRow >= 0 ? m_curRow : i64(row); m_anchorCol = m_curRow >= 0 ? m_curCol : col; }
            SelectionRange r{u32(std::min<i64>(m_anchorRow, row)), u32(std::max<i64>(m_anchorRow, row))};
            if (cells) { r.c0 = std::min(m_anchorCol, col); r.c1 = std::max(m_anchorCol, col); }
            setSelection(r);
            keepAnchor = true;
        } else if (ctrl) {
            keepAnchor = true;
        } else {
            setSelection(one);
        }
        break;
    }
    if (!keepAnchor) { m_anchorRow = row; m_anchorCol = col; }
    setCurrent(row, col);
    --m_batch;
    if (m_dirty) changed();
}
void SelectionModel::rowsInserted(u32 first, u32 count) {
    if (!count) return;
    bool any = false;
    m_scratch.clear();
    for (const SelectionRange& g : m_ranges) {
        if (g.r1 < first) { m_scratch.push_back(g); continue; }
        any = true;
        if (g.r0 >= first) { m_scratch.push_back({g.r0 + count, g.r1 + count, g.c0, g.c1}); continue; }
        m_scratch.push_back({g.r0, first - 1, g.c0, g.c1}); // the new rows split the range and are not selected
        m_scratch.push_back({first + count, g.r1 + count, g.c0, g.c1});
    }
    if (any) {
        m_ranges.swap(m_scratch);
        if (!std::is_sorted(m_ranges.begin(), m_ranges.end(), rangeLess)) std::sort(m_ranges.begin(), m_ranges.end(), rangeLess);
        m_partial = u32(std::count_if(m_ranges.begin(), m_ranges.end(), [](const SelectionRange& g) { return !fullSpan(g); }));
    }
    if (m_curRow >= i64(first)) { m_curRow += count; any = true; }
    if (m_anchorRow >= i64(first)) m_anchorRow += count;
    if (any) changed();
}
void SelectionModel::rowsRemoved(u32 first, u32 count) {
    if (!count) return;
    const u64 last = u64(first) + count - 1;
    bool any = false;
    m_scratch.clear();
    auto push = [&](const SelectionRange& g) { // row ranges that became adjacent merge
        if (!m_scratch.empty() && fullSpan(g) && fullSpan(m_scratch.back()) && m_scratch.back().r1 + 1 >= g.r0) m_scratch.back().r1 = g.r1;
        else m_scratch.push_back(g);
    };
    for (const SelectionRange& g : m_ranges) {
        if (g.r1 < first) { m_scratch.push_back(g); continue; }
        any = true;
        if (g.r0 > last) { push({g.r0 - count, g.r1 - count, g.c0, g.c1}); continue; }
        const i64 a = g.r0 < first ? g.r0 : first; // what is left of a range the removal cut into
        const i64 b = g.r1 > last ? i64(g.r1) - count : i64(first) - 1;
        if (b >= a) push({u32(a), u32(b), g.c0, g.c1});
    }
    if (any) {
        m_ranges.swap(m_scratch);
        if (!std::is_sorted(m_ranges.begin(), m_ranges.end(), rangeLess)) std::sort(m_ranges.begin(), m_ranges.end(), rangeLess);
        m_partial = u32(std::count_if(m_ranges.begin(), m_ranges.end(), [](const SelectionRange& g) { return !fullSpan(g); }));
    }
    auto fix = [&](i64& r) {
        if (r < i64(first)) return false;
        r = r > i64(last) ? r - count : i64(first); // a removed current row hands over to the row that took its place
        return true;
    };
    any |= fix(m_curRow);
    fix(m_anchorRow);
    if (any) changed();
}
void SelectionModel::rowsMoved(u32 first, u32 count, u32 dest) {
    if (!count || first == dest) return;
    ++m_batch;
    // remember what was selected inside the block, relative to its start
    std::vector<SelectionRange> block;
    for (const SelectionRange& g : m_ranges)
        if (g.r0 < first + count && g.r1 >= first)
            block.push_back({std::max(g.r0, first) - first, std::min(g.r1, first + count - 1) - first, g.c0, g.c1});
    const i64 cur = m_curRow, anchor = m_anchorRow;
    rowsRemoved(first, count);
    rowsInserted(dest, count);
    for (SelectionRange g : block) { g.r0 += dest; g.r1 += dest; addNoNotify(g); }
    if (cur >= i64(first) && cur < i64(first) + count) m_curRow = cur - first + dest;
    if (anchor >= i64(first) && anchor < i64(first) + count) m_anchorRow = anchor - first + dest;
    m_dirty = true;
    --m_batch;
    changed();
}
void SelectionModel::track(ModelNotifier* notifier) {
    if (m_tracked == notifier) return;
    if (m_tracked) m_tracked->unsubscribe(m_trackId);
    m_tracked = notifier;
    m_trackId = 0;
    if (!notifier) return;
    m_trackId = notifier->subscribe([](void* user, u64, const ModelChange& c) {
        auto& self = *static_cast<SelectionModel*>(user);
        if (c.parent != kRootItem) return true;
        switch (c.kind) {
        case ModelChange::Kind::RowsInserted: self.rowsInserted(c.first, c.count); break;
        case ModelChange::Kind::RowsRemoved: self.rowsRemoved(c.first, c.count); break;
        case ModelChange::Kind::RowsMoved: self.rowsMoved(c.first, c.count, c.dest); break;
        case ModelChange::Kind::Reset: self.clear(); break;
        case ModelChange::Kind::DataChanged: break;
        }
        return true;
    }, this);
}

// ---- StringTreeModel -----------------------------------------------------------------------------------------------
StringTreeModel::StringTreeModel(std::string_view headers) {
    eachTab(headers, [&](std::string_view h) { m_headers.emplace_back(h); });
    m_cols = u32(m_headers.size());
    m_nodes.emplace_back(); // root
    m_model.user = this;
    m_model.columns = m_cols;
    m_model.notifier = &m_notifier;
    m_model.rows = [](const void* u) { return static_cast<const StringTreeModel*>(u)->childCount(kRootItem); };
    m_model.data = [](const void* u, u32 r, u32 c, std::string&) {
        const auto* m = static_cast<const StringTreeModel*>(u);
        return m->cell(m->childAt(kRootItem, r), c);
    };
    m_model.header = [](const void* u, u32 c) { return std::string_view(static_cast<const StringTreeModel*>(u)->m_headers[c]); };
    m_model.childCount = [](const void* u, ItemId p) { return static_cast<const StringTreeModel*>(u)->childCount(p); };
    m_model.childAt = [](const void* u, ItemId p, u32 r) { return static_cast<const StringTreeModel*>(u)->childAt(p, r); };
    m_model.itemData = [](const void* u, ItemId i, u32 c, std::string&) { return static_cast<const StringTreeModel*>(u)->cell(i, c); };
    m_model.flags = [](const void* u, ItemId, u32) -> u32 {
        return kItemSelectable | kItemEnabled | (static_cast<const StringTreeModel*>(u)->m_editable ? u32(kItemEditable) : 0u);
    };
    m_model.setData = [](const void* u, ItemId i, u32 c, ItemRole role, const ItemValue& v) {
        auto* m = const_cast<StringTreeModel*>(static_cast<const StringTreeModel*>(u));
        if (role != ItemRole::Edit || !m->m_editable || !m->get(i) || i == kRootItem || c >= m->m_cols) return false;
        m->setCell(i, c, v.text);
        return true;
    };
}
ItemId StringTreeModel::add(ItemId parent, std::string_view cells, u32 row) {
    if (!get(parent)) return kRootItem;
    const ItemId id = m_nodes.size();
    m_nodes.emplace_back();
    Node& n = m_nodes.back();
    n.parent = parent;
    n.live = true;
    eachTab(cells, [&](std::string_view c) { if (n.cells.size() < m_cols) n.cells.emplace_back(c); });
    n.cells.resize(m_cols);
    auto& kids = m_nodes[parent].kids;
    row = std::min<u32>(row, u32(kids.size()));
    kids.insert(kids.begin() + row, id);
    ++m_live;
    m_notifier.rowsInserted(row, 1, parent);
    return id;
}
void StringTreeModel::remove(ItemId item) {
    if (item == kRootItem || !get(item)) return;
    const ItemId parent = m_nodes[item].parent;
    auto& kids = m_nodes[parent].kids;
    const auto it = std::find(kids.begin(), kids.end(), item);
    const u32 row = u32(it - kids.begin());
    if (it != kids.end()) kids.erase(it);
    std::vector<ItemId> stack{item};
    while (!stack.empty()) {
        const ItemId k = stack.back();
        stack.pop_back();
        Node& n = m_nodes[k];
        stack.insert(stack.end(), n.kids.begin(), n.kids.end());
        n = Node{};
        --m_live;
    }
    m_notifier.rowsRemoved(row, 1, parent);
}
void StringTreeModel::setCell(ItemId item, u32 col, std::string_view text) {
    if (item == kRootItem || !get(item) || col >= m_cols) return;
    m_nodes[item].cells[col].assign(text);
    const ItemId parent = m_nodes[item].parent;
    const auto& kids = m_nodes[parent].kids;
    m_notifier.dataChanged(u32(std::find(kids.begin(), kids.end(), item) - kids.begin()), 1, col, col, parent);
}
std::string_view StringTreeModel::cell(ItemId item, u32 col) const {
    const Node* n = item == kRootItem ? nullptr : get(item);
    return n && col < n->cells.size() ? std::string_view(n->cells[col]) : std::string_view{};
}
u32 StringTreeModel::childCount(ItemId parent) const { const Node* n = get(parent); return n ? u32(n->kids.size()) : 0u; }
ItemId StringTreeModel::childAt(ItemId parent, u32 row) const {
    const Node* n = get(parent);
    return n && row < n->kids.size() ? n->kids[row] : kRootItem;
}
ItemId StringTreeModel::parentOf(ItemId item) const { const Node* n = get(item); return n ? n->parent : kRootItem; }

// ---- TreeFilterProxy -----------------------------------------------------------------------------------------------
TreeFilterProxy::TreeFilterProxy(const ItemModel* source) : m_src(source) {
    using P = TreeFilterProxy;
    auto self = [](const void* u) { return static_cast<const P*>(u); };
    m_model.user = this;
    m_model.columns = source ? source->columns : 1;
    m_model.notifier = &m_notifier;
    if (!source) return;
    if (source->header) m_model.header = [](const void* u, u32 c) { const P* p = static_cast<const P*>(u); return p->m_src->header(p->m_src->user, c); };
    m_model.childCount = [](const void* u, ItemId parent) -> u32 {
        const P* p = static_cast<const P*>(u);
        if (p->m_needle.empty()) return p->m_src->childCount ? p->m_src->childCount(p->m_src->user, parent) : 0u;
        const auto it = p->m_children.find(parent);
        return it == p->m_children.end() ? 0u : u32(it->second.size());
    };
    m_model.childAt = [](const void* u, ItemId parent, u32 row) -> ItemId {
        const P* p = static_cast<const P*>(u);
        if (p->m_needle.empty()) return p->m_src->childAt(p->m_src->user, parent, row);
        const auto it = p->m_children.find(parent);
        return it == p->m_children.end() || row >= it->second.size() ? kRootItem : it->second[row];
    };
    m_model.hasChildren = [](const void* u, ItemId item) -> bool {
        const P* p = static_cast<const P*>(u);
        if (!p->m_needle.empty()) return p->m_children.contains(item);
        if (p->m_src->hasChildren) return p->m_src->hasChildren(p->m_src->user, item);
        return p->m_src->childCount && p->m_src->childCount(p->m_src->user, item) > 0;
    };
    m_model.itemData = [](const void* u, ItemId i, u32 c, std::string& s) { const P* p = static_cast<const P*>(u); return p->m_src->itemData(p->m_src->user, i, c, s); };
    if (source->role)
        m_model.role = [](const void* u, ItemId i, u32 c, ItemRole r, ItemValue& o, std::string& s) {
            const P* p = static_cast<const P*>(u);
            return p->m_src->role(p->m_src->user, i, c, r, o, s);
        };
    if (source->flags) m_model.flags = [](const void* u, ItemId i, u32 c) { const P* p = static_cast<const P*>(u); return p->m_src->flags(p->m_src->user, i, c); };
    if (source->setData)
        m_model.setData = [](const void* u, ItemId i, u32 c, ItemRole r, const ItemValue& v) {
            const P* p = static_cast<const P*>(u);
            return p->m_src->setData(p->m_src->user, i, c, r, v);
        };
    if (source->canFetchMore && source->fetchMore) { // lazy children only while unfiltered
        m_model.canFetchMore = [](const void* u, ItemId i) { const P* p = static_cast<const P*>(u); return p->m_needle.empty() && p->m_src->canFetchMore(p->m_src->user, i); };
        m_model.fetchMore = [](const void* u, ItemId i) { const P* p = static_cast<const P*>(u); if (p->m_needle.empty()) p->m_src->fetchMore(p->m_src->user, i); };
    }
    (void)self;
    if (source->notifier)
        m_srcSub = source->notifier->subscribe([](void* u, u64, const ModelChange& c) {
            auto* p = static_cast<P*>(u);
            if (p->m_needle.empty()) { p->m_notifier.notify(c); return true; } // same ids, same rows
            p->invalidate();
            return true;
        }, this);
}
TreeFilterProxy::~TreeFilterProxy() {
    if (m_src && m_src->notifier && m_srcSub) m_src->notifier->unsubscribe(m_srcSub);
}
void TreeFilterProxy::setFilter(i32 col, std::string_view needle) {
    m_col = col;
    m_needle.assign(needle);
    lower(m_needle);
    invalidate();
}
bool TreeFilterProxy::matches(ItemId item) const {
    const auto it = m_match.find(item);
    return it != m_match.end() && it->second;
}
bool TreeFilterProxy::build(ItemId parent, std::string& scratch, std::string& low, u32 depth) {
    if (depth > 512) return false;
    const u32 n = m_src->childCount(m_src->user, parent);
    std::vector<ItemId> kept;
    for (u32 r = 0; r < n; ++r) {
        const ItemId c = m_src->childAt(m_src->user, parent, r);
        bool self = false;
        const u32 c0 = m_col < 0 ? 0u : u32(m_col), c1 = m_col < 0 ? m_src->columns : u32(m_col) + 1;
        for (u32 k = c0; k < c1 && k < m_src->columns && !self; ++k) {
            low.assign(m_src->itemData(m_src->user, c, k, scratch));
            lower(low);
            self = low.find(m_needle) != std::string::npos;
        }
        const bool below = build(c, scratch, low, depth + 1);
        if (!self && !below) continue;
        kept.push_back(c);
        m_match[c] = self;
        ++m_kept;
    }
    if (kept.empty()) return false;
    m_children[parent] = std::move(kept);
    return true;
}
void TreeFilterProxy::invalidate() {
    m_children.clear();
    m_match.clear();
    m_kept = 0;
    if (m_src && m_src->childCount && m_src->childAt && m_src->itemData && !m_needle.empty()) {
        std::string scratch, low;
        build(kRootItem, scratch, low, 0);
    }
    m_notifier.reset();
}

// ---- AsyncModel ----------------------------------------------------------------------------------------------------
namespace {
// Live async models by id: a posted commit finds its model here, so one destroyed before the drain is skipped.
struct AsyncRegistry {
    std::mutex mutex;
    std::unordered_map<u64, AsyncModel*> live;
    u64 next = 1;
};
AsyncRegistry& asyncRegistry() { static AsyncRegistry r; return r; }
} // namespace

AsyncModel::AsyncModel(UiContext& ui, u32 columns, u32 pageRows, u32 maxPages)
    : m_ui(ui), m_cols(std::max(1u, columns)), m_pageRows(std::max(1u, pageRows)), m_maxPages(std::max(2u, maxPages)) {
    {
        AsyncRegistry& r = asyncRegistry();
        const std::lock_guard lock(r.mutex);
        m_id = r.next++;
        r.live[m_id] = this;
    }
    m_pages.reserve(m_maxPages);
    m_headers.resize(m_cols);
    m_model.user = this;
    m_model.columns = m_cols;
    m_model.notifier = &m_notifier;
    m_model.rows = [](const void* u) { return static_cast<const AsyncModel*>(u)->m_rows; };
    m_model.data = [](const void* u, u32 r, u32 c, std::string&) { return const_cast<AsyncModel*>(static_cast<const AsyncModel*>(u))->cell(r, c); };
    m_model.header = [](const void* u, u32 c) {
        const auto* m = static_cast<const AsyncModel*>(u);
        return c < m->m_headers.size() ? std::string_view(m->m_headers[c]) : std::string_view{};
    };
}
AsyncModel::~AsyncModel() {
    AsyncRegistry& r = asyncRegistry();
    const std::lock_guard lock(r.mutex);
    r.live.erase(m_id);
}
void AsyncModel::setSource(FetchFn fetch, void* user) { m_fetch = fetch; m_fetchUser = user; }
void AsyncModel::setHeaders(std::string_view headers) {
    u32 i = 0;
    eachTab(headers, [&](std::string_view h) { if (i < m_cols) m_headers[i++].assign(h); });
}
void AsyncModel::setRowCount(u32 rows) {
    m_rows = rows;
    ++m_generation;
    m_liveGeneration.store(m_generation, std::memory_order_release);
    m_pages.clear();
    m_pending = 0;
    {
        const std::lock_guard lock(m_mutex);
        m_inbox.clear();
    }
    m_notifier.reset();
}
AsyncModel::Page* AsyncModel::find(u32 page) {
    for (Page& p : m_pages)
        if (p.index == page) return &p;
    return nullptr;
}
const AsyncModel::Page* AsyncModel::find(u32 page) const { return const_cast<AsyncModel*>(this)->find(page); }
bool AsyncModel::loaded(u32 row) const {
    const Page* p = find(row / m_pageRows);
    return p && p->loaded;
}
std::string_view AsyncModel::cell(u32 row, u32 col) {
    if (row >= m_rows || col >= m_cols) return {};
    const u32 page = row / m_pageRows;
    Page* p = find(page);
    if (!p) {
        if (m_pages.size() >= m_maxPages) { // drop the least recently used page (its late answer is ignored)
            auto lru = std::min_element(m_pages.begin(), m_pages.end(), [](const Page& a, const Page& b) { return a.stamp < b.stamp; });
            if (!lru->loaded && m_pending) --m_pending;
            *lru = std::move(m_pages.back());
            m_pages.pop_back();
        }
        m_pages.push_back({page, ++m_stamp, false, {}});
        ++m_pending;
        ++m_fetches;
        const u32 first = page * m_pageRows;
        if (m_fetch) m_fetch(m_fetchUser, *this, first, std::min(m_pageRows, m_rows - first), (u64(m_generation) << 32) | page);
        return m_placeholder;
    }
    p->stamp = ++m_stamp;
    if (!p->loaded) return m_placeholder;
    const std::size_t at = std::size_t(row - page * m_pageRows) * m_cols + col;
    return at < p->cells.size() ? std::string_view(p->cells[at]) : std::string_view{};
}
bool AsyncModel::deliver(u64 ticket, std::vector<std::string>&& cells) {
    if (u32(ticket >> 32) != m_liveGeneration.load(std::memory_order_acquire)) return false;
    {
        const std::lock_guard lock(m_mutex);
        m_inbox.push_back({ticket, std::move(cells)});
    }
    m_ui.post(&AsyncModel::commitCmd, NodeHandle{}, m_id, 0); // queue full: the page waits in the inbox for poll()
    return true;
}
void AsyncModel::commitCmd(UiContext&, NodeHandle, u64 id, u64) {
    AsyncModel* m = nullptr;
    {
        AsyncRegistry& r = asyncRegistry();
        const std::lock_guard lock(r.mutex);
        const auto it = r.live.find(id);
        if (it != r.live.end()) m = it->second;
    }
    if (m) m->poll(); // owner thread: the model cannot be destroyed under us
}
u32 AsyncModel::poll() {
    std::vector<Inbox> in;
    {
        const std::lock_guard lock(m_mutex);
        if (m_inbox.empty()) return 0;
        in.swap(m_inbox);
    }
    u32 committed = 0;
    for (Inbox& b : in) {
        if (u32(b.ticket >> 32) != m_generation) continue;
        const u32 page = u32(b.ticket & 0xFFFFFFFFu);
        Page* p = find(page);
        if (!p || p->loaded) continue; // evicted meanwhile, or answered twice
        const u32 first = page * m_pageRows, count = std::min(m_pageRows, m_rows - first);
        b.cells.resize(std::size_t(count) * m_cols);
        p->cells = std::move(b.cells);
        p->loaded = true;
        if (m_pending) --m_pending;
        ++committed;
        m_notifier.dataChanged(first, count);
    }
    return committed;
}

} // namespace fuse::ui::widgets
