// FUSE UI Tier B (plan §5), second half: splitters, dock area, item models, virtualised table view.
#include "fuse/ui/from_chars.hpp"
#include "fuse/ui/widgets/widgets.hpp"
#include "widgets_internal.hpp"
#include "item_selection.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <memory>
#include <numeric>

namespace fuse::ui::widgets {
namespace {

NodeHandle make(UiContext& ui, std::string_view cls, NodeHandle parent, std::string_view id = {}) {
    auto h = ui.create(cls, parent, id);
    return h ? *h : NodeHandle{};
}
NodeHandle prevSibling(const UiContext& ui, NodeHandle n) {
    NodeHandle prev;
    for (NodeHandle c = ui.firstChild(ui.parent(n)); ui.alive(c) && c != n; c = ui.nextSibling(c)) prev = c;
    return prev;
}
f32 rowHeight(const UiContext& ui, NodeHandle n) {
    const f32 px = ui.computed(n).fontSize;
    return std::ceil(ui.font() ? ui.font()->metrics(px).lineHeight() : px * 1.25f) + 6.f;
}
void setBasis(UiContext& ui, NodeHandle pane, f32 px) {
    ui.set(pane, Prop::Basis, PropValue::ofLength(Length::px(std::round(px))));
}

// ---- splitter ----------------------------------------------------------------------------------------------------
constexpr f32 kMinPane = 24.f;
struct SplitHandle { // between two panes; value() = 1 when the splitter is stacked
    static constexpr std::string_view kName = "splithandle";
    static constexpr A11yRole kRole = A11yRole::Separator;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {6.f, 6.f}; }
    static bool stacked(const UiContext& ui, NodeHandle n) { return ui.value(n) > 0.5f; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect r = pc.rect;
        const bool hot = pc.states & (kStateHover | kStatePressed | kStateFocus);
        const Color c = pc.tint(hot ? Color::rgb(0x3D8BFD) : Color::rgb(0x5A5A66));
        if (stacked(pc.ui, pc.node)) dl.rect({r.x + r.w * 0.5f - 12.f, r.y + r.h * 0.5f - 1.f, 24.f, 2.f}, c, 1.f);
        else dl.rect({r.x + r.w * 0.5f - 1.f, r.y + r.h * 0.5f - 12.f, 2.f, 24.f}, c, 1.f);
    }
    /// Moves the handle so the previous pane ends at `pos` (context coordinate along the split axis).
    static void moveTo(UiContext& ui, NodeHandle h, f32 pos) {
        const NodeHandle a = prevSibling(ui, h), b = ui.nextSibling(h);
        if (!ui.alive(a) || !ui.alive(b)) return;
        const bool v = stacked(ui, h);
        const Rect ra = ui.rect(a), rb = ui.rect(b);
        const f32 start = v ? ra.y : ra.x, total = v ? ra.h + rb.h : ra.w + rb.w;
        if (total < 2.f * kMinPane) return;
        f32 na = std::clamp(pos - start, kMinPane, total - kMinPane);
        if (ui.hasClass(ui.parent(h), "collapsible")) { // QSplitter::setChildrenCollapsible: past half the minimum, snap shut
            if (pos - start < kMinPane * 0.5f) na = 0.f;
            else if (start + total - pos < kMinPane * 0.5f) na = total;
        }
        const bool fa = ui.hasClass(a, "fixed"), fb = ui.hasClass(b, "fixed");
        if (fa || !fb) { // the fixed pane follows the handle; with none fixed the first pane becomes fixed
            if (!fa) { ui.addClass(a, "fixed"); ui.setCss(a, "flex-grow", "0"); ui.setCss(a, "flex-shrink", "0"); }
            setBasis(ui, a, na);
        }
        if (fb) setBasis(ui, b, total - na);
        ui.emit(ui.parent(h), SignalKind::ValueChanged, {.value = na});
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        const bool v = stacked(ui, ec.node);
        if (e.type == EventType::PointerDown) return true;
        if (e.type == EventType::PointerMove && ui.pressed() == ec.node) {
            moveTo(ui, ec.node, (v ? e.pos.y : e.pos.x) - 3.f);
            return true;
        }
        if (e.type == EventType::KeyDown) {
            const f32 d = (v ? (e.key == Key::Down) - (e.key == Key::Up) : (e.key == Key::Right) - (e.key == Key::Left)) * 16.f;
            if (d == 0.f) return false;
            const Rect ra = ui.rect(prevSibling(ui, ec.node));
            moveTo(ui, ec.node, (v ? ra.bottom() : ra.right()) + d);
            return true;
        }
        return false;
    }
};

NodeHandle addPane(UiContext& ui, NodeHandle split, f32 size, std::string_view id) {
    if (ui.firstChild(split).valid()) {
        const NodeHandle h = make(ui, "splithandle", split);
        ui.setRange(h, 0.f, 1.f, 0.f);
        ui.setValue(h, ui.hasClass(split, "stacked") ? 1.f : 0.f);
        ui.setCss(h, "flex-shrink", "0");
    }
    const NodeHandle p = panel(ui, split, id);
    ui.addClass(p, "pane");
    ui.setCss(p, "padding", "0px");
    if (size > 0.f) {
        ui.addClass(p, "fixed");
        setBasis(ui, p, size);
        ui.setCss(p, "flex-grow", "0");
        ui.setCss(p, "flex-shrink", "0");
    } else {
        setBasis(ui, p, 0.f);
        ui.setCss(p, "flex-grow", "1");
    }
    return p;
}

// ---- dock area: column [ row [left | centre | right] | bottom ] ----------------------------------------------------
struct DockTitle { // the title strip of a dock widget; text = title. Close box at the right end.
    static constexpr std::string_view kName = "docktitle";
    static constexpr A11yRole kRole = A11yRole::Text;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) { return {std::ceil(ui.measureString(ui.text(n), ui.computed(n).fontSize)) + 24.f, rowHeight(ui, n) - 2.f}; }
    static Rect closeBox(const Rect& r) { return {r.right() - r.h, r.y, r.h, r.h}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect r = pc.rect;
        dl.rect(r, pc.tint(Color::rgb(0x2A2A33)), 3.f);
        pc.ui.paintString(dl, pc.ui.text(pc.node), {pc.content.x, r.y, pc.content.w - r.h, r.h}, pc.tint(pc.style.color), pc.style.fontSize,
                          TextAlignment::Start, pc.node.index);
        const Rect x = closeBox(r);
        const Color c = pc.tint(pc.ui.hovered() == pc.node && x.contains(pc.ui.pointer()) ? Color::rgb(0xFF6B6B) : Color::rgb(0x9A9AA6));
        const f32 m = x.h * 0.32f;
        dl.line({x.x + m, x.y + m}, {x.right() - m, x.bottom() - m}, c, 1.5f);
        dl.line({x.right() - m, x.y + m}, {x.x + m, x.bottom() - m}, c, 1.5f);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        if (e.type == EventType::PointerMove) { ec.ui.invalidate(ec.node); return false; }
        if (e.type == EventType::Click && closeBox(ec.ui.rect(ec.node)).contains(e.pos)) { dockClose(ec.ui, ec.ui.parent(ec.node)); return true; }
        return false;
    }
};

NodeHandle nthChild(const UiContext& ui, NodeHandle n, u32 i) {
    NodeHandle c = ui.firstChild(n);
    while (i-- && ui.alive(c)) c = ui.nextSibling(c);
    return c;
}
bool anyShown(const UiContext& ui, NodeHandle region) {
    for (NodeHandle c = ui.firstChild(region); ui.alive(c); c = ui.nextSibling(c))
        // dock widgets: the closed class is authoritative (computed visibility is stale right after dockTo re-shows one)
        if (ui.hasClass(c, "dock") ? !ui.hasClass(c, "dock-closed") : ui.computed(c).visible) return true;
    return false;
}
void showNode(UiContext& ui, NodeHandle n, bool on) { ui.setCss(n, "display", on ? "flex" : "none"); }
/// Shows a side region (and its handle) only while it holds an open dock widget.
void refreshDock(UiContext& ui, NodeHandle area) {
    const NodeHandle row = nthChild(ui, area, 0);
    for (DockSide s : {DockSide::Left, DockSide::Right, DockSide::Bottom}) {
        const NodeHandle reg = dockRegion(ui, area, s);
        const bool on = anyShown(ui, reg);
        showNode(ui, reg, on);
        showNode(ui, s == DockSide::Left ? nthChild(ui, row, 1) : s == DockSide::Right ? nthChild(ui, row, 3) : nthChild(ui, area, 1), on);
    }
}
NodeHandle areaOf(const UiContext& ui, NodeHandle dock) {
    NodeHandle n = dock;
    while (ui.alive(n) && !ui.hasClass(n, "dockarea")) n = ui.parent(n);
    return n;
}

