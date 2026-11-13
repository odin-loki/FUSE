// FUSE UI model / view, the view side (plan §4.11): one virtualised item view class behind tableView, modelListView
// and treeModelView. Flat rows are the model's rows; tree rows are located through the records of the expanded nodes
// only (TreeRows), so nothing here is proportional to the number of rows. The model side is widgets_models.cpp.
#include "fuse/ui/widgets/modelview.hpp"
#include "fuse/text/from_chars.hpp"
#include "widgets_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::ui::widgets {
namespace {

constexpr u32 kMaxCols = 32;
constexpr f32 kBar = 8.f, kIndent = 16.f;
constexpr i32 kNoNode = -1;

NodeHandle make(UiContext& ui, std::string_view cls, NodeHandle parent, std::string_view id = {}) {
    auto h = ui.create(cls, parent, id);
    return h ? *h : NodeHandle{};
}
void setPx(UiContext& ui, NodeHandle n, Prop p, f32 v) { ui.set(n, p, PropValue::ofLength(Length::px(std::round(v)))); }
bool parseNum(std::string_view s, double& out) {
    const auto [p, ec] = fuse::text::fromChars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size();
}

// ---- tree rows: the expanded nodes -------------------------------------------------------------------------------
// A record per EXPANDED node (the root is always expanded). `visible` = rows shown below the node: its children
// plus the visible rows of its expanded children. A visible row is found by walking down from the root, skipping
// whole expanded subtrees by their `visible` count.
struct TreeNode {
    ItemId id = kRootItem;
    i32 parent = kNoNode;
    u32 row = 0;     ///< child row in the parent
    u32 count = 0;   ///< children (fetched so far)
    u32 visible = 0;
    std::vector<i32> kids; ///< expanded children, sorted by row
    bool used = false;
};
struct TreeRows {
    std::vector<TreeNode> nodes; ///< [0] = root
    std::vector<i32> freeList;
    std::unordered_map<ItemId, i32> byId;
};
struct RowRef {
    ItemId id = kRootItem;
    i32 parent = 0;     ///< record of the parent
    u32 childRow = 0;
    u32 depth = 0;
    i32 node = kNoNode; ///< the row's own record when it is expanded
    bool kids = false;
};

struct ViewState {
    UiContext* ui = nullptr;
    u64 uid = 0; ///< ui->uid(): the pool outlives contexts, `ui` is only used while UiContext::exists(uid)
    NodeHandle node;
    u32 serial = 0;
    const ItemModel* model = nullptr;
    u32 first = 0;
    f32 scrollX = 0.f;
    f32 colW[kMaxCols]{};
    u8 order[kMaxCols]{}; ///< visual position -> model column
    u32 hiddenMask = 0;
    i32 sortCol = -1;
    bool asc = true;
    bool header = true, list = false, tree = false;
    i32 resizing = -1;
    u8 dragScroll = 0; ///< 1 = vertical bar, 2 = horizontal bar
    f32 grab = 0.f;
    std::string scratch, scratch2;
    const CellDelegate* delegate[kMaxCols]{};
    // selection
    SelectionModel* sel = nullptr; ///< null: plain single selection, value() is the selected row
    std::unique_ptr<SelectionModel> ownSel;
    SelectionMode mode = SelectionMode::Single;
    SelectionUnit unit = SelectionUnit::Rows;
    u32 curCol = 0;
    // columns / rows
    bool movable = false, alternating = true;
    i32 pressCol = -1, moving = -1; ///< visual positions
    f32 pressX = 0.f, moveX = 0.f;
    u32 frozen = 0;
    f32 rowH = 0.f;
    // editing
    NodeHandle editor;
    u32 editRow = 0, editCol = 0;
    ItemId editItem = kRootItem;
    u32 triggers = kEditAll;
    bool closing = false;
    // double click
    i64 clickRow = -1;
    u32 clickCol = 0;
    i64 pressRow = -1;         ///< the row of the last left press on a row (-1: header, scroll bar, expander, no row)
    bool pressConsumed = false; ///< that press already acted as a double click (editor, expand / collapse) or a check
    // tooltips
    bool tips = true;
    f32 tipDelay = 0.5f;
    NodeHandle tip;
    i64 hoverRow = -1;
    u32 hoverCol = 0, hoverSerial = 0;
    // type-ahead
    std::string typed;
    u32 typeSerial = 0;
    i32 typeCol = 0;
    // hooks
    ViewContextMenuFn menuFn = nullptr;
    void* menuUser = nullptr;
    ViewIconPainter iconFn = nullptr;
    void* iconUser = nullptr;
    // tree
    std::unique_ptr<TreeRows> rows;
    bool fetching = false;
};
// Process-wide pool (a context is single-threaded and states are never freed, so a subscription callback can
// always look at its state and see by the serial that the slot moved on).
std::vector<std::unique_ptr<ViewState>>& pool() { static std::vector<std::unique_ptr<ViewState>> p; return p; }
u32& serialCounter() { static u32 s = 0; return s; }
ViewState* stateOf(const UiContext& ui, NodeHandle n) {
    const u64 slot = ui.alive(n) ? ui.userData(n) : 0;
    auto& p = pool();
    if (slot == 0 || slot > p.size()) return nullptr;
    ViewState* s = p[slot - 1].get();
    return s->node == n && s->uid == ui.uid() ? s : nullptr;
}
ViewState& newState(UiContext& ui, NodeHandle n) {
    auto& p = pool();
    std::size_t at = p.size();
    for (std::size_t i = 0; i < p.size(); ++i)
        if (!UiContext::exists(p[i]->uid) || (p[i]->uid == ui.uid() && (!ui.alive(p[i]->node) || ui.userData(p[i]->node) != i + 1))) { at = i; break; }
    if (at == p.size()) p.push_back(std::make_unique<ViewState>());
    *p[at] = ViewState{};
    p[at]->ui = &ui;
    p[at]->uid = ui.uid();
    p[at]->node = n;
    p[at]->serial = ++serialCounter();
    for (u32 c = 0; c < kMaxCols; ++c) p[at]->order[c] = u8(c);
    ui.setUserData(n, at + 1);
    return *p[at];
}
/// Subscription tag -> state, or null when the slot was reused / the node died.
ViewState* liveState(void* user, u64 tag) {
    auto* s = static_cast<ViewState*>(user);
    return s->serial == u32(tag) && s->ui && UiContext::exists(s->uid) && s->ui->alive(s->node) && s->ui->userData(s->node) != 0 ? s : nullptr;
}

u32 cols(const ViewState& s) { return s.model ? std::min(s.model->columns, kMaxCols) : 0u; }
f32 colWidth(const ViewState& s, u32 k) { return (s.hiddenMask >> k & 1u) ? 0.f : s.colW[k]; }
f32 fontRowHeight(const UiContext& ui, NodeHandle n) {
    const f32 px = ui.computed(n).fontSize;
    return std::ceil(ui.font() ? ui.font()->metrics(px).lineHeight() : px * 1.25f) + 6.f;
}
f32 rowHeight(const UiContext& ui, NodeHandle n, const ViewState& s) { return s.rowH > 0.f ? s.rowH : fontRowHeight(ui, n); }
u32 rowCount(const ViewState& s) {
    if (s.tree) return s.rows ? s.rows->nodes[0].visible : 0u;
    return s.model && s.model->rows ? s.model->rows(s.model->user) : 0u;
}

// ---- tree bookkeeping ------------------------------------------------------------------------------------------------
u32 modelChildCount(const ViewState& s, ItemId id) { return s.model && s.model->childCount ? s.model->childCount(s.model->user, id) : 0u; }
/// Children of a node that is being shown: a lazy model with none fetched yet fetches its first batch now.
u32 openChildCount(ViewState& s, ItemId id) {
    u32 n = modelChildCount(s, id);
    if (n == 0 && s.model && s.model->canFetchMore && s.model->fetchMore && s.model->canFetchMore(s.model->user, id)) {
        s.fetching = true;
        s.model->fetchMore(s.model->user, id);
        s.fetching = false;
        n = modelChildCount(s, id);
    }
    return n;
}
void treeReset(ViewState& s) {
    if (!s.rows) s.rows = std::make_unique<TreeRows>();
    TreeRows& t = *s.rows;
    t.nodes.clear();
    t.freeList.clear();
    t.byId.clear();
    TreeNode root;
    root.used = true;
    root.count = root.visible = openChildCount(s, kRootItem);
    t.nodes.push_back(std::move(root));
    t.byId[kRootItem] = 0;
}
RowRef locate(const ViewState& s, u32 row) {
    const TreeRows& t = *s.rows;
    RowRef ref;
    i32 n = 0;
    u32 r = row, depth = 0;
    for (;;) {
        const TreeNode& node = t.nodes[std::size_t(n)];
        u32 before = 0; // visible rows of the expanded children passed so far
        i32 into = kNoNode;
        ref = {};
        ref.parent = n;
        ref.depth = depth;
        bool found = false;
        for (const i32 k : node.kids) {
            const TreeNode& kid = t.nodes[std::size_t(k)];
            const u32 pos = kid.row + before; // the kid's own row inside `node`
            if (r < pos) break;
            if (r == pos) { ref.childRow = kid.row; ref.node = k; ref.kids = true; found = true; break; }
            if (r <= pos + kid.visible) { into = k; r -= pos + 1; break; }
            before += kid.visible;
        }
        if (into != kNoNode) { n = into; ++depth; continue; }
        if (!found) ref.childRow = r - before;
        ref.id = s.model->childAt(s.model->user, node.id, ref.childRow);
        if (!found) {
            ref.kids = s.model->hasChildren ? s.model->hasChildren(s.model->user, ref.id) : modelChildCount(s, ref.id) > 0;
        }
        return ref;
    }
}
i64 rowOfNode(const TreeRows& t, i32 n);
/// Visible row of child `childRow` of record `p`.
i64 rowOfChild(const TreeRows& t, i32 p, u32 childRow) {
    const TreeNode& node = t.nodes[std::size_t(p)];
    u64 off = childRow;
    for (const i32 k : node.kids) {
        const TreeNode& kid = t.nodes[std::size_t(k)];
        if (kid.row >= childRow) break;
        off += kid.visible;
    }
    return (p == 0 ? 0 : rowOfNode(t, p) + 1) + i64(off);
}
i64 rowOfNode(const TreeRows& t, i32 n) {
    if (n <= 0) return -1;
    const TreeNode& node = t.nodes[std::size_t(n)];
    return rowOfChild(t, node.parent, node.row);
}
void addVisible(TreeRows& t, i32 n, i64 delta) {
    for (; n != kNoNode; n = t.nodes[std::size_t(n)].parent) t.nodes[std::size_t(n)].visible = u32(i64(t.nodes[std::size_t(n)].visible) + delta);
}
void freeSubtree(TreeRows& t, i32 n) {
    TreeNode& node = t.nodes[std::size_t(n)];
    for (const i32 k : node.kids) freeSubtree(t, k);
    t.byId.erase(node.id);
    node = TreeNode{};
    t.freeList.push_back(n);
}

// ---- geometry --------------------------------------------------------------------------------------------------------
struct Geo {
    Rect c;
    f32 rh = 0.f, hh = 0.f, total = 0.f, frozenW = 0.f, maxX = 0.f;
    u32 rows = 0, vis = 1;
    bool hbar = false;
    f32 bodyTop() const { return c.y + hh; }
    f32 bodyBottom() const { return c.bottom() - (hbar ? kBar : 0.f); }
};
Geo geo(const UiContext& ui, NodeHandle n, const ViewState& s) {
    Geo g;
    g.c = ui.contentRect(n);
    g.rh = rowHeight(ui, n, s);
    g.hh = s.header ? fontRowHeight(ui, n) : 0.f;
    const u32 nc = cols(s);
    if (!s.list)
        for (u32 k = 0; k < nc; ++k) g.total += colWidth(s, k);
    g.hbar = !s.list && g.total > g.c.w + 0.5f;
    g.rows = rowCount(s);
    g.vis = std::max(1u, u32((g.c.h - g.hh - (g.hbar ? kBar : 0.f)) / g.rh));
    if (g.hbar) {
        g.maxX = g.total - g.c.w + (g.rows > g.vis ? kBar : 0.f);
        for (u32 v = 0; v < nc && v < s.frozen; ++v) g.frozenW += colWidth(s, s.order[v]);
        if (g.frozenW > g.c.w * 0.75f) g.frozenW = 0.f; // frozen columns that would hide everything else scroll
    }
    return g;
}
/// f(model column, visual position, x, width, frozen) for each shown column, left to right; false stops.
template <class F>
void eachCol(const ViewState& s, const Geo& g, F&& f) {
    const u32 nc = cols(s);
    f32 x = g.c.x;
    for (u32 v = 0; v < nc; ++v) {
        const u32 k = s.order[v];
        const f32 w = colWidth(s, k);
        if (w <= 0.f) continue;
        const bool frozen = g.frozenW > 0.f && v < s.frozen;
        if (!f(k, v, frozen ? x : x - s.scrollX, w, frozen)) return;
        x += w;
    }
}
/// Horizontal clip of a column's cells (the scrolled columns stop at the frozen ones and the view edge).
Rect clipCol(const Geo& g, bool frozen, Rect r) {
    if (!g.hbar) return r;
    const f32 left = frozen ? g.c.x : g.c.x + g.frozenW;
    return r.intersect({left, r.y, g.c.right() - left, r.h});
}
struct ColHit { i32 col = -1; u32 visual = 0; f32 x = 0.f, w = 0.f; };
ColHit colAt(const ViewState& s, const Geo& g, f32 px) {
    ColHit hit;
    eachCol(s, g, [&](u32 k, u32 v, f32 x, f32 w, bool frozen) {
        if (px < x || px >= x + w) return true;
        if (!frozen && g.frozenW > 0.f && px < g.c.x + g.frozenW) return true;
        hit = {i32(k), v, x, w};
        return false;
    });
    return hit;
}
bool vbarGeom(const Geo& g, const ViewState& s, Rect& track, Rect& thumb) {
    if (g.rows <= g.vis) return false;
    track = {g.c.right() - kBar, g.bodyTop(), kBar, g.c.bottom() - g.bodyTop()}; // the corner belongs to this bar
    const f32 tl = std::max(16.f, track.h * f32(g.vis) / f32(g.rows));
    thumb = {track.x, track.y + (track.h - tl) * f32(s.first) / f32(g.rows - g.vis), kBar, tl};
    return true;
}
bool hbarGeom(const Geo& g, const ViewState& s, Rect& track, Rect& thumb) {
    if (!g.hbar || g.maxX <= 0.f) return false;
    track = {g.c.x, g.c.bottom() - kBar, g.c.w - (g.rows > g.vis ? kBar : 0.f), kBar};
    const f32 tl = std::max(16.f, track.w * track.w / (track.w + g.maxX));
    thumb = {track.x + (track.w - tl) * std::clamp(s.scrollX / g.maxX, 0.f, 1.f), track.y, tl, kBar};
    return true;
}

ItemId itemOf(const ViewState& s, u32 row) { return s.tree ? locate(s, row).id : ItemId(row); }
std::string_view cellText(ViewState& s, const RowRef& ref, u32 row, u32 k) {
    if (s.tree) return s.model->itemData ? s.model->itemData(s.model->user, ref.id, k, s.scratch) : std::string_view{};
    return s.model->data(s.model->user, row, k, s.scratch);
}
u32 itemFlags(const ViewState& s, ItemId item, u32 col) {
    return s.model->flags ? s.model->flags(s.model->user, item, col) : u32(kItemSelectable | kItemEnabled);
}
bool rowSelected(const UiContext& ui, const ViewState& s, u32 row) {
    return s.sel ? s.sel->isSelected(row, 0) : i64(ui.value(s.node)) == i64(row);
}
i64 currentRow(const UiContext& ui, const ViewState& s) { return s.sel ? s.sel->currentRow() : i64(ui.value(s.node)); }

/// Rect of a cell's text area in context coordinates (tree column: after the indent), clipped; empty when not shown.
Rect cellRect(const UiContext& ui, NodeHandle n, const ViewState& s, u32 row, u32 col) {
    const Geo g = geo(ui, n, s);
    if (row < s.first || row >= s.first + g.vis || row >= g.rows) return {};
    Rect out;
    eachCol(s, g, [&](u32 k, u32, f32 x, f32 w, bool frozen) {
        if (k != col) return true;
        f32 x0 = x;
        if (s.tree && k == 0) x0 += 4.f + f32(locate(s, row).depth) * kIndent + 14.f - 6.f;
        out = clipCol(g, frozen, {x0, g.bodyTop() + f32(row - s.first) * g.rh, std::max(0.f, x + w - x0), g.rh});
        out = out.intersect(g.c);
        return false;
    });
    return out;
}

// ---- editing ---------------------------------------------------------------------------------------------------------
void placeEditor(UiContext& ui, NodeHandle n, ViewState& s, Rect r) {
    const Rect c = ui.contentRect(n);
    setPx(ui, s.editor, Prop::Left, r.x - c.x);
    setPx(ui, s.editor, Prop::Top, r.y - c.y);
    setPx(ui, s.editor, Prop::Width, r.w);
    setPx(ui, s.editor, Prop::Height, r.h);
    setPx(ui, s.editor, Prop::MinHeight, r.h);
}
bool inSubtree(const UiContext& ui, NodeHandle n, NodeHandle root) {
    for (; ui.alive(n); n = ui.parent(n))
        if (n == root) return true;
    return false;
}
void closeEditor(UiContext& ui, NodeHandle n, ViewState& s) {
    if (!ui.alive(s.editor)) { s.editor = {}; return; }
    s.closing = true;
    const NodeHandle ed = s.editor;
    s.editor = {};
    if (inSubtree(ui, ui.focused(), ed)) ui.setFocus(n, FocusReason::Keyboard);
    ui.destroy(ed);
    ui.invalidate(n);
    s.closing = false;
}
bool commitEditor(UiContext& ui, NodeHandle n, ViewState& s) {
    if (!ui.alive(s.editor)) { s.editor = {}; return false; }
    ItemValue out;
    bool ok = true;
    const CellDelegate* d = s.editCol < kMaxCols ? s.delegate[s.editCol] : nullptr;
    if (d && d->commit) {
        ok = d->commit(d->user, ui, s.editor, out, s.scratch2);
    } else {
        s.scratch2 = ui.editBuffer(s.editor).text();
        out.text = s.scratch2;
        parseNum(out.text, out.number);
    }
    // the edit is over before the model hears of it: its dataChanged report must not find an editor to sync
    const NodeHandle ed = s.editor;
    s.editor = {};
    if (ok) ok = s.model && s.model->setData && s.model->setData(s.model->user, s.editItem, s.editCol, ItemRole::Edit, out);
    s.editor = ed;
    closeEditor(ui, n, s);
    return ok;
}
/// Keeps the editor over its cell; a cell that left the window commits.
void syncEditor(UiContext& ui, NodeHandle n, ViewState& s) {
    if (!s.editor.valid()) return;
    if (!ui.alive(s.editor)) { s.editor = {}; return; }
    const Rect r = cellRect(ui, n, s, s.editRow, s.editCol);
    if (r.w < 8.f || r.h <= 0.f) { commitEditor(ui, n, s); return; }
    placeEditor(ui, n, s, r);
}
bool cellEditable(const ViewState& s, u32 row, u32 col) {
    if (!s.model || !s.model->setData || !s.model->flags || col >= cols(s) || row >= rowCount(s)) return false;
    const u32 f = itemFlags(s, itemOf(s, row), col);
    return (f & kItemEditable) && (f & kItemEnabled);
}
bool beginEdit(UiContext& ui, NodeHandle n, ViewState& s, u32 row, u32 col, std::string_view typed = {}) {
    if (s.editor.valid()) commitEditor(ui, n, s);
    if (!cellEditable(s, row, col)) return false;
    const Rect r = cellRect(ui, n, s, row, col);
    if (r.w < 8.f || r.h <= 0.f) return false;
    const RowRef ref = s.tree ? locate(s, row) : RowRef{.id = row};
    const CellDelegate* d = s.delegate[col];
    // the Edit value: the role, else the display text; number = what the text parses to
    ItemValue v;
    std::string text;
    if (s.model->role && s.model->role(s.model->user, ref.id, col, ItemRole::Edit, v, s.scratch2)) text.assign(v.text);
    else text.assign(cellText(s, ref, row, col));
    if (v.number == 0.0) parseNum(text, v.number);
    const bool custom = d && d->createEditor;
    const NodeHandle ed = custom ? d->createEditor(d->user, ui, n, ref.id, col) : lineEdit(ui, n, {});
    if (!ui.alive(ed)) return false;
    v.text = text; // (node creation invalidated nothing of ours: `text` is a copy)
    if (d && d->setEditorData) {
        d->setEditorData(d->user, ui, ed, v);
    } else if (!custom) {
        ui.setText(ed, text);
        ui.editBuffer(ed).setText(text);
        ui.editBuffer(ed).selectAll();
    }
    s.editor = ed;
    s.editRow = row;
    s.editCol = col;
    s.editItem = ref.id;
    // accessibility: the editor is named after its column (it is a real child node with its class's role)
    if (s.model->header) ui.setA11y(ed, ui.role(ed), s.model->header(s.model->user, col));
    placeEditor(ui, n, s, r);
    ui.setFocus(ed, FocusReason::Keyboard);
    ui.invalidate(n);
    if (!typed.empty() && ui.focused() == ed) { // the key that opened the editor is its first input
        const std::string first(typed); // the event text does not outlive a nested dispatch
        ui.textInput(first);
    }
    return true;
}

// ---- scrolling / current ----------------------------------------------------------------------------------------------
void setFirst(UiContext& ui, NodeHandle n, ViewState& s, i64 f) {
    const Geo g = geo(ui, n, s);
    const u32 nf = u32(std::clamp<i64>(f, 0, g.rows > g.vis ? i64(g.rows - g.vis) : 0));
    if (nf != s.first) { s.first = nf; ui.invalidate(n); syncEditor(ui, n, s); }
}
void setScrollX(UiContext& ui, NodeHandle n, ViewState& s, f32 x) {
    const Geo g = geo(ui, n, s);
    x = std::round(std::clamp(x, 0.f, std::max(0.f, g.maxX)));
    if (x != s.scrollX) { s.scrollX = x; ui.invalidate(n); syncEditor(ui, n, s); }
}
void scrollToRow(UiContext& ui, NodeHandle n, ViewState& s, i64 row) {
    const Geo g = geo(ui, n, s);
    if (row < i64(s.first)) setFirst(ui, n, s, row);
    else if (row >= i64(s.first + g.vis)) setFirst(ui, n, s, row - g.vis + 1);
}
/// Keeps a column in view horizontally.
void scrollToCol(UiContext& ui, NodeHandle n, ViewState& s, u32 col) {
    const Geo g = geo(ui, n, s);
    if (!g.hbar) return;
    eachCol(s, g, [&](u32 k, u32, f32 x, f32 w, bool frozen) {
        if (k != col) return true;
        const f32 left = g.c.x + g.frozenW, right = g.c.right() - (g.rows > g.vis ? kBar : 0.f);
        if (frozen) return false;
        if (x < left) setScrollX(ui, n, s, s.scrollX - (left - x));
        else if (x + w > right) setScrollX(ui, n, s, s.scrollX + std::min(x - left, x + w - right));
        return false;
    });
}
enum class Via : u8 { Pointer, Key };
/// Makes (row, col) current and applies the selection rules; plain single mode sets value().
void setCurrent(UiContext& ui, NodeHandle n, ViewState& s, i64 row, u32 col, u8 mods, Via via) {
    const u32 r = rowCount(s);
    if (!r) return;
    row = std::clamp<i64>(row, 0, i64(r) - 1);
    scrollToRow(ui, n, s, row);
    s.curCol = col;
    if (s.sel) {
        if (via == Via::Pointer) s.sel->pointerSelect(u32(row), col, mods, s.mode, s.unit);
        else s.sel->keySelect(u32(row), col, mods, s.mode, s.unit);
    } else {
        ui.setValue(n, f32(row), true);
    }
    if (s.unit == SelectionUnit::Cells) { scrollToCol(ui, n, s, col); ui.invalidate(n); }
}
bool selectionChanged(void* user, u64 tag, const SelectionModel& sel) {
    ViewState* s = liveState(user, tag);
    if (!s || s->sel != &sel) return false;
    UiContext& ui = *s->ui;
    const f32 cur = f32(sel.currentRow());
    s->curCol = sel.currentColumn();
    ui.invalidate(s->node);
    if (ui.value(s->node) != cur) ui.setValue(s->node, cur, true);
    else ui.emit(s->node, SignalKind::ValueChanged, {.value = cur, .flag = true});
    return true;
}
void attachSelection(UiContext& ui, NodeHandle n, ViewState& s, SelectionModel* shared) {
    if (shared) {
        s.ownSel.reset();
        s.sel = shared;
        if (!s.tree && s.model && s.model->notifier && !shared->tracking()) shared->track(s.model->notifier);
    } else {
        s.ownSel = std::make_unique<SelectionModel>();
        s.sel = s.ownSel.get();
        if (const i64 v = i64(ui.value(n)); v >= 0 && s.mode != SelectionMode::None) { // carry the plain selection over
            s.sel->selectRow(u32(v));
            s.sel->setCurrent(v, s.curCol);
            s.sel->setAnchor(v, s.curCol);
        }
    }
    s.sel->subscribe(&selectionChanged, &s, s.serial);
    ui.invalidate(n);
}

// ---- tree: expand / collapse / fetch ----------------------------------------------------------------------------------
/// Row bookkeeping shared by the model notifications and the tree: `count` rows appeared at visible row `first`.
void rowsAppeared(UiContext& ui, NodeHandle n, ViewState& s, u32 first, u32 count, bool adjustSel) {
    if (!count) return;
    if (s.sel) {
        if (adjustSel) s.sel->rowsInserted(first, count);
    } else if (const i64 v = i64(ui.value(n)); v >= i64(first)) {
        ui.setValue(n, f32(v + count), true);
    }
    if (first < s.first || (first == s.first && s.first > 0)) s.first += count; // the same rows stay in view
    if (s.editor.valid() && s.editRow >= first) s.editRow += count;
    ui.invalidate(n);
}
void rowsVanished(UiContext& ui, NodeHandle n, ViewState& s, u32 first, u32 count, bool adjustSel) {
    if (!count) return;
    const u64 last = u64(first) + count - 1;
    if (s.editor.valid()) {
        if (s.editRow >= first && s.editRow <= last) closeEditor(ui, n, s);
        else if (s.editRow > last) s.editRow -= count;
    }
    if (s.sel) {
        if (adjustSel) s.sel->rowsRemoved(first, count);
    } else if (const i64 v = i64(ui.value(n)); v >= i64(first)) {
        ui.setValue(n, v > i64(last) ? f32(v - count) : -1.f, true);
    }
    if (s.first > last) s.first -= count;
    else if (s.first > first) s.first = first;
    ui.invalidate(n);
}
/// After any row-count change: clamps the scroll position and the current row.
void clampView(UiContext& ui, NodeHandle n, ViewState& s) {
    const Geo g = geo(ui, n, s);
    const u32 maxFirst = g.rows > g.vis ? g.rows - g.vis : 0u;
    if (s.first > maxFirst) { s.first = maxFirst; ui.invalidate(n); }
    if (s.scrollX > g.maxX) { s.scrollX = std::max(0.f, g.maxX); ui.invalidate(n); }
    if (s.sel) {
        if (s.sel->currentRow() >= i64(g.rows)) s.sel->setCurrent(i64(g.rows) - 1, s.sel->currentColumn());
    } else if (ui.value(n) >= f32(g.rows)) {
        ui.setValue(n, -1.f, true);
    }
    syncEditor(ui, n, s);
}
bool expandRow(UiContext& ui, NodeHandle n, ViewState& s, u32 row) {
    if (!s.tree || row >= rowCount(s)) return false;
    const RowRef ref = locate(s, row);
    if (ref.node != kNoNode || !ref.kids) return false;
    TreeRows& t = *s.rows;
    i32 idx;
    if (!t.freeList.empty()) { idx = t.freeList.back(); t.freeList.pop_back(); }
    else { idx = i32(t.nodes.size()); t.nodes.emplace_back(); }
    const u32 count = openChildCount(s, ref.id);
    TreeNode& node = t.nodes[std::size_t(idx)];
    node.used = true;
    node.id = ref.id;
    node.parent = ref.parent;
    node.row = ref.childRow;
    node.count = node.visible = count;
    auto& kids = t.nodes[std::size_t(ref.parent)].kids;
    kids.insert(std::upper_bound(kids.begin(), kids.end(), ref.childRow, [&](u32 r, i32 k) { return r < t.nodes[std::size_t(k)].row; }), idx);
    t.byId[ref.id] = idx;
    addVisible(t, ref.parent, count);
    rowsAppeared(ui, n, s, row + 1, count, true); // rows above the window keep the window on the same rows
    clampView(ui, n, s);
    ui.invalidate(n);
    ui.emit(n, SignalKind::Toggled, {.value = f32(row), .flag = true, .text = "expand"});
    return true;
}
bool collapseRow(UiContext& ui, NodeHandle n, ViewState& s, u32 row) {
    if (!s.tree || row >= rowCount(s)) return false;
    const RowRef ref = locate(s, row);
    if (ref.node == kNoNode) return false;
    TreeRows& t = *s.rows;
    const u32 gone = t.nodes[std::size_t(ref.node)].visible;
    auto& kids = t.nodes[std::size_t(ref.parent)].kids;
    kids.erase(std::find(kids.begin(), kids.end(), ref.node));
    freeSubtree(t, ref.node);
    addVisible(t, ref.parent, -i64(gone));
    const i64 cur = currentRow(ui, s);
    const bool inside = cur > i64(row) && cur <= i64(row) + gone;
    rowsVanished(ui, n, s, row + 1, gone, true);
    if (inside) { // a current row that collapsed away hands over to the collapsed node
        if (!s.sel) ui.setValue(n, f32(row), true);
        else if (s.mode == SelectionMode::Single) s.sel->pointerSelect(row, s.curCol, 0, s.mode, s.unit);
        else s.sel->setCurrent(row, s.curCol);
    }
    clampView(ui, n, s);
    ui.invalidate(n);
    ui.emit(n, SignalKind::Toggled, {.value = f32(row), .flag = false, .text = "expand"});
    return true;
}
/// Re-reads the child count of an expanded record after fetchMore (new rows are appended).
void syncCount(UiContext& ui, NodeHandle n, ViewState& s, i32 idx) {
    TreeRows& t = *s.rows;
    const u32 old = t.nodes[std::size_t(idx)].count, now = modelChildCount(s, t.nodes[std::size_t(idx)].id);
    if (now <= old) return;
    const i64 at = rowOfChild(t, idx, old);
    t.nodes[std::size_t(idx)].count = now;
    addVisible(t, idx, i64(now - old));
    rowsAppeared(ui, n, s, u32(at), now - old, true);
}
/// Lazy children: fetches more for a parent whose last fetched child is in the window (a few per frame at most).
void fetchVisible(UiContext& ui, NodeHandle n, ViewState& s) {
    if (!s.tree || !s.model || !s.model->canFetchMore || !s.model->fetchMore) return;
    for (int round = 0; round < 4; ++round) {
        const Geo g = geo(ui, n, s);
        bool fetched = false;
        for (u32 i = 0; i <= g.vis && s.first + i < g.rows; ++i) {
            const RowRef ref = locate(s, s.first + i);
            const TreeNode& p = s.rows->nodes[std::size_t(ref.parent)];
            if (ref.childRow + 1 != p.count || !s.model->canFetchMore(s.model->user, p.id)) continue;
            s.fetching = true;
            s.model->fetchMore(s.model->user, p.id);
            s.fetching = false;
            syncCount(ui, n, s, ref.parent);
            fetched = true;
            break;
        }
        if (!fetched) return;
    }
}

// ---- model notifications ----------------------------------------------------------------------------------------------
bool modelChanged(void* user, u64 tag, const ModelChange& c) {
    ViewState* sp = liveState(user, tag);
    if (!sp) return false;
    ViewState& s = *sp;
    UiContext& ui = *s.ui;
    const NodeHandle n = s.node;
    if (s.fetching) return true; // the view syncs the count itself after fetchMore
    using K = ModelChange::Kind;
    if (c.kind == K::Reset) {
        if (s.editor.valid()) closeEditor(ui, n, s);
        if (s.tree) { treeReset(s); if (s.sel) s.sel->clear(); }
        else if (s.ownSel) s.ownSel->clear();
        clampView(ui, n, s);
        ui.invalidate(n);
        return true;
    }
    if (!s.tree) {
        if (c.parent != kRootItem) return true;
        const bool own = s.sel && s.sel == s.ownSel.get(); // a shared selection model tracks the notifier itself
        switch (c.kind) {
        case K::RowsInserted: rowsAppeared(ui, n, s, c.first, c.count, own); break;
        case K::RowsRemoved: rowsVanished(ui, n, s, c.first, c.count, own); break;
        case K::RowsMoved:
            if (s.editor.valid()) commitEditor(ui, n, s);
            if (own) s.sel->rowsMoved(c.first, c.count, c.dest);
            else if (!s.sel) {
                const i64 v = i64(ui.value(n));
                if (v >= i64(c.first) && v < i64(c.first) + c.count) ui.setValue(n, f32(v - c.first + c.dest), true);
                else if (v >= 0) {
                    i64 w = v > i64(c.first) ? v - c.count : v; // without the block
                    if (w >= i64(c.dest)) w += c.count;        // with it back in
                    ui.setValue(n, f32(w), true);
                }
            }
            ui.invalidate(n);
            break;
        case K::DataChanged: { // repaint only when the rows are in the window
            const Geo g = geo(ui, n, s);
            if (u64(c.first) + c.count > s.first && c.first <= s.first + g.vis) ui.invalidate(n);
            break;
        }
        case K::Reset: break;
        }
        clampView(ui, n, s);
        return true;
    }
    // tree: only expanded parents have rows on screen
    TreeRows& t = *s.rows;
    const auto it = t.byId.find(c.parent);
    if (it == t.byId.end()) { ui.invalidate(n); return true; } // a collapsed parent may have gained / lost its expander
    const i32 idx = it->second;
    auto removeRows = [&](u32 first, u32 count) {
        TreeNode& p = t.nodes[std::size_t(idx)];
        count = std::min(count, p.count > first ? p.count - first : 0u);
        if (!count) return;
        const i64 at = rowOfChild(t, idx, first);
        u32 gone = count;
        for (std::size_t k = 0; k < p.kids.size();) {
            TreeNode& kid = t.nodes[std::size_t(p.kids[k])];
            if (kid.row >= first && kid.row < first + count) {
                gone += kid.visible;
                freeSubtree(t, p.kids[k]);
                p.kids.erase(p.kids.begin() + std::ptrdiff_t(k));
                continue;
            }
            if (kid.row >= first + count) kid.row -= count;
            ++k;
        }
        p.count -= count;
        addVisible(t, idx, -i64(gone));
        rowsVanished(ui, n, s, u32(at), gone, true);
    };
    auto insertRows = [&](u32 first, u32 count) {
        TreeNode& p = t.nodes[std::size_t(idx)];
        first = std::min(first, p.count);
        const i64 at = rowOfChild(t, idx, first);
        for (const i32 k : p.kids)
            if (t.nodes[std::size_t(k)].row >= first) t.nodes[std::size_t(k)].row += count;
        p.count += count;
        addVisible(t, idx, count);
        rowsAppeared(ui, n, s, u32(at), count, true);
    };
    switch (c.kind) {
    case K::RowsInserted: insertRows(c.first, c.count); break;
    case K::RowsRemoved: removeRows(c.first, c.count); break;
    case K::RowsMoved: removeRows(c.first, c.count); insertRows(c.dest, c.count); break; // moved nodes come back collapsed
    case K::DataChanged: {
        const Geo g = geo(ui, n, s);
        const i64 a = rowOfChild(t, idx, c.first), b = rowOfChild(t, idx, std::min(c.first + c.count, t.nodes[std::size_t(idx)].count));
        if (b > i64(s.first) && a <= i64(s.first + g.vis)) ui.invalidate(n);
        break;
    }
    case K::Reset: break;
    }
    clampView(ui, n, s);
    return true;
}

// ---- tooltips, double click, type-ahead --------------------------------------------------------------------------------
void hideTip(UiContext& ui, ViewState& s) {
    if (ui.alive(s.tip) && ui.computed(s.tip).visible) ui.setCss(s.tip, "display", "none");
}
void tipTimer(UiContext& ui, NodeHandle n, u32 serial) {
    ViewState* s = stateOf(ui, n);
    if (!s || s->hoverSerial != serial || s->hoverRow < 0 || ui.hovered() != n || !s->model) return;
    const Geo g = geo(ui, n, *s);
    const u32 row = u32(s->hoverRow), col = s->hoverCol;
    if (row >= g.rows || col >= cols(*s)) return;
    const RowRef ref = s->tree ? locate(*s, row) : RowRef{.id = row};
    std::string text;
    ItemValue v;
    if (s->model->role && s->model->role(s->model->user, ref.id, col, ItemRole::ToolTip, v, s->scratch2)) {
        text.assign(v.text);
    } else { // elided: the text is wider than the cell's text area
        const std::string_view d = cellText(*s, ref, row, col);
        const Rect r = cellRect(ui, n, *s, row, col);
        const f32 avail = s->list ? std::min(r.w, g.c.w) - 12.f : r.w - 12.f;
        if (r.w > 0.f && ui.measureString(d, ui.computed(n).fontSize) > avail) text.assign(d);
    }
    if (text.empty()) return;
    if (!ui.alive(s->tip)) {
        s->tip = make(ui, "tooltip", overlayLayer(ui));
        ui.addClass(s->tip, "tooltip");
    }
    ui.setText(s->tip, text);
    setPx(ui, s->tip, Prop::Left, ui.pointer().x + 12.f);
    setPx(ui, s->tip, Prop::Top, ui.pointer().y + 16.f);
    overlayLayer(ui);
    ui.setCss(s->tip, "display", "flex");
}
void hoverCell(UiContext& ui, NodeHandle n, ViewState& s, i64 row, u32 col) {
    if (row == s.hoverRow && col == s.hoverCol) return;
    s.hoverRow = row;
    s.hoverCol = col;
    ++s.hoverSerial;
    hideTip(ui, s);
    if (!s.tips || row < 0) return;
    const u32 serial = s.hoverSerial;
    ui.startTimer(s.tipDelay, false, [n, serial](UiContext& c) { tipTimer(c, n, serial); });
}
/// True when this press is the second one of a double click (the context's click count: GestureConfig time and
/// slop, the same rule as the DoubleClick event that follows on release) and it is on the same cell as the first.
bool doubleClick(ViewState& s, const Event& e, i64 row, u32 col) {
    const bool again = e.clicks == 2 && s.clickRow == row && s.clickCol == col;
    s.clickRow = row;
    s.clickCol = col;
    return again;
}
bool typeAhead(UiContext& ui, NodeHandle n, ViewState& s, std::string_view text) {
    if (s.typeCol < 0 || u32(s.typeCol) >= cols(s) || text.empty() || static_cast<unsigned char>(text[0]) < 0x20) return false;
    const u32 rows = rowCount(s);
    if (!rows) return false;
    for (const char ch : text) s.typed += char(std::tolower(static_cast<unsigned char>(ch)));
    const u32 serial = ++s.typeSerial;
    ui.startTimer(1.f, false, [n, serial](UiContext& c) {
        if (ViewState* st = stateOf(c, n); st && st->typeSerial == serial) st->typed.clear();
    });
    const i64 cur = currentRow(ui, s);
    // a longer prefix refines from the current row, a first letter moves on to the next match
    u32 row = cur < 0 ? 0u : (u32(cur) + (s.typed.size() == text.size() ? 1u : 0u)) % rows;
    const u32 scan = std::min(rows, kTypeAheadScanRows);
    for (u32 i = 0; i < scan; ++i, row = row + 1 == rows ? 0u : row + 1) {
        const RowRef ref = s.tree ? locate(s, row) : RowRef{.id = row};
        const std::string_view d = cellText(s, ref, row, u32(s.typeCol));
        if (d.size() < s.typed.size()) continue;
        bool match = true;
        for (std::size_t k = 0; k < s.typed.size() && match; ++k) match = char(std::tolower(static_cast<unsigned char>(d[k]))) == s.typed[k];
        if (!match) continue;
        setCurrent(ui, n, s, row, s.curCol, 0, Via::Key);
        return true;
    }
    return true; // consumed: the key belongs to the search even without a match
}
bool toggleCheck(UiContext& ui, NodeHandle n, ViewState& s, u32 row, u32 col) {
    if (!s.model || !s.model->role || !s.model->setData || col >= cols(s)) return false;
    const ItemId item = itemOf(s, row);
    const u32 f = itemFlags(s, item, col);
    ItemValue v;
    if (!(f & kItemCheckable) || !(f & kItemEnabled) || !s.model->role(s.model->user, item, col, ItemRole::CheckState, v, s.scratch2)) return false;
    ItemValue nv;
    nv.integer = v.integer == 1 ? 0 : 1;
    nv.number = nv.integer;
    s.model->setData(s.model->user, item, col, ItemRole::CheckState, nv);
    ui.invalidate(n);
    return true;
}
/// The next (or previous) editable cell after (row, col) in reading order; false when none within a bounded scan.
bool nextEditable(const ViewState& s, u32& row, u32& col, bool backward) {
    const u32 nc = cols(s), rows = rowCount(s);
    if (!nc || !rows) return false;
    u32 v = 0;
    for (u32 i = 0; i < nc; ++i)
        if (s.order[i] == col) v = i;
    i64 r = row, pos = v;
    for (u32 guard = 0; guard < 1024; ++guard) {
        pos += backward ? -1 : 1;
        if (pos >= i64(nc)) { pos = 0; ++r; }
        if (pos < 0) { pos = i64(nc) - 1; --r; }
        if (r < 0 || r >= i64(rows)) return false;
        const u32 k = s.order[pos];
        if (colWidth(s, k) > 0.f && cellEditable(s, u32(r), k)) { row = u32(r); col = k; return true; }
    }
    return false;
}

// ---- the widget class ------------------------------------------------------------------------------------------------
struct ItemView {
    static constexpr std::string_view kName = "tableview";
    static constexpr A11yRole kRole = A11yRole::List;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static ComputedStyle defaults() {
        ComputedStyle s;
        s.layout = LayoutKind::Absolute; // the only child is the editor, placed over its cell
        return s;
    }
    // touch: a drag on a scroll bar / a column edge stays with the view, elsewhere rows follow the finger
    static TouchDrag touchDrag(UiContext& ui, NodeHandle n, Vec2) {
        const ViewState* s = stateOf(ui, n);
        if (!s) return {};
        if (s->dragScroll || s->resizing >= 0 || s->moving >= 0) return {TouchDrag::Mode::Capture, {}, {}};
        const Geo g = geo(ui, n, *s);
        // rows follow the finger (3 rows per notch); a view wider than its columns pans sideways (40 px per notch)
        const bool vy = g.rows > g.vis, vx = g.hbar && g.maxX > 0.f;
        if (!vy && !vx) return {};
        return {TouchDrag::Mode::Wheel, {vx ? 8.f : 0.f, vy ? g.rh : 0.f}, {vx ? 0.2f : 0.f, vy ? 1.f / 3.f : 0.f}};
    }
    /// Column edges of the header row resize (a running column resize keeps the cursor); a header being dragged to
    /// another position shows Move.
    static Cursor cursor(UiContext& ui, NodeHandle n, Vec2 p) {
        const ViewState* s = stateOf(ui, n);
        if (!s) return Cursor::Auto;
        if (s->resizing >= 0) return Cursor::ResizeH;
        if (s->moving >= 0) return Cursor::Move;
        if (s->dragScroll || !s->header) return Cursor::Auto;
        const Geo g = geo(ui, n, *s);
        if (p.y < g.c.y || p.y >= g.c.y + g.hh || p.x < g.c.x || p.x >= g.c.right()) return Cursor::Auto;
        Rect tr, th;
        if (vbarGeom(g, *s, tr, th) && tr.contains(p)) return Cursor::Auto;
        bool edge = false;
        eachCol(*s, g, [&](u32, u32, f32 x, f32 w, bool) { // the same test as the press
            edge = std::abs(p.x - (x + w)) <= 4.f;
            return !edge && p.x >= x + w;
        });
        return edge ? Cursor::ResizeH : Cursor::Auto;
    }
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        const ViewState* s = stateOf(ui, n);
        f32 w = 0.f;
        // (a list's one column is as wide as the view, not the other way round: its width is not an intrinsic size)
        if (s && !s->list) for (u32 c = 0; c < cols(*s); ++c) w += colWidth(*s, c);
        return {std::max(160.f, w + kBar), fontRowHeight(ui, n) * 8.f};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        ViewState* s = stateOf(ui, pc.node);
        if (!s || !s->model) return;
        fetchVisible(ui, pc.node, *s);
        const Geo g = geo(ui, pc.node, *s);
        const Rect c = pc.content;
        const f32 rh = g.rh, hh = g.hh, fs = pc.style.fontSize;
        const ItemModel& m = *s->model;
        const Color text = pc.tint(pc.style.color), dim = pc.tint(pc.style.color.withAlpha(pc.style.color.a * 0.65f));
        const Color acc = pc.tint(pc.style.background2.a > 0.f ? pc.style.background2 : themed(pc, tok::accent, 0x3D8BFD));
        const bool cells = s->sel && s->unit == SelectionUnit::Cells;
        dl.pushClip(c);
        if (s->header) {
            dl.rect({c.x, c.y, c.w, hh}, pc.tint(themed(pc, tok::raised, 0x2A2A33)));
            eachCol(*s, g, [&](u32 k, u32 v, f32 x, f32 w, bool frozen) {
                const Rect clip = clipCol(g, frozen, {x, c.y, w, hh});
                if (clip.empty()) return true;
                if (frozen && g.hbar) dl.rect(clip, pc.tint(themed(pc, tok::raised, 0x2A2A33)));
                if (s->moving == i32(v)) dl.rect(clip, acc.withAlpha(acc.a * 0.3f));
                const std::string_view h = m.header ? m.header(m.user, k) : std::string_view{};
                dl.pushClip(clip);
                ui.paintString(dl, h, {x + 6.f, c.y, w - 12.f, hh}, text, fs, TextAlignment::Start, pc.node.index);
                if (i32(k) == s->sortCol) { // sort arrow
                    const f32 ax = x + w - 12.f, ay = c.y + hh * 0.5f, d = s->asc ? -3.f : 3.f;
                    dl.line({ax - 4.f, ay - d}, {ax, ay + d}, acc, 1.5f);
                    dl.line({ax, ay + d}, {ax + 4.f, ay - d}, acc, 1.5f);
                }
                dl.popClip();
                if (const Rect sep = clipCol(g, frozen, {x + w - 1.f, c.y + 4.f, 1.f, hh - 8.f}); !sep.empty())
                    dl.rect(sep, pc.tint(themed(pc, tok::grid_line, 0x55555F)));
                return true;
            });
            if (s->moving >= 0) { // drop marker: where the dragged column would land
                f32 mx = c.x;
                bool found = false;
                eachCol(*s, g, [&](u32, u32, f32 x, f32 w, bool) {
                    found = s->moveX <= x + w * 0.5f;
                    mx = found ? x : x + w;
                    return !found;
                });
                dl.rect({std::clamp(mx - 1.f, c.x, c.right() - 2.f), c.y, 2.f, hh}, acc);
            }
        }
        if (g.hbar) dl.pushClip({c.x, g.bodyTop(), c.w, g.bodyBottom() - g.bodyTop()});
        const i64 selLegacy = i64(ui.value(pc.node));
        const i64 cur = s->sel ? s->sel->currentRow() : -1;
        const f32 bottom = g.bodyBottom();
        for (u32 i = 0; i < g.vis + 1 && s->first + i < g.rows; ++i) {
            const u32 row = s->first + i;
            const f32 y = c.y + hh + f32(i) * rh;
            if (y >= bottom) break;
            const bool selRow = s->sel ? (!cells && s->sel->isSelected(row, 0)) : i64(row) == selLegacy;
            const bool stripe = !selRow && (row & 1u) && s->alternating;
            if (selRow) dl.rect({c.x, y, c.w, rh}, acc.withAlpha(acc.a * 0.55f));
            else if (stripe) dl.rect({c.x, y, c.w, rh}, pc.tint(themed(pc, tok::stripe, 0xFFFFFF, 0.035f)));
            const RowRef ref = s->tree ? locate(*s, row) : RowRef{.id = row};
            eachCol(*s, g, [&](u32 k, u32, f32 x, f32 w, bool frozen) {
                Rect clip = clipCol(g, frozen, {x, y, w - 2.f, rh});
                if (clip.empty()) return true;
                if (frozen && g.hbar) { // frozen cells cover what scrolls under them
                    dl.rect({x, y, w, rh}, pc.tint(pc.style.background.a > 0.f ? pc.style.background : themed(pc, tok::bg, 0x1E1F24)));
                    if (selRow) dl.rect({x, y, w, rh}, acc.withAlpha(acc.a * 0.55f));
                    else if (stripe) dl.rect({x, y, w, rh}, pc.tint(themed(pc, tok::stripe, 0xFFFFFF, 0.035f)));
                }
                const bool selCell = cells ? s->sel->isSelected(row, k) : selRow;
                if (cells && selCell) dl.rect(clipCol(g, frozen, {x, y, w, rh}), acc.withAlpha(acc.a * 0.55f));
                Rect cell{x + 6.f, y, w - 12.f, rh};
                Color col = k ? dim : text;
                TextAlignment align = TextAlignment::Start;
                dl.pushClip(clip);
                if (s->tree && k == 0) { // indentation + expander
                    const f32 ax = x + 4.f + f32(ref.depth) * kIndent, my = y + rh * 0.5f;
                    if (ref.kids) {
                        if (ref.node != kNoNode) { dl.line({ax + 2.f, my - 2.f}, {ax + 6.f, my + 2.f}, text, 1.5f); dl.line({ax + 6.f, my + 2.f}, {ax + 10.f, my - 2.f}, text, 1.5f); }
                        else { dl.line({ax + 4.f, my - 4.f}, {ax + 8.f, my}, text, 1.5f); dl.line({ax + 8.f, my}, {ax + 4.f, my + 4.f}, text, 1.5f); }
                    }
                    cell.x = ax + 14.f;
                    cell.w = x + w - 6.f - cell.x;
                }
                if (m.role) { // typed roles (models without the callback skip all of this)
                    ItemValue v;
                    if (m.role(m.user, ref.id, k, ItemRole::Background, v, s->scratch2) && v.color.a > 0.f && !selCell)
                        dl.rect({x, y, w, rh}, pc.tint(v.color));
                    v = {};
                    if (m.role(m.user, ref.id, k, ItemRole::CheckState, v, s->scratch2)) {
                        const Rect box{cell.x, y + (rh - 12.f) * 0.5f, 12.f, 12.f};
                        dl.roundedRect(box, v.integer ? acc : Color{}, {2.f, 2.f, 2.f, 2.f}, text.withAlpha(text.a * 0.7f), 1.f);
                        if (v.integer == 1) {
                            dl.line({box.x + 2.5f, box.y + 6.f}, {box.x + 5.f, box.y + 9.f}, pc.tint(themed(pc, tok::on_accent, 0xFFFFFF)), 1.5f);
                            dl.line({box.x + 5.f, box.y + 9.f}, {box.x + 9.5f, box.y + 3.f}, pc.tint(themed(pc, tok::on_accent, 0xFFFFFF)), 1.5f);
                        } else if (v.integer == 2) {
                            dl.line({box.x + 3.f, box.y + 6.f}, {box.x + 9.f, box.y + 6.f}, pc.tint(themed(pc, tok::on_accent, 0xFFFFFF)), 1.5f);
                        }
                        cell.x += 18.f;
                        cell.w -= 18.f;
                    }
                    v = {};
                    if (m.role(m.user, ref.id, k, ItemRole::Decoration, v, s->scratch2)) {
                        const Rect box{cell.x, y + (rh - 12.f) * 0.5f, 12.f, 12.f};
                        if (s->iconFn) s->iconFn(s->iconUser, pc, dl, box, v.integer, v.color);
                        else if (v.color.a > 0.f) dl.rect(box, pc.tint(v.color), 3.f);
                        cell.x += 18.f;
                        cell.w -= 18.f;
                    }
                    v = {};
                    if (m.role(m.user, ref.id, k, ItemRole::Foreground, v, s->scratch2) && v.color.a > 0.f) col = pc.tint(v.color);
                    v = {};
                    if (m.role(m.user, ref.id, k, ItemRole::TextAlign, v, s->scratch2)) align = TextAlignment(v.integer);
                }
                if (m.flags && !(m.flags(m.user, ref.id, k) & kItemEnabled)) col = col.withAlpha(col.a * 0.45f);
                const std::string_view d = cellText(*s, ref, row, k);
                if (const CellDelegate* dg = s->delegate[k]; dg && dg->paint) dg->paint(dg->user, pc, dl, cell, row, k, d, selCell);
                else ui.paintString(dl, d, cell, col, fs, align, pc.node.index);
                dl.popClip();
                if (cells && i64(row) == cur && k == s->curCol && (pc.states & kStateFocus))
                    dl.roundedRect(clipCol(g, frozen, {x, y, w, rh}), {}, {}, acc, 1.f);
                return true;
            });
            if (!cells && i64(row) == cur && (pc.states & kStateFocus)) dl.roundedRect({c.x, y, c.w, rh}, {}, {}, acc, 1.f); // current row
        }
        if (g.hbar) dl.popClip();
        Rect tr, th;
        if (vbarGeom(g, *s, tr, th)) {
            dl.rect(tr, pc.tint(themed(pc, tok::view_scroll_track, 0x000000, 0.15f)), kBar * 0.5f);
            dl.rect(th, pc.tint(s->dragScroll == 1 ? themedA(pc, tok::scroll_thumb_active, 0xB4B4C0, 0.85f) : themedA(pc, tok::scroll_thumb, 0x8A8A96, 0.85f)), kBar * 0.5f);
        }
        if (hbarGeom(g, *s, tr, th)) {
            dl.rect(tr, pc.tint(themed(pc, tok::view_scroll_track, 0x000000, 0.15f)), kBar * 0.5f);
            dl.rect(th, pc.tint(s->dragScroll == 2 ? themedA(pc, tok::scroll_thumb_active, 0xB4B4C0, 0.85f) : themedA(pc, tok::scroll_thumb, 0x8A8A96, 0.85f)), kBar * 0.5f);
        }
        if (pc.states & kStateFocus) dl.roundedRect(pc.rect, {}, {}, acc.withAlpha(acc.a * 0.6f), 1.f);
        dl.popClip();
    }
    static void sortClick(UiContext& ui, NodeHandle n, ViewState& s, u32 k) {
        s.asc = s.sortCol == i32(k) ? !s.asc : true;
        s.sortCol = i32(k);
        ui.emit(n, SignalKind::Toggled, {.value = f32(k), .flag = s.asc, .text = "sort"});
        ui.invalidate(n);
    }
    /// Keys and focus of the open editor, seen on their way down to it.
    static bool editorEvent(EventContext& ec, const Event& e, ViewState& s) {
        UiContext& ui = ec.ui;
        const NodeHandle n = ec.node;
        if (!inSubtree(ui, ec.target, s.editor)) return false;
        if (e.type == EventType::FocusOut) {
            if (s.closing) return false;
            // focus that moved into a popup of the editor (a combo list) keeps the edit open
            const u32 layer = ui.registry().find("layer");
            for (NodeHandle f = ui.focused(); ui.alive(f); f = ui.parent(f))
                if (f == s.editor || ui.classOf(f) == layer) return false;
            commitEditor(ui, n, s);
            return true;
        }
        if (e.type != EventType::KeyDown) return false;
        switch (e.key) {
        case Key::Enter: commitEditor(ui, n, s); return true;
        case Key::Escape: closeEditor(ui, n, s); return true;
        case Key::Tab: {
            u32 row = s.editRow, col = s.editCol;
            commitEditor(ui, n, s);
            if (nextEditable(s, row, col, (e.mods & kModShift) != 0)) {
                setCurrent(ui, n, s, row, col, 0, Via::Key);
                scrollToCol(ui, n, s, col);
                beginEdit(ui, n, s, row, col);
            }
            return true;
        }
        default: return false;
        }
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        UiContext& ui = ec.ui;
        ViewState* s = stateOf(ui, ec.node);
        if (!s) return false;
        if (ec.phase == Phase::Capture) return s->editor.valid() && editorEvent(ec, e, *s);
        if (ec.phase != Phase::Target || !s->model) return false;
        const NodeHandle n = ec.node;
        switch (e.type) {
        case EventType::PointerDown: {
            if (s->editor.valid()) commitEditor(ui, n, *s);
            hoverCell(ui, n, *s, -1, 0);
            const Geo g = geo(ui, n, *s);
            const Rect c = g.c;
            if (e.button != MouseButton::Left) { // a right / middle press makes the cell current (keeping a selection it is in)
                if (e.pos.y >= g.bodyTop() && e.pos.y < g.bodyBottom()) {
                    const i64 row = i64(s->first) + i64((e.pos.y - g.bodyTop()) / g.rh);
                    const ColHit h = colAt(*s, g, e.pos.x);
                    if (row < i64(g.rows) && h.col >= 0) {
                        const bool selected = s->sel ? s->sel->isSelected(u32(row), u32(h.col)) : i64(ui.value(n)) == row;
                        if (!selected) setCurrent(ui, n, *s, row, u32(h.col), 0, Via::Pointer);
                        else if (s->sel) s->sel->setCurrent(row, u32(h.col));
                        if (e.button == MouseButton::Right && s->menuFn) {
                            ec.contextMenuHandled = true; // the per-cell hook answers; a setContextMenu menu stays closed
                            s->menuFn(s->menuUser, ui, n, u32(row), itemOf(*s, u32(row)), u32(h.col), e.pos);
                        }
                    }
                }
                return true;
            }
            s->resizing = -1;
            s->dragScroll = 0;
            s->pressCol = s->moving = -1;
            s->pressRow = -1;
            s->pressConsumed = false;
            Rect tr, th;
            if (vbarGeom(g, *s, tr, th) && tr.contains(e.pos)) {
                s->dragScroll = 1;
                s->grab = th.contains(e.pos) ? e.pos.y - th.y : th.h * 0.5f;
                Event mv = e;
                mv.type = EventType::PointerMove;
                return onEvent(ec, mv);
            }
            if (hbarGeom(g, *s, tr, th) && tr.contains(e.pos)) {
                s->dragScroll = 2;
                s->grab = th.contains(e.pos) ? e.pos.x - th.x : th.w * 0.5f;
                Event mv = e;
                mv.type = EventType::PointerMove;
                return onEvent(ec, mv);
            }
            if (e.pos.y < c.y + g.hh) {
                eachCol(*s, g, [&](u32 k, u32 v, f32 x, f32 w, bool) {
                    const f32 edge = x + w;
                    if (std::abs(e.pos.x - edge) <= 4.f) {
                        if (doubleClick(*s, e, -2, k)) { resizeViewColumnToContents(ui, n, k); return false; }
                        s->resizing = i32(k);
                        s->grab = edge - s->colW[k];
                        return false;
                    }
                    if (e.pos.x < edge) {
                        if (s->movable) { s->pressCol = i32(v); s->pressX = e.pos.x; } // sorts on release unless dragged
                        else sortClick(ui, n, *s, k);
                        return false;
                    }
                    return true;
                });
                return true;
            }
            if (e.pos.y >= g.bodyBottom()) return true;
            const i64 row = i64(s->first) + i64((e.pos.y - c.y - g.hh) / g.rh);
            if (row >= i64(g.rows)) return true;
            const ColHit h = colAt(*s, g, e.pos.x);
            const u32 col = h.col >= 0 ? u32(h.col) : s->curCol;
            if (s->tree && h.col == 0) { // expander
                const RowRef ref = locate(*s, u32(row));
                const f32 ax = h.x + 4.f + f32(ref.depth) * kIndent;
                if (ref.kids && e.pos.x >= ax && e.pos.x < ax + 14.f) {
                    if (ref.node != kNoNode) collapseRow(ui, n, *s, u32(row));
                    else expandRow(ui, n, *s, u32(row));
                    return true;
                }
            }
            bool checked = false;
            if (h.col >= 0 && s->model->role && s->model->flags) { // check box at the start of the cell
                f32 bx = h.x + 6.f;
                if (s->tree && h.col == 0) bx = h.x + 4.f + f32(locate(*s, u32(row)).depth) * kIndent + 14.f;
                if (e.pos.x >= bx - 2.f && e.pos.x < bx + 14.f) checked = toggleCheck(ui, n, *s, u32(row), col);
            }
            setCurrent(ui, n, *s, row, col, e.mods, Via::Pointer);
            s->pressRow = row;
            s->pressConsumed = checked;
            if (!checked && h.col >= 0 && doubleClick(*s, e, row, col)) {
                // precedence of a double click: an editable cell opens its editor, a tree node with children folds /
                // unfolds, anything else activates the row (Submitted, on the DoubleClick event of the release)
                if ((s->triggers & kEditDoubleClick) && beginEdit(ui, n, *s, u32(row), col)) { s->pressConsumed = true; return true; }
                if (s->tree) {
                    if (locate(*s, u32(row)).node != kNoNode) s->pressConsumed = collapseRow(ui, n, *s, u32(row));
                    else s->pressConsumed = expandRow(ui, n, *s, u32(row));
                }
            }
            return true;
        }
        case EventType::PointerMove: {
            if (ui.pressed() != n && !s->dragScroll) { // hover: tooltip for the cell under the pointer
                const Geo g = geo(ui, n, *s);
                i64 row = -1;
                u32 col = 0;
                if (e.pos.y >= g.bodyTop() && e.pos.y < g.bodyBottom()) {
                    const ColHit h = colAt(*s, g, e.pos.x);
                    const i64 r = i64(s->first) + i64((e.pos.y - g.bodyTop()) / g.rh);
                    if (h.col >= 0 && r < i64(g.rows)) { row = r; col = u32(h.col); }
                }
                hoverCell(ui, n, *s, row, col);
                return false;
            }
            if (s->resizing >= 0) {
                s->colW[s->resizing] = std::max(24.f, std::round(e.pos.x - s->grab));
                ui.invalidate(n);
                syncEditor(ui, n, *s);
            } else if (s->dragScroll == 1) {
                const Geo g = geo(ui, n, *s);
                Rect tr, th;
                if (!vbarGeom(g, *s, tr, th)) return true;
                const f32 f = std::clamp((e.pos.y - tr.y - s->grab) / std::max(1.f, tr.h - th.h), 0.f, 1.f);
                setFirst(ui, n, *s, i64(std::lround(f * f32(g.rows - g.vis))));
                ui.invalidate(n);
            } else if (s->dragScroll == 2) {
                const Geo g = geo(ui, n, *s);
                Rect tr, th;
                if (!hbarGeom(g, *s, tr, th)) return true;
                const f32 f = std::clamp((e.pos.x - tr.x - s->grab) / std::max(1.f, tr.w - th.w), 0.f, 1.f);
                setScrollX(ui, n, *s, f * g.maxX);
                ui.invalidate(n);
            } else if (s->pressCol >= 0) {
                if (s->moving < 0 && std::abs(e.pos.x - s->pressX) > 6.f) s->moving = s->pressCol;
                if (s->moving >= 0) { s->moveX = e.pos.x; ui.invalidate(n); }
            }
            return true;
        }
        case EventType::PointerUp:
            if (s->dragScroll) ui.invalidate(n);
            if (s->moving >= 0 && !e.cancel) { // drop: before the first column whose middle is right of the pointer
                const Geo g = geo(ui, n, *s);
                u32 to = 0, last = 0;
                eachCol(*s, g, [&](u32, u32 v, f32 x, f32 w, bool) { last = v; if (e.pos.x > x + w * 0.5f) to = v + 1; return true; });
                const u32 from = u32(s->moving);
                if (to > from) --to; // positions are counted with the dragged column removed
                moveViewColumn(ui, n, from, std::min(to, last));
            } else if (s->pressCol >= 0 && !e.cancel && u32(s->pressCol) < cols(*s)) {
                sortClick(ui, n, *s, s->order[s->pressCol]);
            }
            if (s->moving >= 0) ui.invalidate(n);
            s->resizing = -1;
            s->dragScroll = 0;
            s->pressCol = s->moving = -1;
            return false;
        case EventType::PointerLeave: hoverCell(ui, n, *s, -1, 0); return false;
        case EventType::DoubleClick: { // a double click on a row activates it, like Enter - unless the second press
            // already opened an editor, folded a tree node or toggled a check box (see PointerDown)
            if (e.button != MouseButton::Left || s->pressRow < 0 || s->pressConsumed || s->editor.valid()) return true;
            const i64 cur = currentRow(ui, *s);
            if (cur >= 0 && cur == s->pressRow && cur == i64(tableRowAt(ui, n, e.pos)))
                ui.emit(n, SignalKind::Submitted, {.value = f32(cur)});
            return true;
        }
        case EventType::Wheel:
            hoverCell(ui, n, *s, -1, 0);
            if ((e.mods & kModShift) || (e.wheel.x != 0.f && e.wheel.y == 0.f)) {
                setScrollX(ui, n, *s, s->scrollX - (e.wheel.x != 0.f ? e.wheel.x : e.wheel.y) * 40.f);
                return true;
            }
            if (e.wheel.x != 0.f) setScrollX(ui, n, *s, s->scrollX - e.wheel.x * 40.f); // a diagonal pan (touch, trackpad)
            setFirst(ui, n, *s, i64(s->first) - i64(std::lround(e.wheel.y * 3.f)));
            return true;
        case EventType::Text: {
            const i64 cur = currentRow(ui, *s);
            if ((s->triggers & kEditAnyKey) && cur >= 0 && !e.text.empty() && static_cast<unsigned char>(e.text[0]) >= 0x20 &&
                e.text != " " && cellEditable(*s, u32(cur), s->curCol)) {
                scrollToRow(ui, n, *s, cur);
                if (beginEdit(ui, n, *s, u32(cur), s->curCol, e.text)) return true;
            }
            if (e.text == " " && s->typed.empty()) return false; // Space toggles (KeyDown), it does not start a search
            return typeAhead(ui, n, *s, e.text);
        }
        case EventType::KeyDown: {
            const Geo g = geo(ui, n, *s);
            const i64 cur = currentRow(ui, *s), vis = g.vis;
            const u32 col = s->curCol;
            const u8 mods = e.mods;
            auto go = [&](i64 row) { setCurrent(ui, n, *s, row, col, mods, Via::Key); return true; };
            switch (e.key) {
            case Key::Down: return go(cur + 1);
            case Key::Up: return go(cur < 0 ? 0 : cur - 1);
            case Key::PageDown: return go(cur + vis);
            case Key::PageUp: return go(cur - vis);
            case Key::Home: return go(0);
            case Key::End: return go(i64(g.rows) - 1);
            case Key::Enter:
                if (cur >= 0) ui.emit(n, SignalKind::Submitted, {.value = f32(cur)});
                return true;
            case Key::F2:
                if (cur >= 0 && (s->triggers & kEditKeyF2)) { scrollToRow(ui, n, *s, cur); beginEdit(ui, n, *s, u32(cur), col); }
                return true;
            case Key::Space:
                if (cur < 0) return false;
                if (toggleCheck(ui, n, *s, u32(cur), col)) return true;
                if (s->sel && (s->mode == SelectionMode::Multi || s->mode == SelectionMode::Extended)) {
                    s->sel->toggle(u32(cur), s->unit == SelectionUnit::Cells ? col : kAllColumns);
                    return true;
                }
                return false;
            case Key::A:
                if (!(mods & kModCtrl) || !s->sel || (s->mode != SelectionMode::Multi && s->mode != SelectionMode::Extended)) return false;
                s->sel->selectAll(g.rows);
                return true;
            case Key::Right:
            case Key::Left: {
                const bool right = e.key == Key::Right;
                if (s->tree && cur >= 0) {
                    const RowRef ref = locate(*s, u32(cur));
                    if (right) {
                        if (ref.kids && ref.node == kNoNode) expandRow(ui, n, *s, u32(cur));
                        else if (ref.node != kNoNode && s->rows->nodes[std::size_t(ref.node)].count) go(cur + 1); // first child
                    } else {
                        if (ref.node != kNoNode) collapseRow(ui, n, *s, u32(cur));
                        else if (ref.parent != 0) go(rowOfNode(*s->rows, ref.parent));
                    }
                    return true;
                }
                if (s->sel && s->unit == SelectionUnit::Cells && cur >= 0) { // next / previous shown column
                    const u32 nc = cols(*s);
                    i64 v = viewColumnVisual(ui, n, col);
                    for (v += right ? 1 : -1; v >= 0 && v < i64(nc); v += right ? 1 : -1)
                        if (colWidth(*s, s->order[v]) > 0.f) { setCurrent(ui, n, *s, cur, s->order[v], mods, Via::Key); break; }
                    return true;
                }
                if (!g.hbar) return false;
                setScrollX(ui, n, *s, s->scrollX + (right ? 40.f : -40.f));
                return true;
            }
            default: return false;
            }
        }
        default: return false;
        }
    }
};

