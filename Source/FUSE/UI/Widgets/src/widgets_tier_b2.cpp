// FUSE UI Tier B (plan §5), second half: splitters, dock area, item models, virtualised table view.
#include "fuse/ui/widgets/widgets.hpp"
#include "fuse/text/from_chars.hpp"
#include "widgets_internal.hpp"

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
    static constexpr bool kCapturesDrag = true; // touch: the drag resizes the panes
    static constexpr A11yRole kRole = A11yRole::Separator;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {6.f, 6.f}; }
    static bool stacked(const UiContext& ui, NodeHandle n) { return ui.value(n) > 0.5f; }
    static Cursor cursor(UiContext& ui, NodeHandle n, Vec2) { return stacked(ui, n) ? Cursor::ResizeV : Cursor::ResizeH; }
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
        const f32 na = std::clamp(pos - start, kMinPane, total - kMinPane);
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
/// Per-node state pool keyed by the node's user data (slot + 1); a slot is reused once its node is gone.
template <class S>
struct Pool {
    struct Slot { NodeHandle node; std::unique_ptr<S> s; };
    std::vector<Slot> slots;
    S* get(const UiContext& ui, NodeHandle n) {
        const u64 k = ui.alive(n) ? ui.userData(n) : 0;
        if (k == 0 || k > slots.size() || slots[k - 1].node != n) return nullptr;
        return slots[k - 1].s.get();
    }
    S& create(UiContext& ui, NodeHandle n) {
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (!ui.alive(slots[i].node) || ui.userData(slots[i].node) != i + 1) {
                slots[i] = {n, std::make_unique<S>()};
                ui.setUserData(n, i + 1);
                return *slots[i].s;
            }
        slots.push_back({n, std::make_unique<S>()});
        ui.setUserData(n, slots.size());
        return *slots.back().s;
    }
};
// A dock widget: its area (a floating dock has left the area's subtree), its tab strip while tabbed, the floating rect.
struct DockState {
    NodeHandle area, tabs;
    bool floating = false, dragging = false;
    Rect floatRect;
    Vec2 grab; ///< pointer offset from the dock's top-left while its title is dragged
};
// A tab strip: the docks tabbed together, in tab order, and the one whose body shows.
struct DockTabsState {
    std::vector<NodeHandle> members;
    NodeHandle current;
};
Pool<DockState>& dockPool() { static Pool<DockState> p; return p; }
Pool<DockTabsState>& tabsPool() { static Pool<DockTabsState> p; return p; }
DockState* dockState(const UiContext& ui, NodeHandle d) { return ui.alive(d) && ui.hasClass(d, "dock") ? dockPool().get(ui, d) : nullptr; }
DockTabsState* tabsState(const UiContext& ui, NodeHandle t) { return ui.alive(t) && ui.hasClass(t, "docktabs") ? tabsPool().get(ui, t) : nullptr; }
void moveFloat(UiContext& ui, NodeHandle dock, DockState& s, Vec2 at);
void raiseFloat(UiContext& ui, NodeHandle dock);

struct DockTitle { // the title strip of a dock widget; text = title. Close box at the right end. Drags a floating dock.
    static constexpr std::string_view kName = "docktitle";
    static constexpr bool kCapturesDrag = true; // touch: the drag moves a floating dock
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
        UiContext& ui = ec.ui;
        const NodeHandle dock = ui.parent(ec.node);
        DockState* s = dockState(ui, dock);
        if (e.type == EventType::PointerDown && s) { // a floating dock comes to the front and follows the pointer
            s->dragging = s->floating && e.button == MouseButton::Left && !closeBox(ui.rect(ec.node)).contains(e.pos);
            if (s->dragging) {
                const Rect r = ui.rect(dock);
                s->grab = {e.pos.x - r.x, e.pos.y - r.y};
                raiseFloat(ui, dock);
            }
            return false;
        }
        if (e.type == EventType::PointerMove) {
            if (s && s->dragging && ui.pressed() == ec.node) { moveFloat(ui, dock, *s, {e.pos.x - s->grab.x, e.pos.y - s->grab.y}); return true; }
            ui.invalidate(ec.node);
            return false;
        }
        if (e.type == EventType::PointerUp && s) { s->dragging = false; return false; }
        if (e.type == EventType::Click && closeBox(ui.rect(ec.node)).contains(e.pos)) { dockClose(ui, dock); return true; }
        return false;
    }
};