// ---- table view ---------------------------------------------------------------------------------------------------
constexpr u32 kMaxCols = 32;
struct TableState {
    NodeHandle node;
    const ItemModel* model = nullptr;
    u32 first = 0;
    f32 colW[kMaxCols]{};
    i32 sortCol = -1;
    bool asc = true;
    bool header = true;
    i32 resizing = -1;
    bool dragScroll = false;
    f32 grab = 0.f;
    std::string scratch;
    const CellDelegate* delegate[kMaxCols]{};
    TableDragDrop dd;
    detail::ItemSelection sel;
    u32 curCol = 0;
    std::vector<u32> dragRows; ///< rows of the drag this table started
    bool droppedSelf = false;
    f32 xOff = 0.f;      ///< horizontal scroll (QAbstractScrollArea::horizontalScrollBar)
    bool hDrag = false;  ///< dragging the horizontal bar's thumb
    bool band = false;   ///< rubber-band selection in progress (QAbstractItemView::rubberBand)
    Vec2 bandA, bandB;
    std::set<u64> bandBase; ///< Ctrl+band: the selection the band adds to
    i64 pendingRow = -1, pendingCol = 0; ///< press on a selected item: narrowed on release unless it became a drag
    u8 order[kMaxCols]{};    ///< visual -> logical column (QHeaderView::logicalIndex)
    f32 hiddenW[kMaxCols]{}; ///< > 0: column hidden, width to restore (QHeaderView::hideSection)
    bool movable = false;    ///< QHeaderView::setSectionsMovable
    i32 hdrPress = -1;       ///< logical section pressed (movable header: sort on release, or move on drag)
    bool hdrMove = false;
    f32 hdrX = 0.f, hdrAt = 0.f;
};
// Process-wide pool (the UI is single-threaded); a slot is reused once its node is gone.
std::vector<std::unique_ptr<TableState>>& pool() { static std::vector<std::unique_ptr<TableState>> p; return p; }
TableState* stateOf(const UiContext& ui, NodeHandle n) {
    const u64 slot = ui.userData(n);
    auto& p = pool();
    if (slot == 0 || slot > p.size()) return nullptr;
    TableState* s = p[slot - 1].get();
    return s->node == n ? s : nullptr;
}
TableState& newState(UiContext& ui, NodeHandle n) {
    auto& p = pool();
    for (std::size_t i = 0; i < p.size(); ++i)
        if (!ui.alive(p[i]->node) || ui.classOf(p[i]->node) != ui.registry().find("tableview")) {
            *p[i] = TableState{};
            p[i]->node = n;
            ui.setUserData(n, i + 1);
            return *p[i];
        }
    p.push_back(std::make_unique<TableState>());
    p.back()->node = n;
    ui.setUserData(n, p.size());
    return *p.back();
}