NodeHandle createView(UiContext& ui, std::string_view cls, NodeHandle parent, const ItemModel* model, std::string_view id, bool tree) {
    registerTierB(ui);
    const NodeHandle n = make(ui, cls, parent, id);
    if (!ui.alive(n)) return n;
    ViewState& s = newState(ui, n);
    s.model = model;
    s.tree = tree;
    const u32 nc = cols(s);
    for (u32 c = 0; c < nc; ++c) s.colW[c] = c + 1 == nc ? 160.f : 120.f;
    ui.setRange(n, -1.f, 1e9f, 1.f);
    ui.setValue(n, -1.f);
    if (tree) treeReset(s);
    if (model && model->notifier) model->notifier->subscribe(&modelChanged, &s, s.serial);
    return n;
}

} // namespace

namespace detail {
void registerModelView(UiContext& ui) {
    WidgetRegistry& r = ui.registry();
    const u32 id = r.registerWidget<ItemView>();
    WidgetClass tree = r.get(id); // the same function table under a second name, so themes can tell them apart
    tree.name = "treemodelview";
    tree.nameHash = 0;
    r.add(std::move(tree));
    a11yModelView(r);
}
} // namespace detail

// ---- table / list API (the names predate the shared view) ------------------------------------------------------------------
NodeHandle tableView(UiContext& ui, NodeHandle parent, const ItemModel* model, std::string_view id) {
    return createView(ui, "tableview", parent, model, id, false);
}
NodeHandle modelListView(UiContext& ui, NodeHandle parent, const ItemModel* model, std::string_view id) {
    const NodeHandle n = tableView(ui, parent, model, id);
    if (ViewState* s = stateOf(ui, n)) {
        s->header = false;
        s->list = true;
        s->colW[0] = 4096.f; // clipped to the view
        for (u32 c = 1; c < kMaxCols; ++c) s->colW[c] = 0.f;
    }
    return n;
}
NodeHandle treeModelView(UiContext& ui, NodeHandle parent, const ItemModel* model, std::string_view id) {
    if (!model || !model->childCount || !model->childAt || !model->itemData) return {};
    const NodeHandle n = createView(ui, "treemodelview", parent, model, id, true);
    if (ViewState* s = stateOf(ui, n)) {
        s->header = model->columns > 1 && model->header;
        if (model->columns <= 1) { s->list = true; s->colW[0] = 4096.f; } // one column fills the view
        else s->colW[0] = 200.f;
    }
    return n;
}
void tableModelChanged(UiContext& ui, NodeHandle table) {
    ViewState* s = stateOf(ui, table);
    if (!s) return;
    if (s->tree) { // without notifications nothing says what moved: start over from the collapsed tree
        if (s->editor.valid()) closeEditor(ui, table, *s);
        treeReset(*s);
        if (s->sel) s->sel->clear();
    }
    setFirst(ui, table, *s, s->first);
    const u32 rows = rowCount(*s);
    if (s->sel) {
        if (s->sel->currentRow() >= i64(rows)) s->sel->setCurrent(i64(rows) - 1, s->sel->currentColumn());
        if (rows) s->sel->deselect({rows, 0xFFFFFFFFu}); else s->sel->clear();
    } else if (ui.value(table) >= f32(rows)) {
        ui.setValue(table, -1.f, true);
    }
    syncEditor(ui, table, *s);
    ui.invalidate(table);
}
// Row geometry for the drag-and-drop glue of widgets_interaction.cpp (interaction.hpp). Rows are view rows: the
// model's rows in a table / list, the visible rows in a tree.
const ItemModel* tableModel(const UiContext& ui, NodeHandle table) {
    const ViewState* s = stateOf(ui, table);
    return s ? s->model : nullptr;
}
Rect tableRowRect(const UiContext& ui, NodeHandle table, u32 row) {
    const ViewState* s = stateOf(ui, table);
    if (!s) return {};
    const Geo g = geo(ui, table, *s);
    // the full width of the view whatever the horizontal scroll (a row is selected and dropped on as a whole); may
    // lie outside the body when the row is scrolled out
    return {g.c.x, g.bodyTop() + (f32(row) - f32(s->first)) * g.rh, g.c.w, g.rh};
}
i32 tableRowAt(const UiContext& ui, NodeHandle table, Vec2 p) {
    const ViewState* s = stateOf(ui, table);
    if (!s) return -1;
    const Geo g = geo(ui, table, *s);
    if (!g.c.contains(p) || p.y < g.bodyTop() || p.y >= g.bodyBottom()) return -1; // header, horizontal bar
    Rect tr, th;
    if (vbarGeom(g, *s, tr, th) && tr.contains(p)) return -1;
    const i64 row = i64(s->first) + i64((p.y - g.bodyTop()) / g.rh);
    return row < i64(g.rows) ? i32(row) : -1;
}
u32 tableFirstVisible(const UiContext& ui, NodeHandle table) { const ViewState* s = stateOf(ui, table); return s ? s->first : 0u; }
f32 tableColumnWidth(const UiContext& ui, NodeHandle table, u32 col) {
    const ViewState* s = stateOf(ui, table);
    return s && col < kMaxCols ? s->colW[col] : 0.f;
}
void setTableColumnWidth(UiContext& ui, NodeHandle table, u32 col, f32 px) {
    if (ViewState* s = stateOf(ui, table); s && col < kMaxCols) {
        s->colW[col] = std::max(24.f, px);
        ui.invalidate(table);
        clampView(ui, table, *s);
    }
}
void setTableDelegate(UiContext& ui, NodeHandle table, u32 col, const CellDelegate* delegate) {
    if (ViewState* s = stateOf(ui, table); s && col < kMaxCols) { s->delegate[col] = delegate; ui.invalidate(table); }
}