// The tab strip of docks tabbed together (dockTabify): one tab per open member, the current one highlighted; a close
// box at the right end closes the current dock. value() = index of the current dock among the members.
struct DockTabs {
    static constexpr std::string_view kName = "docktabs";
    static constexpr A11yRole kRole = A11yRole::Tab;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static constexpr f32 kTabPad = 20.f;
    static std::string_view title(const UiContext& ui, NodeHandle dock) { return ui.text(ui.firstChild(dock)); }
    static bool open(const UiContext& ui, NodeHandle dock) { return ui.alive(dock) && !ui.hasClass(dock, "dock-closed"); }
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        f32 w = rowHeight(ui, n);
        if (const DockTabsState* s = tabsState(ui, n))
            for (NodeHandle m : s->members)
                if (open(ui, m)) w += std::ceil(ui.measureString(title(ui, m), ui.computed(n).fontSize)) + kTabPad;
        return {w, rowHeight(ui, n) - 2.f};
    }
    static Rect closeBox(const Rect& r) { return {r.right() - r.h, r.y, r.h, r.h}; }
    /// Tab rect of `dock` (empty when it has no tab). Tabs share the strip left of the close box, shrunk to fit.
    static Rect tabRect(UiContext& ui, NodeHandle n, const DockTabsState& s, NodeHandle dock) {
        const Rect r = ui.rect(n);
        const f32 fs = ui.computed(n).fontSize;
        f32 total = 0.f;
        for (NodeHandle m : s.members)
            if (open(ui, m)) total += std::ceil(ui.measureString(title(ui, m), fs)) + kTabPad;
        const f32 avail = std::max(0.f, r.w - r.h), k = total > avail && total > 0.f ? avail / total : 1.f;
        f32 x = r.x;
        for (NodeHandle m : s.members) {
            if (!open(ui, m)) continue;
            const f32 w = (std::ceil(ui.measureString(title(ui, m), fs)) + kTabPad) * k;
            if (m == dock) return {x, r.y, w, r.h};
            x += w;
        }
        return {};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        const DockTabsState* s = tabsState(ui, pc.node);
        if (!s) return;
        const Rect r = pc.rect;
        const Color acc = pc.tint(pc.style.background2.a > 0.f ? pc.style.background2 : Color::rgb(0x3D8BFD));
        dl.rect(r, pc.tint(Color::rgb(0x22222A)), 3.f);
        for (NodeHandle m : s->members) {
            const Rect t = tabRect(ui, pc.node, *s, m);
            if (t.empty()) continue;
            const bool cur = m == s->current, hot = ui.hovered() == pc.node && t.contains(ui.pointer());
            if (cur || hot) dl.rect(t, pc.tint(Color::rgb(cur ? 0x34353E : 0x2A2A33)), 3.f);
            if (cur) dl.rect({t.x + 2.f, t.bottom() - 2.f, t.w - 4.f, 2.f}, acc, 1.f);
            dl.pushClip(t);
            ui.paintString(dl, title(ui, m), {t.x + kTabPad * 0.5f, t.y, t.w - kTabPad * 0.5f, t.h},
                           pc.tint(cur ? pc.style.color : pc.style.color.withAlpha(pc.style.color.a * 0.65f)), pc.style.fontSize,
                           TextAlignment::Start, pc.node.index);
            dl.popClip();
        }
        const Rect x = closeBox(r);
        const Color c = pc.tint(ui.hovered() == pc.node && x.contains(ui.pointer()) ? Color::rgb(0xFF6B6B) : Color::rgb(0x9A9AA6));
        const f32 mg = x.h * 0.32f;
        dl.line({x.x + mg, x.y + mg}, {x.right() - mg, x.bottom() - mg}, c, 1.5f);
        dl.line({x.right() - mg, x.y + mg}, {x.x + mg, x.bottom() - mg}, c, 1.5f);
        if ((pc.states & kStateFocus) && ui.focusVisible()) dl.roundedRect(r, {}, {3.f, 3.f, 3.f, 3.f}, acc.withAlpha(acc.a * 0.6f), 1.f);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        const DockTabsState* s = tabsState(ui, ec.node);
        if (!s) return false;
        if (e.type == EventType::PointerMove) { ui.invalidate(ec.node); return false; }
        if (e.type == EventType::Click) {
            if (closeBox(ui.rect(ec.node)).contains(e.pos)) { dockClose(ui, s->current); return true; }
            const std::vector<NodeHandle> members = s->members; // raising edits the state
            for (NodeHandle m : members)
                if (tabRect(ui, ec.node, *s, m).contains(e.pos)) { dockRaise(ui, m); break; }
            return true;
        }
        if (e.type == EventType::KeyDown && (e.key == Key::Left || e.key == Key::Right)) { // previous / next open tab
            const i32 n = i32(s->members.size()), d = e.key == Key::Right ? 1 : -1;
            i32 at = 0;
            for (i32 i = 0; i < n; ++i) if (s->members[i] == s->current) at = i;
            for (i32 i = 1; i < n; ++i) {
                const NodeHandle m = s->members[std::size_t(((at + d * i) % n + n) % n)];
                if (open(ui, m)) { dockRaise(ui, m); break; }
            }
            return true;
        }
        return false;
    }
};