struct TableView {
    static constexpr std::string_view kName = "tableview";
    static constexpr A11yRole kRole = A11yRole::Grid;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static constexpr f32 kBar = 8.f;
    static u32 rows(const TableState& s) { return s.model && s.model->rows ? s.model->rows(s.model->user) : 0u; }
    static u32 cols(const TableState& s) { return s.model ? std::min(s.model->columns, kMaxCols) : 0u; }
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        const TableState* s = stateOf(ui, n);
        f32 w = 0.f;
        if (s) for (u32 c = 0; c < cols(*s); ++c) w += s->colW[c];
        return {std::max(160.f, w + kBar), rowHeight(ui, n) * 8.f};
    }
    static f32 headerH(const UiContext& ui, NodeHandle n, const TableState& s) { return s.header ? rowHeight(ui, n) : 0.f; }
    static f32 totalW(const TableState& s) { f32 w = 0.f; for (u32 k = 0; k < cols(s); ++k) w += s.colW[k]; return w; }
    static f32 viewW(const UiContext& ui, NodeHandle n) { return std::max(1.f, ui.contentRect(n).w - kBar); }
    static f32 maxX(const UiContext& ui, NodeHandle n, const TableState& s) { return std::max(0.f, totalW(s) - viewW(ui, n)); }
    static bool hasH(const UiContext& ui, NodeHandle n, const TableState& s) { return maxX(ui, n, s) > 0.5f; }
    static void setX(UiContext& ui, NodeHandle n, TableState& s, f32 x) {
        x = std::clamp(std::round(x), 0.f, maxX(ui, n, s));
        if (x != s.xOff) { s.xOff = x; ui.invalidate(n); }
    }
    static bool hBarGeom(const UiContext& ui, NodeHandle n, const TableState& s, Rect& track, Rect& thumb) {
        if (!hasH(ui, n, s)) return false;
        const Rect c = ui.contentRect(n);
        track = {c.x, c.bottom() - kBar, c.w - kBar, kBar};
        const f32 tw = std::max(16.f, track.w * viewW(ui, n) / totalW(s));
        thumb = {track.x + (track.w - tw) * s.xOff / std::max(1.f, maxX(ui, n, s)), track.y, tw, kBar};
        return true;
    }
    /// keeps column `col` in view (QAbstractItemView::scrollTo for the current cell)
    static void showCol(UiContext& ui, NodeHandle n, TableState& s, u32 col) {
        f32 x0 = 0.f;
        for (u32 vi = 0; vi < cols(s) && s.order[vi] != col; ++vi) x0 += s.colW[s.order[vi]];
        const f32 x1 = x0 + (col < cols(s) ? s.colW[col] : 0.f), vw = viewW(ui, n);
        if (x0 < s.xOff) setX(ui, n, s, x0);
        else if (x1 > s.xOff + vw) setX(ui, n, s, std::min(x0, x1 - vw));
    }
    /// logical column under content-x `x` (the last visible one past the end)
    static u32 colAt(const TableState& s, f32 x) {
        u32 last = s.order[0];
        f32 at = 0.f;
        for (u32 vi = 0; vi < cols(s); ++vi) {
            const u32 k = s.order[vi];
            if (s.colW[k] <= 0.f) continue;
            last = k;
            if (x < (at += s.colW[k])) return k;
        }
        return last;
    }
    static u32 visualOf(const TableState& s, u32 logical) {
        for (u32 vi = 0; vi < cols(s); ++vi) if (s.order[vi] == logical) return vi;
        return logical;
    }
    /// next shown column from `col`, `dir` visual steps (Left/Right follow the visual order and skip hidden ones)
    static i64 stepCol(const TableState& s, i64 col, i32 dir) {
        i64 vi = visualOf(s, u32(std::max<i64>(col, 0)));
        for (vi += dir; vi >= 0 && vi < i64(cols(s)); vi += dir)
            if (s.colW[s.order[vi]] > 0.f) return s.order[vi];
        return -1;
    }
    static void moveSection(UiContext& ui, NodeHandle n, TableState& s, u32 from, u32 to) {
        const u32 nc = cols(s);
        if (from >= nc || to >= nc || from == to) return;
        const u8 k = s.order[from];
        if (from < to) std::copy(s.order + from + 1, s.order + to + 1, s.order + from);
        else std::copy_backward(s.order + to, s.order + from, s.order + from + 1);
        s.order[to] = k;
        ui.emit(n, SignalKind::Event, {.value = f32(k), .text = "sectionMoved"});
        ui.invalidate(n);
    }
    static void bandSelect(UiContext& ui, NodeHandle n, TableState& s) {
        const Rect c = ui.contentRect(n);
        const f32 rh = rowHeight(ui, n), hh = headerH(ui, n, s);
        const u32 nr = rows(s), nc = cols(s);
        const f32 y0 = std::min(s.bandA.y, s.bandB.y), y1 = std::max(s.bandA.y, s.bandB.y);
        const f32 x0 = std::min(s.bandA.x, s.bandB.x), x1 = std::max(s.bandA.x, s.bandB.x);
        std::set<u64> keys = s.bandBase;
        const i64 r0 = std::max<i64>(0, i64(s.first) + i64(std::floor((y0 - c.y - hh) / rh)));
        const i64 r1 = std::min<i64>(i64(nr) - 1, i64(s.first) + i64(std::floor((y1 - c.y - hh) / rh)));
        std::vector<u32> cs;
        f32 x = c.x - s.xOff;
        for (u32 vi = 0, k = s.order[0]; vi < nc; x += s.colW[k], ++vi, k = vi < nc ? s.order[vi] : 0)
            if (s.colW[k] > 0.f && x < x1 && x + s.colW[k] > x0) cs.push_back(k);
        for (i64 r = r0; r <= r1; ++r) {
            if (s.sel.behavior == SelectionBehavior::Rows) keys.insert(u64(r));
            else for (u32 k : cs) keys.insert(s.sel.key(r, k));
        }
        if (keys != s.sel.keys) { s.sel.keys = std::move(keys); changed(ui, n); }
        ui.invalidate(n);
    }
    static u32 visible(const UiContext& ui, NodeHandle n, const TableState& s) {
        const f32 h = ui.contentRect(n).h - headerH(ui, n, s) - (hasH(ui, n, s) ? kBar : 0.f);
        return std::max(1u, u32(h / rowHeight(ui, n)));
    }
    static void setFirst(UiContext& ui, NodeHandle n, TableState& s, i64 f) {
        const u32 r = rows(s), vis = visible(ui, n, s);
        const u32 nf = u32(std::clamp<i64>(f, 0, r > vis ? i64(r - vis) : 0));
        if (nf != s.first) { s.first = nf; ui.invalidate(n); }
    }
    static void select(UiContext& ui, NodeHandle n, TableState& s, i64 row) {
        const u32 r = rows(s);
        if (!r) return;
        row = std::clamp<i64>(row, 0, i64(r) - 1);
        const u32 vis = visible(ui, n, s);
        if (row < i64(s.first)) setFirst(ui, n, s, row);
        else if (row >= i64(s.first + vis)) setFirst(ui, n, s, row - vis + 1);
        if (i64(ui.value(n)) == row) return;
        ui.setValue(n, f32(row), true);
        if (s.sel.mode == SelectionMode::Single) ui.emit(n, SignalKind::SelectionChanged, {.value = f32(row)});
    }
    static auto rowsFn() {
        return [](i64 a, i64 b, std::vector<i64>& out) { for (i64 r = std::min(a, b); r <= std::max(a, b); ++r) out.push_back(r); };
    }
    static bool cellSel(const UiContext& ui, NodeHandle n, const TableState& s, i64 row, u32 col) {
        return s.sel.has(row, col, i64(ui.value(n)), s.curCol);
    }
    static void changed(UiContext& ui, NodeHandle n) {
        ui.invalidate(n);
        ui.emit(n, SignalKind::SelectionChanged, {.value = ui.value(n)});
    }
    static void navTo(UiContext& ui, NodeHandle n, TableState& s, i64 row, i64 col, u8 mods) {
        const u32 r = rows(s), nc = cols(s);
        if (!r) return;
        row = std::clamp<i64>(row, 0, i64(r) - 1);
        col = std::clamp<i64>(col, 0, nc ? i64(nc) - 1 : 0);
        const bool colMoved = u32(col) != s.curCol;
        s.curCol = u32(col);
        if (s.sel.behavior != SelectionBehavior::Rows) showCol(ui, n, s, u32(col));
        select(ui, n, s, row);
        if (colMoved && s.sel.mode == SelectionMode::Single && s.sel.behavior != SelectionBehavior::Rows)
            ui.emit(n, SignalKind::SelectionChanged, {.value = f32(row)});
        if (s.sel.navigate(row, col, mods, rowsFn())) changed(ui, n);
        ui.invalidate(n);
    }
    static bool barGeom(const UiContext& ui, NodeHandle n, const TableState& s, Rect& track, Rect& thumb) {
        const u32 r = rows(s), vis = visible(ui, n, s);
        if (r <= vis) return false;
        const Rect c = ui.contentRect(n);
        const f32 hh = headerH(ui, n, s);
        track = {c.right() - kBar, c.y + hh, kBar, c.h - hh};
        const f32 tl = std::max(16.f, track.h * f32(vis) / f32(r));
        thumb = {track.x, track.y + (track.h - tl) * f32(s.first) / f32(r - vis), kBar, tl};
        return true;
    }
    /// QAccessibleTable: header row of ColumnHeaders, then the shown rows (virtualised like the paint) as Rows of
    /// GridCells. Part ids: header row 1, header cell 2 + col; data row r: (r + 1) * (kMaxCols + 1) + 1, its cells + 1 + col.
    static void a11yParts(const UiContext& cui, NodeHandle n, A11ySnapshotNode& self, std::vector<A11ySnapshotNode>& out) {
        UiContext& ui = const_cast<UiContext&>(cui); // model reads use the view's scratch string
        TableState* s = stateOf(ui, n);
        if (!s || !s->model) return;
        const Rect c = ui.contentRect(n);
        const f32 rh = rowHeight(ui, n), hh = headerH(ui, n, *s);
        const u32 nc = cols(*s), nr = rows(*s), vis = visible(ui, n, *s), h = s->header ? 1u : 0u;
        self.rowCount = nr + h;
        self.colCount = nc;
        const i64 sel = i64(ui.value(n));
        if (h) {
            const u32 rowAt = u32(out.size());
            A11ySnapshotNode r;
            r.role = A11yRole::Row; r.part = 1; r.rowIndex = 0; r.bounds = {c.x, c.y, c.w, hh};
            out.push_back(std::move(r));
            f32 x = c.x - s->xOff;
            for (u32 vi = 0; vi < nc; ++vi) { const u32 k = s->order[vi];
                if (s->hiddenW[k] > 0.f) continue; // hidden sections are not exposed
                A11ySnapshotNode hc;
                hc.role = A11yRole::ColumnHeader; hc.part = 2 + k; hc.parent = rowAt; hc.rowIndex = 0; hc.colIndex = i32(k);
                hc.name = std::string(s->model->header ? s->model->header(s->model->user, k) : std::string_view{});
                hc.bounds = {x, c.y, s->colW[k], hh};
                if (i32(k) == s->sortCol) hc.sortDirection = s->asc ? 1 : 2; // QHeaderView sort indicator
                x += s->colW[k];
                out.push_back(std::move(hc));
            }
        }
        for (u32 i = 0; i < vis + 1 && s->first + i < nr; ++i) {
            const u32 row = s->first + i;
            const f32 y = c.y + hh + f32(i) * rh;
            if (y >= c.bottom()) break;
            const u32 id = (row + 1u) * (kMaxCols + 1u) + 1u, rowAt = u32(out.size());
            A11ySnapshotNode r;
            r.role = A11yRole::Row; r.part = id; r.rowIndex = i32(row + h); r.bounds = {c.x, y, c.w, rh};
            r.state.selected = s->sel.rowFullySelected(row, nc, sel, s->curCol);
            out.push_back(std::move(r));
            f32 x = c.x - s->xOff;
            for (u32 vi = 0; vi < nc; ++vi) { const u32 k = s->order[vi];
                if (s->hiddenW[k] > 0.f) continue; // hidden sections are not exposed
                A11ySnapshotNode gc;
                gc.role = A11yRole::GridCell; gc.part = id + 1u + k; gc.parent = rowAt;
                gc.rowIndex = i32(row + h); gc.colIndex = i32(k);
                gc.name = std::string(s->model->data(s->model->user, row, k, s->scratch));
                gc.bounds = {x, y, s->colW[k], rh};
                gc.state.selected = cellSel(ui, n, *s, row, k);
                gc.focused = i64(row) == sel && k == std::min(s->curCol, nc - 1); // the current cell; ANDed with the view's focus
                x += s->colW[k];
                out.push_back(std::move(gc));
            }
        }
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        TableState* s = stateOf(ui, pc.node);
        if (!s) return;
        const Rect c = pc.content;
        const f32 rh = rowHeight(ui, pc.node), hh = headerH(ui, pc.node, *s), fs = pc.style.fontSize;
        const u32 nc = cols(*s), nr = rows(*s), vis = visible(ui, pc.node, *s);
        const Color text = pc.tint(pc.style.color), dim = pc.tint(pc.style.color.withAlpha(pc.style.color.a * 0.65f));
        const Color acc = pc.tint(pc.style.background2.a > 0.f ? pc.style.background2 : Color::rgb(0x3D8BFD));
        dl.pushClip(c);
        if (s->header) {
            dl.rect({c.x, c.y, c.w, hh}, pc.tint(Color::rgb(0x2A2A33)));
            f32 x = c.x - s->xOff;
            for (u32 vi = 0; vi < nc; ++vi) { const u32 k = s->order[vi];
                const Rect cell{x + 6.f, c.y, s->colW[k] - 12.f, hh};
                const std::string_view h = s->model->header ? s->model->header(s->model->user, k) : std::string_view{};
                dl.pushClip({x, c.y, s->colW[k], hh});
                ui.paintString(dl, h, cell, text, fs, TextAlignment::Start, pc.node.index);
                if (i32(k) == s->sortCol) { // sort arrow
                    const f32 ax = x + s->colW[k] - 12.f, ay = c.y + hh * 0.5f, d = s->asc ? -3.f : 3.f;
                    dl.line({ax - 4.f, ay - d}, {ax, ay + d}, acc, 1.5f);
                    dl.line({ax, ay + d}, {ax + 4.f, ay - d}, acc, 1.5f);
                }
                dl.popClip();
                x += s->colW[k];
                dl.rect({x - 1.f, c.y + 4.f, 1.f, hh - 8.f}, pc.tint(Color::rgb(0x55555F)));
            }
        }
        const i32 sel = i32(ui.value(pc.node));
        for (u32 i = 0; i < vis + 1 && s->first + i < nr; ++i) {
            const u32 row = s->first + i;
            const f32 y = c.y + hh + f32(i) * rh;
            if (y >= c.bottom()) break;
            const bool full = s->sel.rowFullySelected(row, nc, sel, s->curCol);
            if (full) dl.rect({c.x, y, c.w, rh}, acc.withAlpha(acc.a * 0.55f));
            else if (row & 1u) dl.rect({c.x, y, c.w, rh}, pc.tint(Color::rgb(0xFFFFFF, 0.035f)));
            f32 x = c.x - s->xOff;
            for (u32 vi = 0; vi < nc; ++vi) { const u32 k = s->order[vi];
                if (s->colW[k] <= 0.f) continue;
                if (!full && cellSel(ui, pc.node, *s, row, k)) dl.rect({x, y, s->colW[k], rh}, acc.withAlpha(acc.a * 0.55f));
                if (i32(row) == sel && k == s->curCol && (pc.states & kStateFocus) && (s->sel.explicitSet() || s->sel.behavior != SelectionBehavior::Rows))
                    dl.roundedRect({x + 0.5f, y + 0.5f, s->colW[k] - 1.f, rh - 1.f}, {}, {}, acc.withAlpha(acc.a * 0.8f), 1.f);
                const std::string_view d = s->model->data(s->model->user, row, k, s->scratch);
                dl.pushClip({x, y, s->colW[k] - 2.f, rh});
                if (const CellDelegate* dg = s->delegate[k]; dg && dg->paint)
                    dg->paint(dg->user, pc, dl, {x + 6.f, y, s->colW[k] - 12.f, rh}, row, k, d, cellSel(ui, pc.node, *s, row, k));
                else
                    ui.paintString(dl, d, {x + 6.f, y, s->colW[k] - 12.f, rh}, k ? dim : text, fs, TextAlignment::Start, pc.node.index);
                dl.popClip();
                x += s->colW[k];
            }
        }
        if (Rect ind; dropIndicator(ui, pc.node, ind)) dl.rect(ind, acc);
        if (s->band) {
            const Rect b{std::min(s->bandA.x, s->bandB.x), std::min(s->bandA.y, s->bandB.y), std::abs(s->bandB.x - s->bandA.x), std::abs(s->bandB.y - s->bandA.y)};
            dl.rect(b, acc.withAlpha(acc.a * 0.2f));
            dl.roundedRect(b, {}, {}, acc.withAlpha(acc.a * 0.8f), 1.f);
        }
        if (Rect htr, hth; hBarGeom(ui, pc.node, *s, htr, hth)) {
            dl.rect(htr, pc.tint(Color::rgb(0x000000, 0.15f)), kBar * 0.5f);
            dl.rect(hth, pc.tint(Color::rgb(s->hDrag ? 0xB4B4C0 : 0x8A8A96, 0.85f)), kBar * 0.5f);
        }
        Rect tr, th;
        if (barGeom(ui, pc.node, *s, tr, th)) {
            dl.rect(tr, pc.tint(Color::rgb(0x000000, 0.15f)), kBar * 0.5f);
            dl.rect(th, pc.tint(Color::rgb(s->dragScroll ? 0xB4B4C0 : 0x8A8A96, 0.85f)), kBar * 0.5f);
        }
        if (pc.states & kStateFocus) dl.roundedRect(pc.rect, {}, {}, acc.withAlpha(acc.a * 0.6f), 1.f);
        dl.popClip();
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        TableState* s = stateOf(ui, ec.node);
        if (!s) return false;
        const Rect c = ui.contentRect(ec.node);
        const f32 rh = rowHeight(ui, ec.node), hh = headerH(ui, ec.node, *s);
        switch (e.type) {
        case EventType::PointerDown: {
            s->resizing = -1;
            s->dragScroll = false;
            s->hDrag = false;
            s->band = false;
            Rect tr, th;
            if (hBarGeom(ui, ec.node, *s, tr, th) && tr.contains(e.pos)) {
                s->hDrag = true;
                s->grab = th.contains(e.pos) ? e.pos.x - th.x : th.w * 0.5f;
                Event mv = e;
                mv.type = EventType::PointerMove;
                return onEvent(ec, mv);
            }
            if (barGeom(ui, ec.node, *s, tr, th) && tr.contains(e.pos)) {
                s->dragScroll = true;
                s->grab = th.contains(e.pos) ? e.pos.y - th.y : th.h * 0.5f;
                Event mv = e;
                mv.type = EventType::PointerMove;
                return onEvent(ec, mv);
            }
            if (e.pos.y < c.y + hh) {
                f32 x = c.x - s->xOff;
                for (u32 vi = 0; vi < cols(*s); ++vi) {
                    const u32 k = s->order[vi];
                    if (s->colW[k] <= 0.f) continue;
                    x += s->colW[k];
                    if (std::abs(e.pos.x - x) <= 4.f) { s->resizing = i32(k); s->grab = x - s->colW[k]; return true; }
                    if (e.pos.x < x && s->movable) { s->hdrPress = i32(k); s->hdrMove = false; s->hdrX = s->hdrAt = e.pos.x; return true; }
                    if (e.pos.x < x) { // sort
                        s->asc = s->sortCol == i32(k) ? !s->asc : true;
                        s->sortCol = i32(k);
                        ui.emit(ec.node, SignalKind::Toggled, {.value = f32(k), .flag = s->asc});
                        ui.invalidate(ec.node);
                        return true;
                    }
                }
                return true;
            }
            const i64 row = i64(s->first) + i64((e.pos.y - c.y - hh) / rh);
            if (row < i64(rows(*s))) {
                const u32 col = colAt(*s, e.pos.x - c.x + s->xOff);
                const bool dragging = (s->dd.drag || s->dd.internalMove) && e.button == MouseButton::Left;
                s->pendingRow = -1;
                s->curCol = col;
                select(ui, ec.node, *s, row);
                if (dragging && !(e.mods & (kModShift | kModCtrl | kModSuper)) && s->sel.explicitSet() && s->sel.mode != SelectionMode::Multi &&
                    s->sel.keys.contains(s->sel.key(row, col))) {
                    s->pendingRow = row; // keep the selection for a multi-row drag
                    s->pendingCol = col;
                } else if (s->sel.click(row, col, e.mods, rowsFn())) changed(ui, ec.node);
                ui.invalidate(ec.node);
                if (dragging) {
                    const bool internal = s->dd.internalMove && !s->dd.drag;
                    dnd::press(ui, ec.node, e.pos, [row, internal](UiContext& c2, NodeHandle self, Vec2, DragPayload& p) {
                        TableState* t = stateOf(c2, self);
                        if (!t || !t->model || row >= i64(rows(*t))) return false;
                        t->pendingRow = -1;
                        std::vector<u32> rs;
                        if (t->sel.explicitSet() && t->sel.behavior != SelectionBehavior::Columns) {
                            for (u64 k : t->sel.keys) {
                                const u32 rr = t->sel.behavior == SelectionBehavior::Rows ? u32(k) : u32(k >> 32);
                                if (rs.empty() || rs.back() != rr) rs.push_back(rr);
                            }
                            if (std::find(rs.begin(), rs.end(), u32(row)) == rs.end()) rs = {u32(row)};
                        } else rs = {u32(row)};
                        std::string txt, idx;
                        for (u32 rr : rs) {
                            if (!txt.empty()) { txt.push_back('\n'); idx.push_back(','); }
                            for (u32 k = 0; k < cols(*t); ++k) {
                                if (k) txt.push_back('\t');
                                txt.append(t->model->data(t->model->user, rr, k, t->scratch));
                            }
                            idx.append(std::to_string(rr));
                        }
                        p.set("text/plain", txt);
                        p.set("application/x-fuse-table-row", idx);
                        if (rs.size() > 1) p.label = std::to_string(rs.size()) + " rows";
                        p.actions = internal ? u8(DropMove) : u8(DropCopy | DropMove);
                        t->dragRows = rs;
                        t->droppedSelf = false;
                        return true;
                    }, [](UiContext& c2, NodeHandle self, u8 action) {
                        TableState* t = stateOf(c2, self);
                        if (!t) return;
                        std::vector<u32> rs = std::move(t->dragRows);
                        const bool own = t->droppedSelf;
                        t->dragRows.clear();
                        t->droppedSelf = false;
                        if (action != DropMove || own || rs.empty() || !t->dd.removeRows) return;
                        std::sort(rs.begin(), rs.end());
                        auto cb = t->dd.removeRows;
                        cb(c2, self, rs);
                        tableModelChanged(c2, self);
                        if ((t = stateOf(c2, self)) && t->sel.explicitSet()) { t->sel.keys.clear(); TableView::changed(c2, self); }
                    });
                }
            } else if (e.button == MouseButton::Left && (s->sel.mode == SelectionMode::Multi || s->sel.mode == SelectionMode::Extended)) {
                // empty space: rubber band (Ctrl adds to the selection)
                s->band = true;
                s->bandA = s->bandB = e.pos;
                const bool add = e.mods & (kModCtrl | kModSuper);
                s->bandBase = add ? s->sel.keys : std::set<u64>{};
                bandSelect(ui, ec.node, *s);
            }
            return true;
        }
        case EventType::PointerMove:
            if (dnd::move(ui, ec.node, e.pos, e.mods)) return true;
            if (ui.pressed() != ec.node && !s->dragScroll && !s->hDrag) return false;
            if (s->band) {
                s->bandB = e.pos;
                if (e.pos.y > c.bottom() - (hasH(ui, ec.node, *s) ? kBar : 0.f)) setFirst(ui, ec.node, *s, i64(s->first) + 1);
                else if (e.pos.y < c.y + hh) setFirst(ui, ec.node, *s, i64(s->first) - 1);
                bandSelect(ui, ec.node, *s);
            } else if (s->hDrag) {
                Rect tr, th;
                if (!hBarGeom(ui, ec.node, *s, tr, th)) return true;
                const f32 f = std::clamp((e.pos.x - tr.x - s->grab) / std::max(1.f, tr.w - th.w), 0.f, 1.f);
                setX(ui, ec.node, *s, f * maxX(ui, ec.node, *s));
                ui.invalidate(ec.node);
            } else if (s->hdrPress >= 0) {
                s->hdrAt = e.pos.x;
                if (std::abs(e.pos.x - s->hdrX) > 6.f) s->hdrMove = true; // QApplication::startDragDistance
                ui.invalidate(ec.node);
            } else if (s->resizing >= 0) {
                s->colW[s->resizing] = std::max(24.f, std::round(e.pos.x - s->grab));
                setX(ui, ec.node, *s, s->xOff); // re-clamp the horizontal scroll to the new total width
                ui.invalidate(ec.node);
            } else if (s->dragScroll) {
                Rect tr, th;
                if (!barGeom(ui, ec.node, *s, tr, th)) return true;
                const u32 r = rows(*s), vis = visible(ui, ec.node, *s);
                const f32 f = std::clamp((e.pos.y - tr.y - s->grab) / std::max(1.f, tr.h - th.h), 0.f, 1.f);
                setFirst(ui, ec.node, *s, i64(std::lround(f * f32(r - vis))));
                ui.invalidate(ec.node);
            }
            return true;
        case EventType::PointerUp:
            if (dnd::release(ui, ec.node, e.pos, e.mods)) return true;
            if (s->pendingRow >= 0) {
                const i64 r = s->pendingRow;
                s->pendingRow = -1;
                if (s->sel.click(r, s->pendingCol, 0, rowsFn())) changed(ui, ec.node);
            }
            if (s->hdrPress >= 0) {
                const u32 k = u32(s->hdrPress);
                s->hdrPress = -1;
                if (s->hdrMove) { // drop the section before the one under the pointer (QHeaderView::moveSection)
                    const u32 at = colAt(*s, e.pos.x - c.x + s->xOff);
                    f32 x0 = 0.f;
                    for (u32 vi = 0; vi < visualOf(*s, at); ++vi) x0 += s->colW[s->order[vi]];
                    u32 to = visualOf(*s, at) + (e.pos.x - c.x + s->xOff > x0 + s->colW[at] * 0.5f ? 1u : 0u);
                    const u32 from = visualOf(*s, k);
                    if (to > from) --to;
                    if (to != from) moveSection(ui, ec.node, *s, from, to);
                } else {
                    s->asc = s->sortCol == i32(k) ? !s->asc : true;
                    s->sortCol = i32(k);
                    ui.emit(ec.node, SignalKind::Toggled, {.value = f32(k), .flag = s->asc});
                }
                s->hdrMove = false;
                ui.invalidate(ec.node);
            }
            if (s->dragScroll || s->hDrag || s->band) ui.invalidate(ec.node);
            s->resizing = -1;
            s->dragScroll = false;
            s->hDrag = false;
            s->band = false;
            return false;
        case EventType::Wheel: {
            // horizontal wheel / tilt, or Shift+wheel (QAbstractScrollArea), scrolls sideways
            const f32 dx = e.wheel.x != 0.f ? e.wheel.x : (e.mods & kModShift) ? e.wheel.y : 0.f;
            if (dx != 0.f) setX(ui, ec.node, *s, s->xOff - dx * 3.f * rh);
            else setFirst(ui, ec.node, *s, i64(s->first) - i64(std::lround(e.wheel.y * 3.f)));
            return true;
        }
        case EventType::KeyDown: {
            const i64 cur = i64(ui.value(ec.node)), vis = visible(ui, ec.node, *s);
            const i64 cc = s->curCol;
            const bool ctrl = e.mods & (kModCtrl | kModSuper);
            switch (e.key) {
            case Key::Down: navTo(ui, ec.node, *s, cur + 1, cc, e.mods); return true;
            case Key::Up: navTo(ui, ec.node, *s, cur < 0 ? 0 : cur - 1, cc, e.mods); return true;
            case Key::PageDown: navTo(ui, ec.node, *s, cur + vis, cc, e.mods); return true;
            case Key::PageUp: navTo(ui, ec.node, *s, cur - vis, cc, e.mods); return true;
            case Key::Home: navTo(ui, ec.node, *s, ctrl || s->sel.behavior == SelectionBehavior::Rows ? 0 : std::max<i64>(cur, 0), ctrl || s->sel.behavior == SelectionBehavior::Rows ? cc : 0, e.mods); return true;
            case Key::End: navTo(ui, ec.node, *s, ctrl || s->sel.behavior == SelectionBehavior::Rows ? i64(rows(*s)) - 1 : std::max<i64>(cur, 0), ctrl || s->sel.behavior == SelectionBehavior::Rows ? cc : i64(cols(*s)) - 1, e.mods); return true;
            case Key::Left:
                if (s->sel.behavior == SelectionBehavior::Rows) return false;
                if (const i64 nc2 = stepCol(*s, cc, -1); nc2 >= 0) navTo(ui, ec.node, *s, std::max<i64>(cur, 0), nc2, e.mods);
                return true;
            case Key::Right:
                if (s->sel.behavior == SelectionBehavior::Rows) return false;
                if (const i64 nc2 = stepCol(*s, cc, +1); nc2 >= 0) navTo(ui, ec.node, *s, std::max<i64>(cur, 0), nc2, e.mods);
                return true;
            case Key::Space:
                if (cur < 0) return false;
                if (s->sel.mode == SelectionMode::Multi || (s->sel.mode == SelectionMode::Extended && ctrl)) {
                    s->sel.toggle(cur, cc);
                    changed(ui, ec.node);
                    return true;
                }
                return false;
            case Key::A:
                if (!ctrl) return false;
                selectAllItems(ui, ec.node);
                return true;
            case Key::Enter:
                if (cur >= 0) ui.emit(ec.node, SignalKind::Submitted, {.value = f32(cur)});
                return true;
            case Key::Escape:
                if (!dragActive(ui)) return false;
                cancelDrag(ui);
                return true;
            default: return false;
            }
        }
        default: return false;
        }
    }
};