// ---- selection API ------------------------------------------------------------------------------------------------------
void setViewSelectionMode(UiContext& ui, NodeHandle view, SelectionMode mode, SelectionUnit unit) {
    ViewState* s = stateOf(ui, view);
    if (!s) { detail::gridSetSelection(ui, view, nullptr, true, mode); return; }
    s->mode = mode;
    s->unit = s->tree || s->list ? SelectionUnit::Rows : unit;
    if (!s->sel && (mode != SelectionMode::Single || s->unit == SelectionUnit::Cells)) attachSelection(ui, view, *s, nullptr);
    if (s->sel && mode == SelectionMode::None) s->sel->clear();
    ui.invalidate(view);
}
void setViewSelectionModel(UiContext& ui, NodeHandle view, SelectionModel* sel) {
    ViewState* s = stateOf(ui, view);
    if (!s) { detail::gridSetSelection(ui, view, sel, false, SelectionMode::Single); return; }
    if (s->sel == sel && sel) return;
    attachSelection(ui, view, *s, sel);
    ui.setValue(view, f32(s->sel->currentRow()), true);
}
SelectionModel* viewSelectionModel(UiContext& ui, NodeHandle view) {
    if (ViewState* s = stateOf(ui, view)) return s->sel;
    return detail::gridSelection(ui, view);
}
i64 viewCurrentRow(const UiContext& ui, NodeHandle view) {
    const ViewState* s = stateOf(ui, view);
    return s ? currentRow(ui, *s) : i64(ui.value(view));
}
u32 viewCurrentColumn(const UiContext& ui, NodeHandle view) { const ViewState* s = stateOf(ui, view); return s ? s->curCol : 0u; }
void setViewCurrent(UiContext& ui, NodeHandle view, u32 row, u32 col) {
    if (ViewState* s = stateOf(ui, view)) setCurrent(ui, view, *s, row, std::min(col, cols(*s) ? cols(*s) - 1 : 0u), 0, Via::Pointer);
}