NodeHandle nthChild(const UiContext& ui, NodeHandle n, u32 i) {
    NodeHandle c = ui.firstChild(n);
    while (i-- && ui.alive(c)) c = ui.nextSibling(c);
    return c;
}
bool anyShown(const UiContext& ui, NodeHandle region) {
    for (NodeHandle c = ui.firstChild(region); ui.alive(c); c = ui.nextSibling(c)) {
        if (ui.hasClass(c, "docktabs")) continue; // a tab strip follows its docks
        // dock widgets: the closed class is authoritative (computed visibility is stale right after dockTo re-shows one)
        if (ui.hasClass(c, "dock") ? !ui.hasClass(c, "dock-closed") : ui.computed(c).visible) return true;
    }
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
    if (const DockState* s = dockState(ui, dock); s && ui.alive(s->area)) return s->area;
    NodeHandle n = dock;
    while (ui.alive(n) && !ui.hasClass(n, "dockarea")) n = ui.parent(n);
    return n;
}
void setPx(UiContext& ui, NodeHandle n, Prop p, f32 v) { ui.set(n, p, PropValue::ofLength(Length::px(std::round(v)))); }

/// Re-derives a tab group's display from its state: only the current open member shows (titles are replaced by the
/// strip), a closed current hands over to the first open member, a group of one dissolves.
void applyTabs(UiContext& ui, NodeHandle tabs) {
    DockTabsState* t = tabsState(ui, tabs);
    if (!t) return;
    std::erase_if(t->members, [&](NodeHandle m) { const DockState* d = dockState(ui, m); return !d || d->tabs != tabs; });
    if (t->members.size() < 2) {
        for (NodeHandle m : t->members) {
            dockState(ui, m)->tabs = {};
            showNode(ui, ui.firstChild(m), true);
            showNode(ui, m, DockTabs::open(ui, m));
        }
        t->members.clear();
        ui.destroy(tabs);
        return;
    }
    if (!DockTabs::open(ui, t->current) || std::find(t->members.begin(), t->members.end(), t->current) == t->members.end()) {
        t->current = {};
        for (NodeHandle m : t->members)
            if (DockTabs::open(ui, m)) { t->current = m; break; }
    }
    i32 index = -1;
    for (std::size_t i = 0; i < t->members.size(); ++i) {
        const NodeHandle m = t->members[i];
        showNode(ui, ui.firstChild(m), false);
        showNode(ui, m, m == t->current);
        if (m == t->current) index = i32(i);
    }
    showNode(ui, tabs, index >= 0);
    ui.setText(tabs, {}); // re-measure (the open tabs changed)
    ui.invalidate(tabs);
    ui.setValue(tabs, f32(index), true);
}
/// Takes a dock out of its tab group and out of the floating state (it stays where it is in the tree).
void detachDock(UiContext& ui, NodeHandle dock) {
    DockState* s = dockState(ui, dock);
    if (!s) return;
    if (const NodeHandle tabs = s->tabs; tabs.valid()) {
        s->tabs = {};
        showNode(ui, ui.firstChild(dock), true);
        showNode(ui, dock, DockTabs::open(ui, dock));
        applyTabs(ui, tabs);
    }
    if (s->floating) {
        s->floating = s->dragging = false;
        ui.removeClass(dock, "dock-floating");
        for (const char* p : {"left", "top", "width", "height"}) ui.setCss(dock, p, "auto");
    }
}
void moveFloat(UiContext& ui, NodeHandle dock, DockState& s, Vec2 at) {
    // the title strip stays reachable: at least 40 px of it inside the viewport, never above the top edge
    const Size vp = ui.viewport();
    s.floatRect.x = std::round(std::clamp(at.x, 40.f - s.floatRect.w, std::max(40.f - s.floatRect.w, vp.w - 40.f)));
    s.floatRect.y = std::round(std::clamp(at.y, 0.f, std::max(0.f, vp.h - 24.f)));
    setPx(ui, dock, Prop::Left, s.floatRect.x);
    setPx(ui, dock, Prop::Top, s.floatRect.y);
}
/// Puts a floating dock above the other floating docks (popups and toasts created later stay above it).
void raiseFloat(UiContext& ui, NodeHandle dock) {
    NodeHandle last;
    for (NodeHandle c = ui.firstChild(ui.parent(dock)); ui.alive(c); c = ui.nextSibling(c))
        if (ui.hasClass(c, "dock-floating")) last = c;
    if (last.valid() && last != dock) ui.moveTo(dock, ui.parent(dock), ui.nextSibling(last));
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
    // touch: a drag on the scroll bar / a column edge stays with the table, elsewhere rows follow the finger
    static TouchDrag touchDrag(UiContext& ui, NodeHandle n, Vec2) {
        const TableState* s = stateOf(ui, n);
        if (!s) return {};
        if (s->dragScroll || s->resizing >= 0) return {TouchDrag::Mode::Capture, {}, {}};
        if (rows(*s) <= visible(ui, n, *s)) return {};
        return {TouchDrag::Mode::Wheel, {0.f, rowHeight(ui, n)}, {0.f, 1.f / 3.f}};
    }
    static constexpr A11yRole kRole = A11yRole::List;
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
    static u32 visible(const UiContext& ui, NodeHandle n, const TableState& s) {
        return std::max(1u, u32((ui.contentRect(n).h - headerH(ui, n, s)) / rowHeight(ui, n)));
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
        ui.setValue(n, f32(row), true);
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
            f32 x = c.x;
            for (u32 k = 0; k < nc; ++k) {
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
            if (i32(row) == sel) dl.rect({c.x, y, c.w, rh}, acc.withAlpha(acc.a * 0.55f));
            else if (row & 1u) dl.rect({c.x, y, c.w, rh}, pc.tint(Color::rgb(0xFFFFFF, 0.035f)));
            f32 x = c.x;
            for (u32 k = 0; k < nc; ++k) {
                if (s->colW[k] <= 0.f) continue;
                const std::string_view d = s->model->data(s->model->user, row, k, s->scratch);
                dl.pushClip({x, y, s->colW[k] - 2.f, rh});
                if (const CellDelegate* dg = s->delegate[k]; dg && dg->paint)
                    dg->paint(dg->user, pc, dl, {x + 6.f, y, s->colW[k] - 12.f, rh}, row, k, d, i32(row) == sel);
                else
                    ui.paintString(dl, d, {x + 6.f, y, s->colW[k] - 12.f, rh}, k ? dim : text, fs, TextAlignment::Start, pc.node.index);
                dl.popClip();
                x += s->colW[k];
            }
        }
        Rect tr, th;
        if (barGeom(ui, pc.node, *s, tr, th)) {
            dl.rect(tr, pc.tint(Color::rgb(0x000000, 0.15f)), kBar * 0.5f);
            dl.rect(th, pc.tint(Color::rgb(s->dragScroll ? 0xB4B4C0 : 0x8A8A96, 0.85f)), kBar * 0.5f);
        }
        if (pc.states & kStateFocus) dl.roundedRect(pc.rect, {}, {}, acc.withAlpha(acc.a * 0.6f), 1.f);
        dl.popClip();
    }
    /// Column edges of the header row resize (and a running column resize keeps the cursor).
    static Cursor cursor(UiContext& ui, NodeHandle n, Vec2 p) {
        const TableState* s = stateOf(ui, n);
        if (!s) return Cursor::Auto;
        if (s->resizing >= 0) return Cursor::ResizeH;
        const Rect c = ui.contentRect(n);
        if (s->dragScroll || p.y < c.y || p.y >= c.y + headerH(ui, n, *s)) return Cursor::Auto;
        f32 x = c.x;
        for (u32 k = 0; k < cols(*s); ++k) {
            x += s->colW[k];
            if (std::abs(p.x - x) <= 4.f) return Cursor::ResizeH;
        }
        return Cursor::Auto;
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
            Rect tr, th;
            if (barGeom(ui, ec.node, *s, tr, th) && tr.contains(e.pos)) {
                s->dragScroll = true;
                s->grab = th.contains(e.pos) ? e.pos.y - th.y : th.h * 0.5f;
                Event mv = e;
                mv.type = EventType::PointerMove;
                return onEvent(ec, mv);
            }
            if (e.pos.y < c.y + hh) {
                f32 x = c.x;
                for (u32 k = 0; k < cols(*s); ++k) {
                    x += s->colW[k];
                    if (std::abs(e.pos.x - x) <= 4.f) { s->resizing = i32(k); s->grab = x - s->colW[k]; return true; }
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
            if (row < i64(rows(*s))) select(ui, ec.node, *s, row);
            return true;
        }
        case EventType::PointerMove:
            if (ui.pressed() != ec.node && !s->dragScroll) return false;
            if (s->resizing >= 0) {
                s->colW[s->resizing] = std::max(24.f, std::round(e.pos.x - s->grab));
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
            if (s->dragScroll) ui.invalidate(ec.node);
            s->resizing = -1;
            s->dragScroll = false;
            return false;
        case EventType::Wheel:
            setFirst(ui, ec.node, *s, i64(s->first) - i64(std::lround(e.wheel.y * 3.f)));
            return true;
        case EventType::KeyDown: {
            const i64 cur = i64(ui.value(ec.node)), vis = visible(ui, ec.node, *s);
            switch (e.key) {
            case Key::Down: select(ui, ec.node, *s, cur + 1); return true;
            case Key::Up: select(ui, ec.node, *s, cur < 0 ? 0 : cur - 1); return true;
            case Key::PageDown: select(ui, ec.node, *s, cur + vis); return true;
            case Key::PageUp: select(ui, ec.node, *s, cur - vis); return true;
            case Key::Home: select(ui, ec.node, *s, 0); return true;
            case Key::End: select(ui, ec.node, *s, i64(rows(*s)) - 1); return true;
            case Key::Enter:
                if (cur >= 0) ui.emit(ec.node, SignalKind::Submitted, {.value = f32(cur)});
                return true;
            default: return false;
            }
        }
        case EventType::DoubleClick: { // double click on a row activates it, like Enter
            const i64 cur = i64(ui.value(ec.node));
            if (e.pos.y >= c.y + hh && cur >= 0 && cur == i64(s->first) + i64((e.pos.y - c.y - hh) / rh))
                ui.emit(ec.node, SignalKind::Submitted, {.value = f32(cur)});
            return true;
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
    r.registerWidget<DockTabs>();
    r.registerWidget<TableView>();
}

bool parseNum(std::string_view s, double& out) {
    const auto [p, ec] = fuse::text::fromChars(s.data(), s.data() + s.size(), out);
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
    dockPool().create(ui, d).area = area;
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
    detachDock(ui, dock);
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
    if (const DockState* s = dockState(ui, dock)) applyTabs(ui, s->tabs); // the next open tab takes over
    refreshDock(ui, areaOf(ui, dock));
    ui.emit(dock, SignalKind::Toggled, {.flag = false});
}

// ---- tabbed and floating docks ---------------------------------------------------------------------------------------
namespace {
/// dockTabify; `reopen` false keeps a closed dock closed (layout restore).
void joinTabs(UiContext& ui, NodeHandle dock, NodeHandle withDock, bool reopen) {
    DockState* ds = dockState(ui, dock);
    DockState* ws = dockState(ui, withDock);
    if (!ds || !ws || dock == withDock || ws->floating) return;
    const NodeHandle area = areaOf(ui, withDock);
    if (!ui.alive(area) || areaOf(ui, dock) != area) return;
    if (ds->tabs.valid() && ds->tabs == ws->tabs) return;
    detachDock(ui, dock);
    const NodeHandle region = ui.parent(withDock);
    if (!tabsState(ui, ws->tabs)) { // first pair: the strip goes right above `withDock`
        const NodeHandle strip = make(ui, "docktabs", region);
        if (!ui.alive(strip)) return;
        ui.addClass(strip, "docktabs");
        ui.moveTo(strip, region, withDock);
        ui.setCss(strip, "flex-shrink", "0");
        ui.setRange(strip, -1.f, 4096.f, 1.f);
        ui.setValue(strip, -1.f);
        DockTabsState& t = tabsPool().create(ui, strip);
        t.members.push_back(withDock);
        t.current = withDock;
        ws->tabs = strip;
    }
    DockTabsState& t = *tabsState(ui, ws->tabs);
    const NodeHandle after = ui.nextSibling(t.members.back());
    t.members.push_back(dock);
    ds->tabs = ws->tabs;
    if (after != dock) ui.moveTo(dock, region, after);
    const bool wasClosed = reopen && ui.hasClass(dock, "dock-closed");
    if (reopen) ui.removeClass(dock, "dock-closed");
    applyTabs(ui, ws->tabs);
    refreshDock(ui, area);
    if (wasClosed) ui.emit(dock, SignalKind::Toggled, {.flag = true});
}
} // namespace
void dockTabify(UiContext& ui, NodeHandle dock, NodeHandle withDock) { joinTabs(ui, dock, withDock, true); }
void dockRaise(UiContext& ui, NodeHandle dock) {
    DockState* s = dockState(ui, dock);
    if (!s) return;
    DockTabsState* t = tabsState(ui, s->tabs);
    if (!t) {
        if (s->floating) raiseFloat(ui, dock);
        return;
    }
    const bool wasClosed = ui.hasClass(dock, "dock-closed");
    ui.removeClass(dock, "dock-closed");
    t->current = dock;
    applyTabs(ui, s->tabs);
    refreshDock(ui, areaOf(ui, dock));
    if (wasClosed) ui.emit(dock, SignalKind::Toggled, {.flag = true});
}
NodeHandle dockCurrent(const UiContext& ui, NodeHandle dock) {
    const DockState* s = dockState(ui, dock);
    if (!s) return {};
    const DockTabsState* t = tabsState(ui, s->tabs);
    return t ? t->current : dock;
}
NodeHandle dockTabBar(const UiContext& ui, NodeHandle dock) {
    const DockState* s = dockState(ui, dock);
    return s && tabsState(ui, s->tabs) ? s->tabs : NodeHandle{};
}
Rect dockTabRect(UiContext& ui, NodeHandle dock) {
    const DockState* s = dockState(ui, dock);
    const DockTabsState* t = s ? tabsState(ui, s->tabs) : nullptr;
    return t ? DockTabs::tabRect(ui, s->tabs, *t, dock) : Rect{};
}
void dockFloat(UiContext& ui, NodeHandle dock, Rect rect) {
    DockState* s = dockState(ui, dock);
    const NodeHandle area = areaOf(ui, dock);
    if (!s || !ui.alive(area)) return;
    const NodeHandle layer = overlayLayer(ui); // may create the layer: `s` stays valid (pool storage, not node storage)
    if (s->tabs.valid()) detachDock(ui, dock);
    s->area = area;
    s->floating = true;
    s->floatRect = {0.f, 0.f, std::round(std::max(rect.w, 80.f)), std::round(std::max(rect.h, 60.f))};
    if (ui.parent(dock) != layer) ui.moveTo(dock, layer);
    ui.addClass(dock, "dock-floating");
    setPx(ui, dock, Prop::Width, s->floatRect.w);
    setPx(ui, dock, Prop::Height, s->floatRect.h);
    moveFloat(ui, dock, *s, {rect.x, rect.y});
    raiseFloat(ui, dock);
    const bool wasClosed = ui.hasClass(dock, "dock-closed");
    ui.removeClass(dock, "dock-closed");
    showNode(ui, dock, true);
    refreshDock(ui, area);
    if (wasClosed) ui.emit(dock, SignalKind::Toggled, {.flag = true});
}
bool dockFloating(const UiContext& ui, NodeHandle dock) {
    const DockState* s = dockState(ui, dock);
    return s && s->floating;
}
Rect dockFloatRect(const UiContext& ui, NodeHandle dock) {
    const DockState* s = dockState(ui, dock);
    return s && s->floating ? s->floatRect : Rect{};
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
const ItemModel* tableModel(const UiContext& ui, NodeHandle table) {
    const TableState* s = stateOf(ui, table);
    return s ? s->model : nullptr;
}
Rect tableRowRect(const UiContext& ui, NodeHandle table, u32 row) {
    const TableState* s = stateOf(ui, table);
    if (!s) return {};
    const Rect c = ui.contentRect(table);
    const f32 rh = rowHeight(ui, table);
    return {c.x, c.y + TableView::headerH(ui, table, *s) + (f32(row) - f32(s->first)) * rh, c.w, rh};
}
i32 tableRowAt(const UiContext& ui, NodeHandle table, Vec2 p) {
    const TableState* s = stateOf(ui, table);
    if (!s) return -1;
    const Rect c = ui.contentRect(table);
    const f32 top = c.y + TableView::headerH(ui, table, *s);
    if (!c.contains(p) || p.y < top || p.x >= c.right() - TableView::kBar) return -1;
    const i64 row = i64(s->first) + i64((p.y - top) / rowHeight(ui, table));
    return row < i64(TableView::rows(*s)) ? i32(row) : -1;
}
u32 tableFirstVisible(const UiContext& ui, NodeHandle table) { const TableState* s = stateOf(ui, table); return s ? s->first : 0u; }
f32 tableColumnWidth(const UiContext& ui, NodeHandle table, u32 col) {
    const TableState* s = stateOf(ui, table);
    return s && col < kMaxCols ? s->colW[col] : 0.f;
}
void setTableColumnWidth(UiContext& ui, NodeHandle table, u32 col, f32 px) {
    if (TableState* s = stateOf(ui, table); s && col < kMaxCols) { s->colW[col] = std::max(24.f, px); ui.invalidate(table); }
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
    // tab groups: one line per named member, in tab order (only written when docks are tabbed)
    auto named = [&](NodeHandle d) {
        const std::string_view id = ui.idOf(d);
        return !id.empty() && id.find_first_of(" \n") == std::string_view::npos;
    };
    u32 group = 0;
    for (DockSide s : {DockSide::Left, DockSide::Right, DockSide::Bottom, DockSide::Center})
        for (NodeHandle t = ui.firstChild(dockRegion(ui, area, s)); ui.alive(t); t = ui.nextSibling(t)) {
            const DockTabsState* ts = tabsState(ui, t);
            if (!ts) continue;
            for (std::size_t i = 0; i < ts->members.size(); ++i)
                if (const NodeHandle d = ts->members[i]; named(d))
                    out += "tab " + std::string(ui.idOf(d)) + " " + std::to_string(group) + " " + std::to_string(i) +
                           (d == ts->current ? " current\n" : " back\n");
            ++group;
        }
    // floating docks, back to front (only written when docks float)
    NodeHandle layer;
    for (const auto& slot : dockPool().slots)
        if (const DockState* d = dockState(ui, slot.node); d && d == slot.s.get() && d->floating && d->area == area) layer = ui.parent(slot.node);
    for (NodeHandle d = ui.firstChild(layer); ui.alive(d); d = ui.nextSibling(d)) {
        const DockState* st = dockState(ui, d);
        if (!st || !st->floating || st->area != area || !named(d)) continue;
        std::snprintf(b, sizeof b, " %.0f %.0f %.0f %.0f ", double(st->floatRect.x), double(st->floatRect.y), double(st->floatRect.w),
                      double(st->floatRect.h));
        out += "float " + std::string(ui.idOf(d)) + b + (ui.hasClass(d, "dock-closed") ? "closed" : "open") + "\n";
    }
    return out;
}

bool dockRestoreLayout(UiContext& ui, NodeHandle area, std::string_view text) {
    if (!ui.alive(area) || !ui.hasClass(area, "dockarea")) return false;
    struct Entry { NodeHandle dock; DockSide side; u32 order; bool open; };
    struct TabEntry { NodeHandle dock; u32 group, index; bool current; };
    struct FloatEntry { NodeHandle dock; Rect rect; bool open; };
    std::vector<Entry> docks;
    std::vector<TabEntry> tabs;
    std::vector<FloatEntry> floats;
    auto dockNamed = [&](std::string_view id) {
        const NodeHandle d = ui.find(id);
        return ui.alive(d) && ui.hasClass(d, "dock") && areaOf(ui, d) == area ? d : NodeHandle{};
    };
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
            const NodeHandle d = dockNamed(w[1]);
            if (!d.valid()) continue; // unknown: ignored
            docks.push_back({d, sd, u32(o), w[4] == "open"});
        } else if (w[0] == "tab" && w.size() == 5 && (w[4] == "current" || w[4] == "back")) {
            double g, i;
            if (!parseNum(w[2], g) || !parseNum(w[3], i) || !(g >= 0.0 && g < 1e6) || !(i >= 0.0 && i < 1e6)) return false;
            if (const NodeHandle d = dockNamed(w[1]); d.valid()) tabs.push_back({d, u32(g), u32(i), w[4] == "current"});
        } else if (w[0] == "float" && w.size() == 7 && (w[6] == "open" || w[6] == "closed")) {
            double v[4];
            for (u32 k = 0; k < 4; ++k)
                if (!parseNum(w[2 + k], v[k]) || !(v[k] >= (k < 2 ? -1e6 : 1.0) && v[k] <= (k < 2 ? 1e6 : 16384.0))) return false;
            if (const NodeHandle d = dockNamed(w[1]); d.valid())
                floats.push_back({d, {f32(v[0]), f32(v[1]), f32(v[2]), f32(v[3])}, w[6] == "open"});
        } else {
            return false;
        }
    }
    if (!header) return false;
    std::stable_sort(docks.begin(), docks.end(), [](const Entry& a, const Entry& b) {
        return a.side != b.side ? u32(a.side) < u32(b.side) : a.order < b.order;
    });
    for (const Entry& e : docks) { // appended in order: each region ends up in saved order (unnamed docks first)
        detachDock(ui, e.dock); // a docked entry leaves its tab group / floating state; "tab" lines regroup below
        ui.moveTo(e.dock, dockRegion(ui, area, e.side));
        if (e.open) dockTo(ui, e.dock, e.side);
        else dockClose(ui, e.dock);
    }
    for (const FloatEntry& e : floats) { // back to front
        const bool wasOpen = !ui.hasClass(e.dock, "dock-closed");
        if (!e.open && !wasOpen) ui.removeClass(e.dock, "dock-closed"); // floats without a spurious re-open signal ...
        dockFloat(ui, e.dock, e.rect);
        if (!e.open && !wasOpen) { ui.addClass(e.dock, "dock-closed"); showNode(ui, e.dock, false); } // ... and stays closed
        else if (!e.open) dockClose(ui, e.dock);
    }
    std::stable_sort(tabs.begin(), tabs.end(), [](const TabEntry& a, const TabEntry& b) {
        return a.group != b.group ? a.group < b.group : a.index < b.index;
    });
    for (std::size_t i = 0; i < tabs.size();) {
        std::size_t j = i + 1;
        while (j < tabs.size() && tabs[j].group == tabs[i].group) ++j;
        if (dockFloating(ui, tabs[i].dock)) dockTo(ui, tabs[i].dock, DockSide::Left); // a group needs a docked first member
        for (std::size_t k = i + 1; k < j; ++k) joinTabs(ui, tabs[k].dock, tabs[i].dock, false);
        for (std::size_t k = i; k < j; ++k)
            if (tabs[k].current && !ui.hasClass(tabs[k].dock, "dock-closed")) dockRaise(ui, tabs[k].dock);
        i = j;
    }
    for (u32 i = 0; i < 3; ++i)
        if (sizes[i] >= 0.f) setBasis(ui, sidePane(ui, area, DockSide(i)), sizes[i]);
    refreshDock(ui, area);
    return true;
}

} // namespace fuse::ui::widgets