void registerTierB2(UiContext& ui) {
    WidgetRegistry& r = ui.registry();
    if (r.find("tableview") != 0xFFFFFFFFu) return;
    r.registerWidget<SplitHandle>();
    r.registerWidget<DockTitle>();
    r.registerWidget<TableView>();
}

bool parseNum(std::string_view s, double& out) {
    const auto [p, ec] = fuse::ui::fromChars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size();
}
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

} // namespace

namespace detail {
void registerTierB2(UiContext& ui) { fuse::ui::widgets::registerTierB2(ui); }
} // namespace detail

// ---- item view selection API (tree + table) --------------------------------------------------------------------------
namespace {
struct SelView {
    detail::ItemSelection* sel = nullptr;
    TableState* table = nullptr;
    u32 rows = 0, cols = 1, curCol = 0;
};
SelView selView(const UiContext& ui, NodeHandle v) {
    SelView r;
    if (!ui.alive(v)) return r;
    if (TableState* t = stateOf(ui, v); t && ui.classOf(v) == ui.registry().find("tableview")) {
        r.sel = &t->sel; r.table = t; r.rows = TableView::rows(*t); r.cols = std::max(1u, TableView::cols(*t)); r.curCol = t->curCol;
    } else if (ui.classOf(v) == ui.registry().find("treeview")) {
        r.sel = detail::treeSelection(ui, v); r.rows = detail::treeLineCount(ui, v);
    }
    return r;
}
void selChanged(UiContext& ui, NodeHandle v, const SelView& s) {
    if (s.table) TableView::changed(ui, v);
    else detail::treeSelectionChanged(ui, v);
}
} // namespace
void setItemSelectionMode(UiContext& ui, NodeHandle view, SelectionMode m) {
    SelView s = selView(ui, view);
    if (!s.sel || s.sel->mode == m) return;
    const bool wasExplicit = s.sel->explicitSet();
    s.sel->mode = m;
    if (wasExplicit && s.sel->explicitSet()) { ui.invalidate(view); return; } // Multi / Extended / Contiguous keep it
    s.sel->keys.clear();
    s.sel->anchorR = -1;
    if (s.sel->explicitSet() && ui.value(view) >= 0.f) { // keep the current item selected
        s.sel->keys.insert(s.sel->key(i64(ui.value(view)), s.curCol));
        s.sel->anchorR = i64(ui.value(view));
        s.sel->anchorC = s.curCol;
    }
    selChanged(ui, view, s);
}
void setItemSelectionBehavior(UiContext& ui, NodeHandle view, SelectionBehavior b) {
    SelView s = selView(ui, view);
    if (!s.sel || !s.table || s.sel->behavior == b) return; // the tree is always Rows
    s.sel->behavior = b;
    s.sel->keys.clear();
    selChanged(ui, view, s);
}
SelectionMode itemSelectionMode(const UiContext& ui, NodeHandle view) {
    const SelView s = selView(ui, view);
    return s.sel ? s.sel->mode : SelectionMode::Single;
}
bool itemSelected(const UiContext& ui, NodeHandle view, u32 row, u32 col) {
    const SelView s = selView(ui, view);
    return s.sel && row < s.rows && s.sel->has(row, s.table ? col : 0, i64(ui.value(view)), s.curCol);
}
std::vector<std::pair<u32, u32>> selectedItems(const UiContext& ui, NodeHandle view) {
    std::vector<std::pair<u32, u32>> out;
    const SelView s = selView(ui, view);
    if (!s.sel) return out;
    for (u32 r = 0; r < s.rows; ++r)
        for (u32 c = 0; c < (s.table ? s.cols : 1u); ++c)
            if (s.sel->has(r, c, i64(ui.value(view)), s.curCol)) out.emplace_back(r, c);
    return out;
}
std::vector<u32> selectedRows(const UiContext& ui, NodeHandle view) {
    std::vector<u32> out;
    const SelView s = selView(ui, view);
    if (!s.sel) return out;
    for (u32 r = 0; r < s.rows; ++r)
        if (s.sel->rowFullySelected(r, s.table ? s.cols : 1u, i64(ui.value(view)), s.curCol)) out.push_back(r);
    return out;
}
void setRowSelected(UiContext& ui, NodeHandle view, u32 row, bool on) {
    SelView s = selView(ui, view);
    if (!s.sel || row >= s.rows || s.sel->mode == SelectionMode::None) return;
    if (!s.sel->explicitSet()) {
        if (on) { if (s.table) TableView::select(ui, view, *s.table, row); else { ui.setValue(view, f32(row), true); ui.emit(view, SignalKind::SelectionChanged, {.value = f32(row)}); } }
        else if (i64(ui.value(view)) == row) { ui.setValue(view, -1.f, true); selChanged(ui, view, s); }
        ui.invalidate(view);
        return;
    }
    bool changed = false;
    if (s.sel->behavior == SelectionBehavior::Columns) return;
    for (u32 c = 0; c < (s.sel->behavior == SelectionBehavior::Items ? s.cols : 1u); ++c) {
        const u64 k = s.sel->key(row, c);
        changed |= on ? s.sel->keys.insert(k).second : s.sel->keys.erase(k) > 0;
    }
    if (changed) selChanged(ui, view, s);
}
void clearItemSelection(UiContext& ui, NodeHandle view) {
    SelView s = selView(ui, view);
    if (!s.sel) return;
    if (!s.sel->explicitSet()) {
        if (ui.value(view) >= 0.f) { ui.setValue(view, -1.f, true); selChanged(ui, view, s); }
        return;
    }
    if (s.sel->keys.empty()) return;
    s.sel->keys.clear();
    selChanged(ui, view, s);
}
void selectAllItems(UiContext& ui, NodeHandle view) {
    SelView s = selView(ui, view);
    if (!s.sel) return;
    std::set<u64> all;
    if (s.sel->behavior == SelectionBehavior::Columns) for (u32 c = 0; c < s.cols; ++c) all.insert(c);
    else for (u32 r = 0; r < s.rows; ++r) {
        if (s.sel->behavior == SelectionBehavior::Rows || !s.table) all.insert(r);
        else for (u32 c = 0; c < s.cols; ++c) all.insert(detail::ItemSelection::cellKey(r, c));
    }
    if (s.sel->setAll(std::move(all))) selChanged(ui, view, s);
}
u32 tableCurrentColumn(const UiContext& ui, NodeHandle table) {
    const SelView s = selView(ui, table);
    return s.table ? s.curCol : 0u;
}