// ---- columns / rows API ---------------------------------------------------------------------------------------------------
void setViewColumnsMovable(UiContext& ui, NodeHandle view, bool on) { if (ViewState* s = stateOf(ui, view)) s->movable = on; }
void moveViewColumn(UiContext& ui, NodeHandle view, u32 from, u32 to) {
    ViewState* s = stateOf(ui, view);
    if (!s) return;
    const u32 nc = cols(*s);
    if (from >= nc || to >= nc || from == to) return;
    const u8 k = s->order[from];
    if (from < to) std::move(s->order + from + 1, s->order + to + 1, s->order + from);
    else std::move_backward(s->order + to, s->order + from, s->order + from + 1);
    s->order[to] = k;
    ui.invalidate(view);
    syncEditor(ui, view, *s);
}
u32 viewColumnAt(const UiContext& ui, NodeHandle view, u32 visual) {
    const ViewState* s = stateOf(ui, view);
    return s && visual < kMaxCols ? s->order[visual] : visual;
}
u32 viewColumnVisual(const UiContext& ui, NodeHandle view, u32 col) {
    if (const ViewState* s = stateOf(ui, view))
        for (u32 v = 0; v < kMaxCols; ++v)
            if (s->order[v] == col) return v;
    return col;
}
void setViewColumnHidden(UiContext& ui, NodeHandle view, u32 col, bool hidden) {
    ViewState* s = stateOf(ui, view);
    if (!s || col >= kMaxCols) return;
    s->hiddenMask = hidden ? (s->hiddenMask | (1u << col)) : (s->hiddenMask & ~(1u << col));
    ui.invalidate(view);
    clampView(ui, view, *s);
}
bool viewColumnHidden(const UiContext& ui, NodeHandle view, u32 col) {
    const ViewState* s = stateOf(ui, view);
    return s && col < kMaxCols && (s->hiddenMask >> col & 1u);
}
void resizeViewColumnToContents(UiContext& ui, NodeHandle view, u32 col) {
    ViewState* s = stateOf(ui, view);
    if (!s || !s->model || col >= cols(*s) || s->list) return;
    const Geo g = geo(ui, view, *s);
    const f32 fs = ui.computed(view).fontSize;
    f32 w = s->header && s->model->header ? ui.measureString(s->model->header(s->model->user, col), fs) + (s->sortCol == i32(col) ? 20.f : 0.f) : 0.f;
    for (u32 i = 0; i < g.vis + 1 && s->first + i < g.rows; ++i) {
        const u32 row = s->first + i;
        const RowRef ref = s->tree ? locate(*s, row) : RowRef{.id = row};
        f32 extra = s->tree && col == 0 ? f32(ref.depth) * kIndent + 12.f : 0.f;
        if (s->model->role) {
            ItemValue v;
            if (s->model->role(s->model->user, ref.id, col, ItemRole::CheckState, v, s->scratch2)) extra += 18.f;
            if (s->model->role(s->model->user, ref.id, col, ItemRole::Decoration, v, s->scratch2)) extra += 18.f;
        }
        w = std::max(w, ui.measureString(cellText(*s, ref, row, col), fs) + extra);
    }
    s->colW[col] = std::max(24.f, std::ceil(w) + 14.f); // 6 px padding each side + the 2 px cell gap
    ui.invalidate(view);
    clampView(ui, view, *s);
}
void setViewFrozenColumns(UiContext& ui, NodeHandle view, u32 n) {
    if (ViewState* s = stateOf(ui, view)) { s->frozen = std::min(n, kMaxCols); ui.invalidate(view); syncEditor(ui, view, *s); }
}
void setViewAlternatingRows(UiContext& ui, NodeHandle view, bool on) {
    if (ViewState* s = stateOf(ui, view)) { s->alternating = on; ui.invalidate(view); }
}
void setViewRowHeight(UiContext& ui, NodeHandle view, f32 px) {
    if (ViewState* s = stateOf(ui, view)) { s->rowH = px > 0.f ? std::max(8.f, std::round(px)) : 0.f; ui.invalidate(view); clampView(ui, view, *s); }
}
void setViewHeaderVisible(UiContext& ui, NodeHandle view, bool on) {
    if (ViewState* s = stateOf(ui, view)) { s->header = on; ui.invalidate(view); clampView(ui, view, *s); }
}
f32 viewScrollX(const UiContext& ui, NodeHandle view) { const ViewState* s = stateOf(ui, view); return s ? s->scrollX : 0.f; }
void setViewScrollX(UiContext& ui, NodeHandle view, f32 px) { if (ViewState* s = stateOf(ui, view)) setScrollX(ui, view, *s, px); }
void setViewSortIndicator(UiContext& ui, NodeHandle view, i32 col, bool ascending) {
    if (ViewState* s = stateOf(ui, view)) { s->sortCol = col; s->asc = ascending; ui.invalidate(view); }
}
void setViewFirstVisible(UiContext& ui, NodeHandle view, u32 row) { if (ViewState* s = stateOf(ui, view)) setFirst(ui, view, *s, row); }
void viewScrollToRow(UiContext& ui, NodeHandle view, u32 row) { if (ViewState* s = stateOf(ui, view)) scrollToRow(ui, view, *s, row); }
u32 viewVisibleRows(const UiContext& ui, NodeHandle view) { const ViewState* s = stateOf(ui, view); return s ? geo(ui, view, *s).vis : 0u; }
u32 viewRowCount(const UiContext& ui, NodeHandle view) { const ViewState* s = stateOf(ui, view); return s ? rowCount(*s) : 0u; }
Rect viewCellRect(const UiContext& ui, NodeHandle view, u32 row, u32 col) {
    const ViewState* s = stateOf(ui, view);
    return s && s->model ? cellRect(ui, view, *s, row, col) : Rect{};
}
void setViewToolTips(UiContext& ui, NodeHandle view, bool on, f32 delay) {
    if (ViewState* s = stateOf(ui, view)) { s->tips = on; s->tipDelay = std::max(0.f, delay); if (!on) hideTip(ui, *s); }
}
NodeHandle viewToolTip(const UiContext& ui, NodeHandle view) {
    const ViewState* s = stateOf(ui, view);
    return s && ui.alive(s->tip) ? s->tip : NodeHandle{};
}
void setViewContextMenu(UiContext& ui, NodeHandle view, ViewContextMenuFn fn, void* user) {
    if (ViewState* s = stateOf(ui, view)) { s->menuFn = fn; s->menuUser = user; }
}
void setViewIconPainter(UiContext& ui, NodeHandle view, ViewIconPainter fn, void* user) {
    if (ViewState* s = stateOf(ui, view)) { s->iconFn = fn; s->iconUser = user; ui.invalidate(view); }
}
void setViewTypeAheadColumn(UiContext& ui, NodeHandle view, i32 col) { if (ViewState* s = stateOf(ui, view)) s->typeCol = col; }