// ---- splitter API ----------------------------------------------------------------------------------------------------
NodeHandle splitter(UiContext& ui, NodeHandle parent, bool stacked, std::string_view id) {
    registerTierB(ui);
    const NodeHandle s = panel(ui, parent, id);
    ui.addClass(s, "splitter");
    if (stacked) ui.addClass(s, "stacked");
    ui.setCss(s, "flex-direction", stacked ? "column" : "row");
    ui.setCss(s, "padding", "0px");
    ui.setCss(s, "gap", "0px");
    return s;
}
NodeHandle splitterPane(UiContext& ui, NodeHandle split, f32 size, std::string_view id) { return addPane(ui, split, size, id); }

std::vector<f32> splitterSizes(const UiContext& ui, NodeHandle split) {
    std::vector<f32> out;
    const bool v = ui.hasClass(split, "stacked");
    for (NodeHandle c = ui.firstChild(split); ui.alive(c); c = ui.nextSibling(c))
        if (ui.hasClass(c, "pane")) out.push_back(std::round(v ? ui.rect(c).h : ui.rect(c).w));
    return out;
}
void setSplitterSizes(UiContext& ui, NodeHandle split, std::span<const f32> sizes) {
    std::size_t i = 0;
    for (NodeHandle c = ui.firstChild(split); ui.alive(c) && i < sizes.size(); c = ui.nextSibling(c)) {
        if (!ui.hasClass(c, "pane")) continue;
        const f32 px = sizes[i++];
        if (px >= 0.f) {
            ui.addClass(c, "fixed");
            ui.setCss(c, "flex-grow", "0");
            ui.setCss(c, "flex-shrink", "0");
            setBasis(ui, c, px);
        } else { // < 0: back to sharing the rest
            ui.removeClass(c, "fixed");
            ui.setCss(c, "flex-grow", "1");
            ui.setCss(c, "flex-shrink", "1");
            setBasis(ui, c, 0.f);
        }
    }
}
void setSplitterCollapsible(UiContext& ui, NodeHandle split, bool on) {
    if (on) ui.addClass(split, "collapsible");
    else ui.removeClass(split, "collapsible");
    const bool v = ui.hasClass(split, "stacked");
    for (NodeHandle c = ui.firstChild(split); ui.alive(c); c = ui.nextSibling(c))
        if (ui.hasClass(c, "pane")) { // a collapsed pane must be able to reach 0 px
            ui.setCss(c, v ? "min-height" : "min-width", on ? "0px" : "auto");
            ui.setCss(c, "overflow", on ? "hidden" : "visible");
        }
}