// ---- editing API -------------------------------------------------------------------------------------------------------
void setViewEditTriggers(UiContext& ui, NodeHandle view, u32 triggers) { if (ViewState* s = stateOf(ui, view)) s->triggers = triggers; }
bool viewEdit(UiContext& ui, NodeHandle view, u32 row, u32 col) {
    ViewState* s = stateOf(ui, view);
    return s && s->model && beginEdit(ui, view, *s, row, col);
}
NodeHandle viewEditor(const UiContext& ui, NodeHandle view) {
    const ViewState* s = stateOf(ui, view);
    return s && ui.alive(s->editor) ? s->editor : NodeHandle{};
}
bool viewCommitEdit(UiContext& ui, NodeHandle view) {
    ViewState* s = stateOf(ui, view);
    return s && commitEditor(ui, view, *s);
}
void viewCancelEdit(UiContext& ui, NodeHandle view) { if (ViewState* s = stateOf(ui, view)) closeEditor(ui, view, *s); }

CellDelegate lineEditDelegate() {
    CellDelegate d;
    d.createEditor = [](const void*, UiContext& ui, NodeHandle view, ItemId, u32) { return lineEdit(ui, view, {}); };
    d.setEditorData = [](const void*, UiContext& ui, NodeHandle ed, const ItemValue& v) {
        ui.setText(ed, v.text);
        ui.editBuffer(ed).setText(v.text);
        ui.editBuffer(ed).selectAll();
    };
    d.commit = [](const void*, UiContext& ui, NodeHandle ed, ItemValue& out, std::string& scratch) {
        scratch = ui.editBuffer(ed).text();
        out.text = scratch;
        parseNum(out.text, out.number);
        return true;
    };
    return d;
}
CellDelegate spinBoxDelegate(const SpinEditorSpec* spec) {
    CellDelegate d;
    d.user = spec;
    d.createEditor = [](const void* u, UiContext& ui, NodeHandle view, ItemId, u32) {
        const auto* sp = static_cast<const SpinEditorSpec*>(u);
        return sp ? spinBox(ui, view, sp->lo, sp->hi, sp->lo, sp->step) : spinBox(ui, view, 0.f, 100.f, 0.f);
    };
    d.setEditorData = [](const void*, UiContext& ui, NodeHandle ed, const ItemValue& v) { ui.setValue(ed, f32(v.number)); };
    d.commit = [](const void*, UiContext& ui, NodeHandle ed, ItemValue& out, std::string& scratch) {
        out.number = double(ui.value(ed));
        const f32 st = ui.rangeStep(ed);
        char buf[48];
        const int len = std::snprintf(buf, sizeof buf, "%.*f", st > 0.f && st < 1.f ? (st < 0.1f ? 2 : 1) : 0, out.number);
        scratch.assign(buf, std::size_t(std::max(0, len)));
        out.text = scratch;
        return true;
    };
    return d;
}
CellDelegate comboBoxDelegate(const std::string* items) {
    CellDelegate d;
    d.user = items;
    d.createEditor = [](const void* u, UiContext& ui, NodeHandle view, ItemId, u32) {
        const auto* it = static_cast<const std::string*>(u);
        return comboBox(ui, view, it ? std::string_view(*it) : std::string_view{}, 0);
    };
    d.setEditorData = [](const void*, UiContext& ui, NodeHandle ed, const ItemValue& v) {
        const std::string want(v.text); // comboText views the node text: compare against a copy
        const u32 n = u32(ui.rangeMax(ed)) + 1;
        for (u32 i = 0; i < n; ++i) {
            ui.setValue(ed, f32(i));
            if (comboText(ui, ed) == want) return;
        }
        ui.setValue(ed, 0.f);
    };
    d.commit = [](const void*, UiContext& ui, NodeHandle ed, ItemValue& out, std::string& scratch) {
        scratch.assign(comboText(ui, ed));
        out.text = scratch;
        out.number = double(ui.value(ed));
        out.integer = i32(ui.value(ed));
        return true;
    };
    return d;
}