// ---- dock API --------------------------------------------------------------------------------------------------------
NodeHandle dockArea(UiContext& ui, NodeHandle parent, std::string_view id) {
    const NodeHandle area = splitter(ui, parent, true, id);
    ui.addClass(area, "dockarea");
    ui.setCss(area, "flex-grow", "1");
    const NodeHandle mid = splitterPane(ui, area);
    ui.setCss(mid, "flex-direction", "row");
    ui.setCss(mid, "gap", "0px");
    splitterPane(ui, area, 160.f); // bottom
    // the middle row is itself a side-by-side splitter
    ui.addClass(mid, "splitter");
    splitterPane(ui, mid, 200.f); // left
    splitterPane(ui, mid);        // centre
    splitterPane(ui, mid, 200.f); // right
    for (DockSide s : {DockSide::Left, DockSide::Right, DockSide::Bottom}) ui.setCss(dockRegion(ui, area, s), "gap", "4px");
    refreshDock(ui, area);
    return area;
}
NodeHandle dockRegion(const UiContext& ui, NodeHandle area, DockSide side) {
    const NodeHandle row = nthChild(ui, area, 0);
    switch (side) {
    case DockSide::Left: return nthChild(ui, row, 0);
    case DockSide::Center: return nthChild(ui, row, 2);
    case DockSide::Right: return nthChild(ui, row, 4);
    case DockSide::Bottom: return nthChild(ui, area, 2);
    }
    return {};
}
NodeHandle dockWidget(UiContext& ui, NodeHandle area, std::string_view title, DockSide side, std::string_view id) {
    registerTierB(ui);
    const NodeHandle d = panel(ui, dockRegion(ui, area, side), id);
    ui.addClass(d, "dock");
    ui.setA11y(d, A11yRole::Group, title);
    ui.setCss(d, "flex-grow", "1");
    ui.setCss(d, "padding", "0px");
    ui.setCss(d, "gap", "0px");
    const NodeHandle t = make(ui, "docktitle", d);
    ui.setText(t, title);
    ui.setCss(t, "padding", "0px 8px");
    const NodeHandle body = panel(ui, d);
    ui.setCss(body, "flex-grow", "1");
    refreshDock(ui, area);
    return d;
}
NodeHandle dockBody(const UiContext& ui, NodeHandle dock) { return nthChild(ui, dock, 1); }
void dockTo(UiContext& ui, NodeHandle dock, DockSide side) {
    const NodeHandle area = areaOf(ui, dock);
    if (!ui.alive(area)) return;
    ui.moveTo(dock, dockRegion(ui, area, side));
    const bool wasClosed = ui.hasClass(dock, "dock-closed");
    ui.removeClass(dock, "dock-closed");
    showNode(ui, dock, true);
    refreshDock(ui, area);
    if (wasClosed) ui.emit(dock, SignalKind::Toggled, {.flag = true});
}
void dockClose(UiContext& ui, NodeHandle dock) {
    if (!ui.alive(dock) || ui.hasClass(dock, "dock-closed")) return;
    ui.addClass(dock, "dock-closed");
    showNode(ui, dock, false);
    refreshDock(ui, areaOf(ui, dock));
    ui.emit(dock, SignalKind::Toggled, {.flag = false});
}