// ---- tree API ------------------------------------------------------------------------------------------------------------
bool setTreeRowExpanded(UiContext& ui, NodeHandle tree, u32 row, bool open) {
    ViewState* s = stateOf(ui, tree);
    if (!s || !s->tree || row >= rowCount(*s)) return false;
    return open ? expandRow(ui, tree, *s, row) : collapseRow(ui, tree, *s, row);
}
bool treeRowExpanded(const UiContext& ui, NodeHandle tree, u32 row) {
    const ViewState* s = stateOf(ui, tree);
    return s && s->tree && row < rowCount(*s) && locate(*s, row).node != kNoNode;
}
bool treeItemExpanded(const UiContext& ui, NodeHandle tree, ItemId item) {
    const ViewState* s = stateOf(ui, tree);
    return s && s->tree && item != kRootItem && s->rows->byId.contains(item);
}
i64 treeRowOf(const UiContext& ui, NodeHandle tree, ItemId item, ItemId parent) {
    const ViewState* s = stateOf(ui, tree);
    if (!s || !s->tree) return -1;
    const TreeRows& t = *s->rows;
    if (const auto own = t.byId.find(item); own != t.byId.end() && item != kRootItem) return rowOfNode(t, own->second);
    const auto it = t.byId.find(parent);
    if (it == t.byId.end()) return -1;
    const u32 n = t.nodes[std::size_t(it->second)].count;
    for (u32 r = 0; r < n; ++r)
        if (s->model->childAt(s->model->user, parent, r) == item) return rowOfChild(t, it->second, r);
    return -1;
}
bool setTreeItemExpanded(UiContext& ui, NodeHandle tree, ItemId item, bool open) {
    ViewState* s = stateOf(ui, tree);
    if (!s || !s->tree || item == kRootItem) return false;
    const TreeRows& t = *s->rows;
    if (const auto it = t.byId.find(item); it != t.byId.end()) return open ? false : collapseRow(ui, tree, *s, u32(rowOfNode(t, it->second)));
    if (!open) return false;
    // not expanded: look for it among the children of the expanded nodes (bounded by what is shown)
    for (std::size_t p = 0; p < t.nodes.size(); ++p) {
        if (!t.nodes[p].used) continue;
        for (u32 r = 0; r < t.nodes[p].count; ++r)
            if (s->model->childAt(s->model->user, t.nodes[p].id, r) == item) return expandRow(ui, tree, *s, u32(rowOfChild(t, i32(p), r)));
    }
    return false;
}
void treeCollapseAll(UiContext& ui, NodeHandle tree) {
    ViewState* s = stateOf(ui, tree);
    if (!s || !s->tree) return;
    if (s->editor.valid()) closeEditor(ui, tree, *s);
    treeReset(*s);
    if (s->sel) s->sel->clear();
    clampView(ui, tree, *s);
    ui.invalidate(tree);
}
ItemId treeItemAt(const UiContext& ui, NodeHandle tree, u32 row) {
    const ViewState* s = stateOf(ui, tree);
    return s && s->tree && row < rowCount(*s) ? locate(*s, row).id : kRootItem;
}
u32 treeRowDepth(const UiContext& ui, NodeHandle tree, u32 row) {
    const ViewState* s = stateOf(ui, tree);
    return s && s->tree && row < rowCount(*s) ? locate(*s, row).depth : 0u;
}
u32 treeExpandedCount(const UiContext& ui, NodeHandle tree) {
    const ViewState* s = stateOf(ui, tree);
    return s && s->tree ? u32(s->rows->byId.size()) - 1u : 0u;
}

} // namespace fuse::ui::widgets