// ---- item models -------------------------------------------------------------------------------------------------------
StringTableModel::StringTableModel(std::string_view headers) {
    eachTab(headers, [&](std::string_view h) { m_headers.emplace_back(h); });
    m_cols = u32(m_headers.size());
    m_model.user = this;
    m_model.columns = m_cols;
    m_model.rows = [](const void* u) { return static_cast<const StringTableModel*>(u)->m_rows; };
    m_model.data = [](const void* u, u32 r, u32 c, std::string&) { return static_cast<const StringTableModel*>(u)->cell(r, c); };
    m_model.header = [](const void* u, u32 c) { return std::string_view(static_cast<const StringTableModel*>(u)->m_headers[c]); };
}
void StringTableModel::addRow(std::string_view cells) {
    u32 n = 0;
    eachTab(cells, [&](std::string_view c) { if (n++ < m_cols) m_cells.emplace_back(c); });
    for (; n < m_cols; ++n) m_cells.emplace_back();
    ++m_rows;
}
void StringTableModel::insertRows(u32 at, std::string_view rowsTsv) {
    at = std::min(at, m_rows);
    std::vector<std::string> add;
    u32 n = 0;
    for (std::size_t p = 0; p <= rowsTsv.size();) {
        const std::size_t e = std::min(rowsTsv.find('\n', p), rowsTsv.size());
        const std::string_view line = rowsTsv.substr(p, e - p);
        p = e + 1;
        if (line.empty()) continue;
        u32 k = 0;
        eachTab(line, [&](std::string_view c) { if (k++ < m_cols) add.emplace_back(c); });
        for (; k < m_cols; ++k) add.emplace_back();
        ++n;
    }
    m_cells.insert(m_cells.begin() + std::ptrdiff_t(std::size_t(at) * m_cols), add.begin(), add.end());
    m_rows += n;
}
void StringTableModel::removeRows(const std::vector<u32>& rows) {
    std::vector<u32> rs(rows);
    std::sort(rs.begin(), rs.end(), std::greater<>());
    rs.erase(std::unique(rs.begin(), rs.end()), rs.end());
    for (u32 r : rs) {
        if (r >= m_rows) continue;
        const auto b = m_cells.begin() + std::ptrdiff_t(std::size_t(r) * m_cols);
        m_cells.erase(b, b + m_cols);
        --m_rows;
    }
}
void setTableDragDrop(UiContext& ui, NodeHandle table, TableDragDrop opts) {
    TableState* s = stateOf(ui, table);
    if (!s) return;
    const bool target = opts.internalMove || opts.acceptDrops;
    s->dd = std::move(opts);
    if (!target) { setDropTarget(ui, table, {}); return; }
    setDropTarget(ui, table, [](UiContext& c, NodeHandle t, const DropEvent& e) -> u8 {
        if (e.phase == DropPhase::Leave) { setDropIndicator(c, t, {}); return 0; }
        TableState* st = stateOf(c, t);
        if (!st) return DropIgnore;
        if (e.source != t || !e.payload->has("application/x-fuse-table-row")) { // foreign rows (DragDrop mode)
            if (!st->dd.acceptDrops || !st->dd.insertRows || !e.payload->has("text/plain")) return DropIgnore;
            const u8 action = e.proposed & DropCopy ? u8(DropCopy) : e.proposed & DropMove ? u8(DropMove) : u8(0);
            if (!action) return DropIgnore;
            const Rect r = c.contentRect(t);
            const f32 rh = rowHeight(c, t), hh = TableView::headerH(c, t, *st);
            const u32 nr = TableView::rows(*st);
            const i64 gap = std::clamp<i64>(i64(st->first) + i64(std::lround((e.pos.y - r.y - hh) / rh)), 0, nr);
            if (e.phase != DropPhase::Drop) {
                const f32 y = r.y + hh + f32(gap - i64(st->first)) * rh;
                setDropIndicator(c, t, {r.x, y - 1.f, r.w - TableView::kBar, 2.f});
                return action;
            }
            setDropIndicator(c, t, {});
            auto cb = st->dd.insertRows;
            if (!cb(c, t, e.payload->get("text/plain"), u32(gap), action)) return DropIgnore;
            tableModelChanged(c, t);
            if ((st = stateOf(c, t))) TableView::select(c, t, *st, u32(gap));
            return action;
        }
        std::vector<u32> froms;
        for (std::string_view v = e.payload->get("application/x-fuse-table-row"); !v.empty();) {
            const std::size_t k = v.find(',');
            froms.push_back(u32(std::stoul(std::string(v.substr(0, k)))));
            v = k == std::string_view::npos ? std::string_view{} : v.substr(k + 1);
        }
        std::sort(froms.begin(), froms.end());
        froms.erase(std::unique(froms.begin(), froms.end()), froms.end());
        const Rect r = c.contentRect(t);
        const f32 rh = rowHeight(c, t), hh = TableView::headerH(c, t, *st);
        const u32 nr = TableView::rows(*st);
        if (froms.empty() || froms.back() >= nr) return DropIgnore;
        if (e.phase == DropPhase::Move && st->dd.autoScroll)
            dnd::autoScroll(c, t, {r.x, r.y + hh, r.w, r.h - hh}, [](UiContext& c2, NodeHandle v, i32 dir) {
                TableState* s2 = stateOf(c2, v);
                if (!s2) return false;
                const u32 before = s2->first;
                TableView::setFirst(c2, v, *s2, i64(before) + dir);
                return s2->first != before;
            });
        // the gap nearest the pointer (QAbstractItemView AboveItem / BelowItem between rows)
        const i64 gap = std::clamp<i64>(i64(st->first) + i64(std::lround((e.pos.y - r.y - hh) / rh)), 0, nr);
        if (e.phase != DropPhase::Drop) {
            const f32 y = r.y + hh + f32(gap - i64(st->first)) * rh;
            setDropIndicator(c, t, {r.x, y - 1.f, r.w - TableView::kBar, 2.f});
            return DropMove;
        }
        setDropIndicator(c, t, {});
        u32 to = u32(gap);
        for (u32 f : froms) if (f < u32(gap)) --to; // the gap's index once the moved rows are taken out
        bool same = true;
        for (u32 i = 0; i < froms.size(); ++i) same &= froms[i] == to + i;
        if (same) return DropIgnore;
        if (st->dd.moveRows) { auto cb = st->dd.moveRows; cb(c, t, froms, to); }
        st->droppedSelf = true;
        c.invalidate(t);
        TableView::select(c, t, *st, to);
        if (st->sel.explicitSet() && st->sel.behavior == SelectionBehavior::Rows) {
            st->sel.keys.clear();
            for (u32 i = 0; i < froms.size(); ++i) st->sel.keys.insert(to + i);
            st->sel.anchorR = to;
            TableView::changed(c, t);
        }
        return DropMove;
    });
}
void StringTableModel::moveRow(u32 from, u32 to) {
    if (from >= m_rows || to >= m_rows || from == to) return;
    const auto b = m_cells.begin();
    const std::size_t c = m_cols;
    if (from < to) std::rotate(b + from * c, b + (from + 1) * c, b + (to + 1) * c);
    else std::rotate(b + to * c, b + from * c, b + (from + 1) * c);
}
void StringTableModel::moveRows(const std::vector<u32>& rows, u32 to) {
    std::vector<u32> rs(rows);
    std::sort(rs.begin(), rs.end());
    rs.erase(std::unique(rs.begin(), rs.end()), rs.end());
    if (rs.empty() || rs.back() >= m_rows || to + rs.size() > m_rows) return;
    std::vector<std::string> moved, rest;
    std::vector<bool> isMoved(m_rows, false);
    for (u32 r : rs) isMoved[r] = true;
    for (u32 r = 0; r < m_rows; ++r)
        for (u32 k = 0; k < m_cols; ++k) (isMoved[r] ? moved : rest).push_back(std::move(m_cells[std::size_t(r) * m_cols + k]));
    rest.insert(rest.begin() + std::ptrdiff_t(std::size_t(to) * m_cols), std::make_move_iterator(moved.begin()), std::make_move_iterator(moved.end()));
    for (std::size_t i = 0; i < rest.size(); ++i) m_cells[i] = std::move(rest[i]);
}
void StringTableModel::sortBy(u32 col, bool ascending) {
    if (col >= m_cols) return;
    std::vector<u32> order(m_rows);
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        const std::string_view x = cell(ascending ? a : b, col), y = cell(ascending ? b : a, col);
        double nx, ny;
        if (parseNum(x, nx) && parseNum(y, ny)) return nx < ny;
        return x < y;
    });
    std::vector<std::string> sorted;
    sorted.reserve(m_cells.size());
    for (u32 r : order)
        for (u32 c = 0; c < m_cols; ++c) sorted.push_back(std::move(m_cells[std::size_t(r) * m_cols + c]));
    m_cells = std::move(sorted);
}