// ---- accessibility (plan §4.14): the item views --------------------------------------------------------------------------
// A table is a header row of column headers plus the VISIBLE rows with their cells; a model list view is a list of
// list items; a tree model view is a tree of tree items (level, expanded / collapsed; with more than one column each
// item carries its cells). Nothing here runs unless a snapshot or an action is asked for.
namespace fuse::ui::widgets {
namespace {
using detail::a11yItem;
// Child ids: row key q -> 1 + q * (kMaxCols + 1), its cell in model column c -> that + 1 + c; the header row and its
// sections sit at the top of the id space (ids stay below 2^31, what the bridge can carry). The key of a flat row is
// the row. A tree row is keyed by its item id, so the id survives expanding / collapsing above it; item ids that do
// not fit (>= kA11yIdKeys) fall back to the visible row in the upper half of the key space (unique, not stable).
constexpr u32 kA11yHeaderRow = 0x7FFF0000u;
constexpr u32 kA11yStride = kMaxCols + 1u;
constexpr u32 kA11yKeys = (kA11yHeaderRow - 1u) / kA11yStride;
constexpr u32 kA11yIdKeys = kA11yKeys / 2u;
constexpr u32 kA11yRowKeys = kA11yKeys - kA11yIdKeys;
constexpr u32 kA11yScanRows = 65536; ///< tree rows searched for an item id that is not in the window
bool a11yKey(const ViewState& s, const RowRef& ref, u32 row, u32& key) {
    if (!s.tree) { key = row; return row < kA11yKeys; }
    key = ref.id < kA11yIdKeys ? u32(ref.id) : kA11yIdKeys + row % kA11yRowKeys;
    return true;
}
i64 a11yRowOf(const UiContext& ui, NodeHandle n, const ViewState& s, u32 key) {
    const u32 rows = rowCount(s);
    if (!s.tree) return key < rows ? i64(key) : -1;
    if (key >= kA11yIdKeys) { // a visible row: the candidate inside the window when there is one
        u32 row = key - kA11yIdKeys;
        while (row < s.first && u64(row) + kA11yRowKeys < rows) row += kA11yRowKeys;
        return row < rows && locate(s, row).id >= kA11yIdKeys ? i64(row) : -1;
    }
    const Geo g = geo(ui, n, s);
    for (u32 i = 0; i <= g.vis && s.first + i < rows; ++i)
        if (locate(s, s.first + i).id == key) return i64(s.first + i);
    for (u32 row = 0; row < std::min(rows, kA11yScanRows); ++row)
        if (locate(s, row).id == key) return i64(row);
    return -1;
}
u32 shownCols(const ViewState& s) {
    u32 shown = 0;
    for (u32 k = 0; k < cols(s); ++k) shown += colWidth(s, k) > 0.f;
    return shown;
}

void viewA11y(const UiContext& ui, NodeHandle n, A11ySnapshotNode& self, std::vector<A11ySnapshotNode>& items) {
    ViewState* sp = stateOf(ui, n);
    if (!sp || !sp->model) return;
    ViewState& s = *sp;
    const ItemModel& m = *s.model;
    const Geo g = geo(ui, n, s);
    const Rect c = g.c;
    const bool tree = s.tree, list = s.list && !tree;
    const bool withCells = !s.list;
    const bool cellUnit = s.sel && s.unit == SelectionUnit::Cells;
    const bool selectable = s.mode != SelectionMode::None;
    const u32 nshown = shownCols(s);
    if (self.role == A11yRole::Table && list) self.role = A11yRole::List;
    const A11yRole rowRole = self.role == A11yRole::Tree || tree ? A11yRole::TreeItem : list ? A11yRole::ListItem : A11yRole::Row;
    self.setSize = g.rows;
    if (!self.disabled) {
        if (g.rows > g.vis) self.actions |= detail::a11yBits({A11yAction::ScrollUp, A11yAction::ScrollDown});
        if (g.hbar && g.maxX > 0.f) self.actions |= detail::a11yBits({A11yAction::ScrollLeft, A11yAction::ScrollRight});
    }
    const u32 itemActions = self.disabled ? 0u : detail::kA11yItemActions;
    if (s.header) {
        const u32 hi = u32(items.size());
        items.push_back(a11yItem(kA11yHeaderRow, A11yRole::Row, {}, {c.x, c.y, c.w, g.hh}));
        u32 pos = 0;
        eachCol(s, g, [&](u32 k, u32, f32 x, f32 w, bool frozen) {
            ++pos;
            const Rect r = clipCol(g, frozen, {x, c.y, w, g.hh});
            if (r.empty()) return true; // scrolled out sideways
            A11ySnapshotNode h = a11yItem(kA11yHeaderRow + 1u + k, A11yRole::ColumnHeader, m.header ? m.header(m.user, k) : std::string_view{}, r,
                                          self.disabled ? 0u : a11yActionBit(A11yAction::Click) | (g.hbar ? a11yActionBit(A11yAction::ScrollIntoView) : 0u));
            h.parent = hi;
            h.posInSet = pos;
            h.setSize = nshown;
            if (s.sortCol == i32(k)) h.valueText = s.asc ? "ascending" : "descending";
            items.push_back(std::move(h));
            return true;
        });
    }
    for (u32 row = s.first; row < g.rows && row < s.first + g.vis; ++row) {
        const RowRef ref = tree ? locate(s, row) : RowRef{.id = row};
        u32 key;
        if (!a11yKey(s, ref, row, key)) break;
        const u32 id = 1u + key * kA11yStride;
        const Rect rr{c.x, g.bodyTop() + f32(row - s.first) * g.rh, c.w, g.rh};
        const u32 ri = u32(items.size());
        A11ySnapshotNode it = a11yItem(id, rowRole, rowRole == A11yRole::Row ? std::string_view{} : cellText(s, ref, row, 0), rr, itemActions);
        it.selectable = selectable && !cellUnit;
        it.selected = it.selectable && rowSelected(ui, s, row);
        it.posInSet = row + 1;
        it.setSize = g.rows;
        if (tree) {
            it.level = ref.depth + 1;
            it.expandable = ref.kids;
            it.expanded = ref.node != kNoNode;
            if (ref.kids && !self.disabled) it.actions |= a11yActionBit(it.expanded ? A11yAction::Collapse : A11yAction::Expand);
        }
        items.push_back(std::move(it));
        if (!withCells) continue;
        u32 pos = 0;
        eachCol(s, g, [&](u32 k, u32, f32 x, f32 w, bool frozen) {
            ++pos;
            const Rect r = clipCol(g, frozen, {x, rr.y, w, g.rh});
            if (r.empty()) return true;
            A11ySnapshotNode cell = a11yItem(id + 1u + k, A11yRole::Cell, cellText(s, ref, row, k), r);
            cell.parent = ri;
            cell.posInSet = pos;
            cell.setSize = nshown;
            const u32 flags = itemFlags(s, ref.id, k);
            cell.disabled = self.disabled || !(flags & kItemEnabled);
            if (cellUnit) {
                cell.selectable = selectable;
                cell.selected = selectable && s.sel->isSelected(row, k);
                if (!cell.disabled) cell.actions |= detail::kA11yItemActions;
            }
            if (m.role && (flags & kItemCheckable)) {
                ItemValue v;
                if (m.role(m.user, ref.id, k, ItemRole::CheckState, v, s.scratch2)) {
                    cell.checked = v.integer == 1;
                    if (!cell.disabled && m.setData) cell.actions |= a11yActionBit(A11yAction::Click); // toggles the check
                }
            }
            if (!cell.disabled && m.setData && m.flags && (flags & kItemEditable)) cell.actions |= a11yActionBit(A11yAction::SetValue);
            items.push_back(std::move(cell));
            return true;
        });
    }
}
bool viewA11yAction(UiContext& ui, NodeHandle n, u32 child, A11yAction a, f32, std::string_view text) {
    ViewState* sp = stateOf(ui, n);
    if (!sp || !sp->model || !child) return false;
    ViewState& s = *sp;
    if (child >= kA11yHeaderRow) { // a column header: Click sorts by it, as a pointer click does
        const u32 k = child - kA11yHeaderRow;
        if (!s.header || k == 0 || k > cols(s) || colWidth(s, k - 1) <= 0.f) return false;
        if (a == A11yAction::Click) { ItemView::sortClick(ui, n, s, k - 1); return true; }
        if (a == A11yAction::ScrollIntoView) { scrollToCol(ui, n, s, k - 1); return true; }
        return false;
    }
    const u32 key = (child - 1u) / kA11yStride, sub = (child - 1u) % kA11yStride; // sub 0 = the row, else cell sub - 1
    const i64 found = a11yRowOf(ui, n, s, key);
    if (found < 0) return false;
    const u32 row = u32(found);
    const bool cell = sub != 0;
    const u32 col = cell ? sub - 1u : s.curCol;
    if (cell && (s.list || col >= cols(s) || colWidth(s, col) <= 0.f)) return false;
    switch (a) {
    case A11yAction::Click:
        if (cell && toggleCheck(ui, n, s, row, col)) return true;
        [[fallthrough]];
    case A11yAction::Select: {
        if (s.editor.valid()) commitEditor(ui, n, s);
        // Select is idempotent (a selected item only becomes current); Click follows the pointer rules of the mode
        const bool cells = s.sel && s.unit == SelectionUnit::Cells;
        const bool selected = s.sel ? s.sel->isSelected(row, cells ? col : 0u) : i64(ui.value(n)) == i64(row);
        if (a == A11yAction::Select && selected) {
            scrollToRow(ui, n, s, row);
            if (s.sel) s.sel->setCurrent(row, col);
            s.curCol = col;
        } else {
            setCurrent(ui, n, s, row, col, 0, Via::Pointer);
        }
        return true;
    }
    case A11yAction::ScrollIntoView:
        scrollToRow(ui, n, s, row);
        if (cell) scrollToCol(ui, n, s, col);
        return true;
    case A11yAction::Expand: return expandRow(ui, n, s, row);
    case A11yAction::Collapse: return collapseRow(ui, n, s, row);
    case A11yAction::SetValue: { // an editable cell: the text goes to the model as an edit would
        if (!cell || !cellEditable(s, row, col)) return false;
        if (s.editor.valid() && s.editRow == row && s.editCol == col) closeEditor(ui, n, s);
        const std::string copy(text);
        ItemValue v;
        v.text = copy;
        parseNum(v.text, v.number);
        const bool ok = s.model->setData(s.model->user, itemOf(s, row), col, ItemRole::Edit, v);
        ui.invalidate(n);
        return ok;
    }
    default: return false;
    }
}
} // namespace
void detail::a11yModelView(WidgetRegistry& r) {
    a11yHook(r, "tableview", A11yRole::Table, &viewA11y, &viewA11yAction);
    a11yHook(r, "treemodelview", A11yRole::Tree, &viewA11y, &viewA11yAction);
}
} // namespace fuse::ui::widgets