// ---- table view API ------------------------------------------------------------------------------------------------------
NodeHandle tableView(UiContext& ui, NodeHandle parent, const ItemModel* model, std::string_view id) {
    registerTierB(ui);
    const NodeHandle n = make(ui, "tableview", parent, id);
    if (!ui.alive(n)) return n;
    TableState& s = newState(ui, n);
    s.model = model;
    const u32 nc = TableView::cols(s);
    for (u32 c = 0; c < nc; ++c) s.colW[c] = c + 1 == nc ? 160.f : 120.f;
    for (u32 c = 0; c < kMaxCols; ++c) s.order[c] = u8(c);
    ui.setRange(n, -1.f, 1e9f, 1.f);
    ui.setValue(n, -1.f);
    return n;
}
NodeHandle modelListView(UiContext& ui, NodeHandle parent, const ItemModel* model, std::string_view id) {
    const NodeHandle n = tableView(ui, parent, model, id);
    if (TableState* s = stateOf(ui, n)) {
        s->header = false;
        s->colW[0] = 4096.f; // clipped to the view
        for (u32 c = 1; c < kMaxCols; ++c) s->colW[c] = 0.f;
    }
    return n;
}
void tableModelChanged(UiContext& ui, NodeHandle table) {
    TableState* s = stateOf(ui, table);
    if (!s) return;
    TableView::setFirst(ui, table, *s, s->first);
    if (ui.value(table) >= f32(TableView::rows(*s))) ui.setValue(table, -1.f, true);
    ui.invalidate(table);
}
u32 tableFirstVisible(const UiContext& ui, NodeHandle table) { const TableState* s = stateOf(ui, table); return s ? s->first : 0u; }
f32 tableColumnWidth(const UiContext& ui, NodeHandle table, u32 col) {
    const TableState* s = stateOf(ui, table);
    return s && col < kMaxCols ? s->colW[col] : 0.f;
}
void setTableColumnWidth(UiContext& ui, NodeHandle table, u32 col, f32 px) {
    if (TableState* s = stateOf(ui, table); s && col < kMaxCols) { s->colW[col] = std::max(24.f, px); TableView::setX(ui, table, *s, s->xOff); ui.invalidate(table); }
}
void setTableDelegate(UiContext& ui, NodeHandle table, u32 col, const CellDelegate* delegate) {
    if (TableState* s = stateOf(ui, table); s && col < kMaxCols) { s->delegate[col] = delegate; ui.invalidate(table); }
}

// ---- dock layout save / restore ---------------------------------------------------------------------------------------
namespace {
constexpr std::string_view kSideName[] = {"left", "right", "bottom", "center"};
NodeHandle sidePane(const UiContext& ui, NodeHandle area, DockSide side) { return dockRegion(ui, area, side); }
f32 paneSize(const UiContext& ui, NodeHandle pane) {
    const Length b = ui.computed(pane).basis;
    return b.unit == Unit::Px ? b.value : 0.f;
}
} // namespace

std::string dockSaveLayout(const UiContext& ui, NodeHandle area) {
    std::string out = "fusedock 1\n";
    char b[160];
    for (DockSide s : {DockSide::Left, DockSide::Right, DockSide::Bottom}) {
        std::snprintf(b, sizeof b, "size %s %.0f\n", kSideName[u32(s)].data(), double(paneSize(ui, sidePane(ui, area, s))));
        out += b;
    }
    for (DockSide s : {DockSide::Left, DockSide::Right, DockSide::Bottom, DockSide::Center}) {
        u32 order = 0;
        for (NodeHandle d = ui.firstChild(dockRegion(ui, area, s)); ui.alive(d); d = ui.nextSibling(d)) {
            if (!ui.hasClass(d, "dock")) continue;
            const std::string_view id = ui.idOf(d);
            if (id.empty() || id.find_first_of(" \n") != std::string_view::npos) { ++order; continue; }
            out += "dock " + std::string(id) + " " + std::string(kSideName[u32(s)]) + " " + std::to_string(order++) + " " +
                   (ui.hasClass(d, "dock-closed") ? "closed" : "open") + "\n";
        }
    }
    return out;
}

bool dockRestoreLayout(UiContext& ui, NodeHandle area, std::string_view text) {
    if (!ui.alive(area) || !ui.hasClass(area, "dockarea")) return false;
    struct Entry { NodeHandle dock; DockSide side; u32 order; bool open; };
    std::vector<Entry> docks;
    f32 sizes[3] = {-1.f, -1.f, -1.f};
    auto side = [](std::string_view w, DockSide& out) {
        for (u32 i = 0; i < 4; ++i) if (w == kSideName[i]) { out = DockSide(i); return true; }
        return false;
    };
    std::size_t at = 0;
    bool header = false;
    while (at < text.size()) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view line = text.substr(at, nl - at);
        at = nl + 1;
        if (line.empty()) continue;
        std::vector<std::string_view> w;
        for (std::size_t p = 0; p <= line.size();) {
            std::size_t q = line.find(' ', p);
            if (q == std::string_view::npos) q = line.size();
            if (q > p) w.push_back(line.substr(p, q - p));
            p = q + 1;
        }
        if (!header) {
            if (w.size() != 2 || w[0] != "fusedock" || w[1] != "1") return false;
            header = true;
            continue;
        }
        DockSide sd;
        if (w[0] == "size" && w.size() == 3 && side(w[1], sd) && sd != DockSide::Center) {
            double v;
            if (!parseNum(w[2], v) || !(v >= 0.0 && v <= 16384.0)) return false;
            sizes[u32(sd)] = f32(v);
        } else if (w[0] == "dock" && w.size() == 5 && side(w[2], sd) && (w[4] == "open" || w[4] == "closed")) {
            double o;
            if (!parseNum(w[3], o) || !(o >= 0.0 && o < 1e6)) return false;
            const NodeHandle d = ui.find(w[1]);
            if (!ui.alive(d) || !ui.hasClass(d, "dock") || areaOf(ui, d) != area) continue; // unknown: ignored
            docks.push_back({d, sd, u32(o), w[4] == "open"});
        } else {
            return false;
        }
    }
    if (!header) return false;
    std::stable_sort(docks.begin(), docks.end(), [](const Entry& a, const Entry& b) {
        return a.side != b.side ? u32(a.side) < u32(b.side) : a.order < b.order;
    });
    for (const Entry& e : docks) { // appended in order: each region ends up in saved order (unnamed docks first)
        ui.moveTo(e.dock, dockRegion(ui, area, e.side));
        if (e.open) dockTo(ui, e.dock, e.side);
        else dockClose(ui, e.dock);
    }
    for (u32 i = 0; i < 3; ++i)
        if (sizes[i] >= 0.f) setBasis(ui, sidePane(ui, area, DockSide(i)), sizes[i]);
    refreshDock(ui, area);
    return true;
}

} // namespace fuse::ui::widgets

namespace fuse::ui::widgets {
void setTableColumnsMovable(UiContext& ui, NodeHandle table, bool on) { if (TableState* s = stateOf(ui, table)) s->movable = on; }
void tableMoveColumn(UiContext& ui, NodeHandle table, u32 fromVisual, u32 toVisual) {
    if (TableState* s = stateOf(ui, table)) TableView::moveSection(ui, table, *s, fromVisual, toVisual);
}
u32 tableLogicalColumn(const UiContext& ui, NodeHandle table, u32 visual) {
    const TableState* s = stateOf(ui, table);
    return s && visual < TableView::cols(*s) ? s->order[visual] : visual;
}
u32 tableVisualColumn(const UiContext& ui, NodeHandle table, u32 logical) {
    const TableState* s = stateOf(ui, table);
    return s ? TableView::visualOf(*s, logical) : logical;
}
void setTableColumnHidden(UiContext& ui, NodeHandle table, u32 col, bool hidden) {
    TableState* s = stateOf(ui, table);
    if (!s || col >= kMaxCols || hidden == (s->hiddenW[col] > 0.f)) return;
    if (hidden) { s->hiddenW[col] = std::max(1.f, s->colW[col]); s->colW[col] = 0.f; }
    else { s->colW[col] = s->hiddenW[col]; s->hiddenW[col] = 0.f; }
    if (hidden && s->curCol == col) {
        i64 nc = TableView::stepCol(*s, col, +1);
        if (nc < 0) nc = TableView::stepCol(*s, col, -1);
        if (nc >= 0) s->curCol = u32(nc);
    }
    TableView::setX(ui, table, *s, s->xOff);
    ui.invalidate(table);
}
bool tableColumnHidden(const UiContext& ui, NodeHandle table, u32 col) {
    const TableState* s = stateOf(ui, table);
    return s && col < kMaxCols && s->hiddenW[col] > 0.f;
}
} // namespace fuse::ui::widgets
