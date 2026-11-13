// FUSE UI Tier B (plan §5), second half: splitters, dock area, StringTableModel (the table / list / tree view is widgets_modelview.cpp).
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
        const Color c = pc.tint(hot ? themed(pc, tok::accent, 0x3D8BFD) : themed(pc, tok::track, 0x5A5A66));
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
// Dock drag and drop (defined below the dock helpers): a press on a title bar or a tab arms a drag, moving past
// kDockDragSlop tears the dock off / shows the drop zones, the release drops, Escape cancels.
constexpr f32 kDockDragSlop = 8.f;
void dragArm(UiContext& ui, NodeHandle dock, NodeHandle source, Vec2 at);
bool dragMove(UiContext& ui, NodeHandle source, Vec2 at);
bool dragRelease(UiContext& ui, NodeHandle source, bool cancel);
bool dragCancel(UiContext& ui);
bool dragSwallowClick(const UiContext& ui, NodeHandle source);
bool dragActive(const UiContext& ui);

struct DockTitle { // the title strip of a dock widget; text = title. Close box at the right end. Drags a floating dock.
    static constexpr std::string_view kName = "docktitle";
    static constexpr bool kCapturesDrag = true; // touch: the drag moves a floating dock
    static constexpr A11yRole kRole = A11yRole::Text;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) { return {std::ceil(ui.measureString(ui.text(n), ui.computed(n).fontSize)) + 24.f, rowHeight(ui, n) - 2.f}; }
    static Rect closeBox(const Rect& r) { return {r.right() - r.h, r.y, r.h, r.h}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect r = pc.rect;
        dl.rect(r, pc.tint(themed(pc, tok::raised, 0x2A2A33)), 3.f);
        pc.ui.paintString(dl, pc.ui.text(pc.node), {pc.content.x, r.y, pc.content.w - r.h, r.h}, pc.tint(pc.style.color), pc.style.fontSize,
                          TextAlignment::Start, pc.node.index);
        const Rect x = closeBox(r);
        const Color c = pc.tint(pc.ui.hovered() == pc.node && x.contains(pc.ui.pointer()) ? themed(pc, tok::hot, 0xFF6B6B) : themed(pc, tok::glyph_dim, 0x9A9AA6));
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
            const bool grab = e.button == MouseButton::Left && !closeBox(ui.rect(ec.node)).contains(e.pos);
            s->dragging = s->floating && grab;
            const Rect r = ui.rect(dock);
            s->grab = {e.pos.x - r.x, e.pos.y - r.y};
            if (s->dragging) raiseFloat(ui, dock);
            if (grab) dragArm(ui, dock, ec.node, e.pos); // past the slop: tears a docked dock off, shows the drop zones
            return false;
        }
        if (e.type == EventType::PointerMove) {
            if (s && ui.pressed() == ec.node && dragMove(ui, ec.node, e.pos)) return true;
            if (s && s->dragging && ui.pressed() == ec.node) { moveFloat(ui, dock, *s, {e.pos.x - s->grab.x, e.pos.y - s->grab.y}); return true; }
            ui.invalidate(ec.node);
            return false;
        }
        if (e.type == EventType::PointerUp && s) { s->dragging = false; dragRelease(ui, ec.node, e.cancel); return false; }
        if (e.type == EventType::KeyDown && e.key == Key::Escape && dragActive(ui)) return dragCancel(ui); // focused while dragged
        if (e.type == EventType::Click) {
            if (dragSwallowClick(ui, ec.node)) return true; // the release that ended a drag is not a click on the close box
            if (closeBox(ui.rect(ec.node)).contains(e.pos)) { dockClose(ui, dock); return true; }
        }
        return false;
    }
};

// The tab strip of docks tabbed together (dockTabify): one tab per open member, the current one highlighted; a close
// box at the right end closes the current dock. value() = index of the current dock among the members.
struct DockTabs {
    static constexpr std::string_view kName = "docktabs";
    static constexpr bool kCapturesDrag = true; // touch: dragging a tab tears its dock off
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
        const Color acc = pc.tint(pc.style.background2.a > 0.f ? pc.style.background2 : themed(pc, tok::accent, 0x3D8BFD));
        dl.rect(r, pc.tint(themed(pc, tok::strip, 0x22222A)), 3.f);
        for (NodeHandle m : s->members) {
            const Rect t = tabRect(ui, pc.node, *s, m);
            if (t.empty()) continue;
            const bool cur = m == s->current, hot = ui.hovered() == pc.node && t.contains(ui.pointer());
            if (cur || hot) dl.rect(t, pc.tint(cur ? themed(pc, tok::surface_hi, 0x34353E) : themed(pc, tok::raised, 0x2A2A33)), 3.f);
            if (cur) dl.rect({t.x + 2.f, t.bottom() - 2.f, t.w - 4.f, 2.f}, acc, 1.f);
            dl.pushClip(t);
            ui.paintString(dl, title(ui, m), {t.x + kTabPad * 0.5f, t.y, t.w - kTabPad * 0.5f, t.h},
                           pc.tint(cur ? pc.style.color : pc.style.color.withAlpha(pc.style.color.a * 0.65f)), pc.style.fontSize,
                           TextAlignment::Start, pc.node.index);
            dl.popClip();
        }
        const Rect x = closeBox(r);
        const Color c = pc.tint(ui.hovered() == pc.node && x.contains(ui.pointer()) ? themed(pc, tok::hot, 0xFF6B6B) : themed(pc, tok::glyph_dim, 0x9A9AA6));
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
        if (e.type == EventType::PointerDown && e.button == MouseButton::Left && !closeBox(ui.rect(ec.node)).contains(e.pos)) {
            for (NodeHandle m : s->members) // a drag that starts on a tab tears that dock off
                if (tabRect(ui, ec.node, *s, m).contains(e.pos)) { dragArm(ui, m, ec.node, e.pos); break; }
            return false;
        }
        if (e.type == EventType::PointerMove) {
            if (ui.pressed() == ec.node && dragMove(ui, ec.node, e.pos)) return true;
            ui.invalidate(ec.node);
            return false;
        }
        if (e.type == EventType::PointerUp) {
            dragRelease(ui, ec.node, e.cancel);
            if (s->members.empty()) ui.destroy(ec.node); // the group dissolved while its strip held the press
            return false;
        }
        if (e.type == EventType::Click) {
            if (dragSwallowClick(ui, ec.node)) return true;
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
        if (ui.pressed() == tabs) showNode(ui, tabs, false); // a tab drag holds the press: destroyed on release
        else ui.destroy(tabs);
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
        ui.removeClass(dock, "os-window");
        for (const char* p : {"left", "top", "width", "height"}) ui.setCss(dock, p, "auto");
    }
}
void moveFloat(UiContext& ui, NodeHandle dock, DockState& s, Vec2 at) {
    // the title strip stays reachable: at least 40 px of it inside the viewport, never above the top edge
    const Size vp = ui.viewport();
    if (ui.osWindows()) { // its own OS window (UiContext::setOsWindows): free to leave the main window
        s.floatRect.x = std::round(at.x);
        s.floatRect.y = std::round(at.y);
    } else {
        s.floatRect.x = std::round(std::clamp(at.x, 40.f - s.floatRect.w, std::max(40.f - s.floatRect.w, vp.w - 40.f)));
        s.floatRect.y = std::round(std::clamp(at.y, 0.f, std::max(0.f, vp.h - 24.f)));
    }
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

// ---- dock drag and drop ----------------------------------------------------------------------------------------------
// One drag at a time per process (one pointer): the session names its context and is ignored by any other.
void joinTabs(UiContext& ui, NodeHandle dock, NodeHandle withDock, bool reopen);
constexpr f32 kDropBox = 36.f;    ///< drop-zone indicator size
constexpr f32 kDropMargin = 10.f; ///< indicator distance from the area edge
struct DockDrag {
    enum class Stage : u8 { Idle, Armed, Active };
    const UiContext* ui = nullptr;
    Stage stage = Stage::Idle;
    NodeHandle dock, source, overlay, prevFocus;
    Vec2 press;
    // where the dock came from (Escape puts it back)
    bool oFloating = false, oCurrent = false;
    Rect oRect;
    DockSide oSide = DockSide::Left;
    NodeHandle oBefore, oPeer, oGroupCurrent;
    u32 oTabIndex = 0;
    // the zone under the pointer
    DockDropZone zone = DockDropZone::None;
    NodeHandle zoneDock, over;
    Rect preview;
    NodeHandle swallow; ///< node whose next Click is the release of a finished drag
};
DockDrag& drag() { static DockDrag d; return d; }
DockDrag* activeDrag(const UiContext& ui) {
    DockDrag& d = drag();
    return d.ui == &ui && d.stage == DockDrag::Stage::Active && ui.alive(d.dock) ? &d : nullptr;
}
bool dragActive(const UiContext& ui) { return activeDrag(ui) != nullptr; }
bool dragSwallowClick(const UiContext& ui, NodeHandle source) {
    DockDrag& d = drag();
    if (d.ui != &ui || d.swallow != source) return false;
    d.swallow = {};
    return true;
}
/// A dock's place in the area: its rect plus its group's tab strip.
Rect dockSlot(const UiContext& ui, NodeHandle dock) {
    Rect r = ui.rect(dock);
    if (const DockState* s = dockState(ui, dock); s && tabsState(ui, s->tabs)) {
        const Rect t = ui.rect(s->tabs);
        r = {std::min(r.x, t.x), std::min(r.y, t.y), std::max(r.right(), t.right()) - std::min(r.x, t.x),
             std::max(r.bottom(), t.bottom()) - std::min(r.y, t.y)};
    }
    return r;
}
/// The docked, showing dock of `area` under `p` (never `skip`).
NodeHandle dockedAt(const UiContext& ui, NodeHandle area, Vec2 p, NodeHandle skip) {
    for (DockSide sd : {DockSide::Left, DockSide::Right, DockSide::Bottom, DockSide::Center})
        for (NodeHandle c = ui.firstChild(dockRegion(ui, area, sd)); ui.alive(c); c = ui.nextSibling(c)) {
            if (c == skip || !ui.hasClass(c, "dock") || ui.hasClass(c, "dock-closed") || !ui.computed(c).visible) continue;
            if (dockSlot(ui, c).contains(p)) return c;
        }
    return {};
}
Rect dropIndicator(const UiContext& ui, NodeHandle area, DockDropZone zone, NodeHandle tabWith) {
    const Rect a = ui.rect(area);
    const f32 b = kDropBox, m = kDropMargin;
    auto centred = [&](const Rect& r) { return Rect{std::round(r.x + (r.w - b) * 0.5f), std::round(r.y + (r.h - b) * 0.5f), b, b}; };
    switch (zone) {
    case DockDropZone::Left: return {a.x + m, std::round(a.y + (a.h - b) * 0.5f), b, b};
    case DockDropZone::Right: return {a.right() - m - b, std::round(a.y + (a.h - b) * 0.5f), b, b};
    case DockDropZone::Bottom: return {std::round(a.x + (a.w - b) * 0.5f), a.bottom() - m - b, b, b};
    case DockDropZone::Center: return centred(ui.rect(dockRegion(ui, area, DockSide::Center)));
    case DockDropZone::Tab: return ui.alive(tabWith) ? centred(dockSlot(ui, tabWith)) : Rect{};
    case DockDropZone::None: break;
    }
    return {};
}
f32 regionBasis(const UiContext& ui, NodeHandle region) {
    const Length l = ui.computed(region).basis;
    return l.unit == Unit::Px && l.value > 0.f ? l.value : 200.f;
}
/// Where a dock dropped on `zone` lands: the region when it shows, else the strip the region will take.
Rect dropPreview(const UiContext& ui, NodeHandle area, DockDropZone zone, NodeHandle tabWith) {
    const Rect a = ui.rect(area);
    const Rect row = ui.rect(ui.firstChild(area));
    auto shown = [&](DockSide sd, Rect& out) {
        const NodeHandle reg = dockRegion(ui, area, sd);
        out = ui.rect(reg);
        return ui.computed(reg).visible && out.w > 0.f && out.h > 0.f;
    };
    Rect r;
    switch (zone) {
    case DockDropZone::Left:
        if (shown(DockSide::Left, r)) return r;
        return {a.x, row.y, std::min(regionBasis(ui, dockRegion(ui, area, DockSide::Left)), a.w * 0.5f), row.h};
    case DockDropZone::Right: {
        if (shown(DockSide::Right, r)) return r;
        const f32 w = std::min(regionBasis(ui, dockRegion(ui, area, DockSide::Right)), a.w * 0.5f);
        return {a.right() - w, row.y, w, row.h};
    }
    case DockDropZone::Bottom: {
        if (shown(DockSide::Bottom, r)) return r;
        const f32 h = std::min(regionBasis(ui, dockRegion(ui, area, DockSide::Bottom)), a.h * 0.5f);
        return {a.x, a.bottom() - h, a.w, h};
    }
    case DockDropZone::Center: return ui.rect(dockRegion(ui, area, DockSide::Center));
    case DockDropZone::Tab: return ui.alive(tabWith) ? dockSlot(ui, tabWith) : Rect{};
    case DockDropZone::None: break;
    }
    return {};
}
bool overlaps(const Rect& a, const Rect& b) { return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom(); }
constexpr DockDropZone kAreaZones[] = {DockDropZone::Left, DockDropZone::Right, DockDropZone::Bottom, DockDropZone::Center};
/// An area indicator is offered unless the "tab with this dock" indicator sits on it.
bool zoneOffered(const UiContext& ui, NodeHandle area, DockDropZone z, NodeHandle over) {
    return !ui.alive(over) || !overlaps(dropIndicator(ui, area, z, {}), dropIndicator(ui, area, DockDropZone::Tab, over));
}
void updateZone(UiContext& ui, DockDrag& d, Vec2 p) {
    const DockState* s = dockState(ui, d.dock);
    const NodeHandle area = s ? s->area : NodeHandle{};
    d.zone = DockDropZone::None;
    d.zoneDock = d.over = {};
    d.preview = {};
    if (ui.alive(area) && ui.rect(area).contains(p)) {
        d.over = dockedAt(ui, area, p, d.dock);
        auto hot = [&](DockDropZone z, NodeHandle with) {
            const Rect r = dropIndicator(ui, area, z, with);
            return !r.empty() && Rect{r.x - 4.f, r.y - 4.f, r.w + 8.f, r.h + 8.f}.contains(p);
        };
        if (d.over.valid() && hot(DockDropZone::Tab, d.over)) {
            d.zone = DockDropZone::Tab;
            d.zoneDock = d.over;
        } else {
            for (DockDropZone z : kAreaZones)
                if (zoneOffered(ui, area, z, d.over) && hot(z, {})) { d.zone = z; break; }
        }
        d.preview = dropPreview(ui, area, d.zone, d.zoneDock);
    }
    if (ui.alive(d.overlay)) ui.invalidate(d.overlay);
}
// The drop-zone layer shown during a drag: one indicator per zone, the hot one lit, and the landing preview.
struct DockDrop {
    static constexpr std::string_view kName = "dockdrop";
    static constexpr A11yRole kRole = A11yRole::None;
    static void glyph(DrawList& dl, const Rect& r, DockDropZone z, Color c) {
        const Rect in{r.x + 8.f, r.y + 8.f, r.w - 16.f, r.h - 16.f};
        dl.roundedRect(in, {}, {2.f, 2.f, 2.f, 2.f}, c, 1.f);
        const f32 k = 0.45f;
        switch (z) { // the filled part is where the dock goes
        case DockDropZone::Left: dl.rect({in.x, in.y, in.w * k, in.h}, c, 1.f); break;
        case DockDropZone::Right: dl.rect({in.right() - in.w * k, in.y, in.w * k, in.h}, c, 1.f); break;
        case DockDropZone::Bottom: dl.rect({in.x, in.bottom() - in.h * k, in.w, in.h * k}, c, 1.f); break;
        case DockDropZone::Center: dl.rect({in.x + 3.f, in.y + 3.f, in.w - 6.f, in.h - 6.f}, c, 1.f); break;
        case DockDropZone::Tab: // a tab strip over a body
            dl.rect({in.x, in.y, in.w * 0.4f, 4.f}, c, 0.f);
            dl.rect({in.x + in.w * 0.5f, in.y, in.w * 0.4f, 4.f}, c.withAlpha(c.a * 0.5f), 0.f);
            break;
        case DockDropZone::None: break;
        }
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const DockDrag* d = activeDrag(pc.ui);
        if (!d || d->overlay != pc.node) return;
        const DockState* s = dockState(pc.ui, d->dock);
        if (!s || !pc.ui.alive(s->area)) return;
        const Color acc = pc.style.background2.a > 0.f ? pc.style.background2 : themed(pc, tok::accent, 0x3D8BFD);
        if (!d->preview.empty()) {
            dl.rect(d->preview, pc.tint(acc.withAlpha(0.28f)), 4.f);
            dl.roundedRect(d->preview, {}, {4.f, 4.f, 4.f, 4.f}, pc.tint(acc.withAlpha(0.85f)), 1.5f);
        }
        if (!pc.ui.rect(s->area).contains(pc.ui.pointer())) return; // indicators show while the pointer is over the area
        auto box = [&](DockDropZone z, NodeHandle with) {
            const Rect r = dropIndicator(pc.ui, s->area, z, with);
            if (r.empty()) return;
            const bool hot = d->zone == z;
            dl.rect(r, pc.tint(hot ? acc : themed(pc, tok::dropzone, 0x22232B).withAlpha(0.92f)), 5.f);
            dl.roundedRect(r, {}, {5.f, 5.f, 5.f, 5.f}, pc.tint(hot ? themed(pc, tok::on_accent, 0xFFFFFF).withAlpha(0.9f) : acc.withAlpha(0.8f)), 1.f);
            glyph(dl, r, z, pc.tint(hot ? themed(pc, tok::on_accent, 0xFFFFFF) : themed(pc, tok::dropzone_glyph, 0xC8CAD4)));
        };
        for (DockDropZone z : kAreaZones)
            if (zoneOffered(pc.ui, s->area, z, d->over)) box(z, {});
        if (d->over.valid()) box(DockDropZone::Tab, d->over);
    }
};
void dragFinish(UiContext& ui, DockDrag& d) {
    if (ui.alive(d.overlay)) ui.destroy(d.overlay);
    if (ui.alive(d.dock)) {
        if (DockState* s = dockState(ui, d.dock)) s->dragging = false;
        const NodeHandle title = ui.firstChild(d.dock);
        const bool had = ui.focused() == title;
        ui.setFocusable(title, false);
        if (had) ui.setFocus(ui.alive(d.prevFocus) ? d.prevFocus : NodeHandle{});
    }
    d.swallow = d.source;
    d.stage = DockDrag::Stage::Idle;
    d.dock = d.overlay = d.prevFocus = d.zoneDock = d.over = {};
    d.zone = DockDropZone::None;
    d.preview = {};
}
void dragArm(UiContext& ui, NodeHandle dock, NodeHandle source, Vec2 at) {
    DockDrag& d = drag();
    if (d.ui == &ui && d.stage == DockDrag::Stage::Active) dragFinish(ui, d); // a drag whose release never arrived
    d = {};
    d.ui = &ui;
    d.stage = DockDrag::Stage::Armed;
    d.dock = dock;
    d.source = source;
    d.press = at;
}
/// Past the slop: remembers where the dock was, floats it under the pointer and shows the drop zones.
void dragBegin(UiContext& ui, DockDrag& d, DockState& s, Vec2 at) {
    const NodeHandle dock = d.dock;
    d.oFloating = s.floating;
    d.oRect = s.floatRect;
    if (!s.floating) {
        const NodeHandle region = ui.parent(dock);
        for (DockSide sd : {DockSide::Left, DockSide::Right, DockSide::Bottom, DockSide::Center})
            if (dockRegion(ui, s.area, sd) == region) d.oSide = sd;
        d.oBefore = ui.nextSibling(dock);
        Rect r = ui.rect(dock);
        if (const DockTabsState* t = tabsState(ui, s.tabs)) {
            for (std::size_t i = 0; i < t->members.size(); ++i) {
                if (t->members[i] == dock) d.oTabIndex = u32(i);
                else if (!d.oPeer.valid()) d.oPeer = t->members[i];
            }
            d.oCurrent = t->current == dock;
            d.oGroupCurrent = t->current;
            if (ui.alive(t->current)) r = ui.rect(t->current);
        }
        const f32 w = std::clamp(r.w, 160.f, 420.f), h = std::clamp(r.h, 120.f, 320.f);
        // the title keeps its place under the pointer; a tab's dock hangs from the pointer by its title
        const Vec2 grab = d.source == ui.firstChild(dock) ? Vec2{r.w > 0.f ? s.grab.x * w / r.w : w * 0.5f, s.grab.y}
                                                         : Vec2{std::min(60.f, w * 0.5f), 12.f};
        dockFloat(ui, dock, {at.x - grab.x, at.y - grab.y, w, h});
        s.grab = grab;
    }
    s.dragging = true;
    d.stage = DockDrag::Stage::Active;
    const NodeHandle layer = overlayLayer(ui);
    d.overlay = make(ui, "dockdrop", layer);
    for (Prop p : {Prop::Left, Prop::Top, Prop::Right, Prop::Bottom}) setPx(ui, d.overlay, p, 0.f);
    ui.setCss(d.overlay, "pointer-events", "none");
    // Escape reaches the drag through the focused title bar
    d.prevFocus = ui.focused();
    const NodeHandle title = ui.firstChild(dock);
    ui.setFocusable(title, true);
    ui.setFocus(title, FocusReason::Programmatic);
}
bool dragMove(UiContext& ui, NodeHandle source, Vec2 at) {
    DockDrag& d = drag();
    if (d.ui != &ui || d.source != source || d.stage == DockDrag::Stage::Idle) return false;
    DockState* s = dockState(ui, d.dock);
    if (!s) { d.stage = DockDrag::Stage::Idle; return false; }
    if (d.stage == DockDrag::Stage::Armed) {
        if (std::hypot(at.x - d.press.x, at.y - d.press.y) < kDockDragSlop) return false;
        dragBegin(ui, d, *s, at);
    }
    moveFloat(ui, d.dock, *s, {at.x - s->grab.x, at.y - s->grab.y});
    updateZone(ui, d, at);
    return true;
}
/// Puts a tabbed dock at `index` of its group (tab order).
void setTabOrder(UiContext& ui, NodeHandle dock, u32 index) {
    const DockState* s = dockState(ui, dock);
    DockTabsState* t = s ? tabsState(ui, s->tabs) : nullptr;
    if (!t) return;
    std::erase(t->members, dock);
    t->members.insert(t->members.begin() + std::min<std::size_t>(index, t->members.size()), dock);
    ui.setText(s->tabs, {});
    ui.invalidate(s->tabs);
}
bool dragCancel(UiContext& ui) {
    DockDrag* d = activeDrag(ui);
    if (!d) {
        if (drag().ui == &ui) drag().stage = DockDrag::Stage::Idle;
        return false;
    }
    const NodeHandle dock = d->dock;
    DockState* s = dockState(ui, dock);
    if (s && d->oFloating) {
        dockFloat(ui, dock, d->oRect);
    } else if (s) {
        dockTo(ui, dock, d->oSide);
        const NodeHandle region = ui.parent(dock);
        if (const DockState* ps = dockState(ui, d->oPeer); ps && !ps->floating && ui.parent(d->oPeer) == region) {
            joinTabs(ui, dock, d->oPeer, true);
            setTabOrder(ui, dock, d->oTabIndex);
            dockRaise(ui, !d->oCurrent && ui.alive(d->oGroupCurrent) ? d->oGroupCurrent : dock);
        }
        if (ui.alive(d->oBefore) && ui.parent(d->oBefore) == region && d->oBefore != dock) ui.moveTo(dock, region, d->oBefore);
    }
    dragFinish(ui, *d);
    return true;
}
/// The release: over a zone the dock lands there, elsewhere it stays floating; `cancel` (a cancelled touch) restores.
bool dragRelease(UiContext& ui, NodeHandle source, bool cancel) {
    DockDrag& d = drag();
    if (d.ui != &ui || d.source != source) return false;
    if (d.stage != DockDrag::Stage::Active || !ui.alive(d.dock)) { d.stage = DockDrag::Stage::Idle; return false; }
    if (cancel) return dragCancel(ui);
    const NodeHandle dock = d.dock, with = d.zoneDock;
    const DockDropZone zone = d.zone;
    dragFinish(ui, d);
    switch (zone) {
    case DockDropZone::Left: dockTo(ui, dock, DockSide::Left); break;
    case DockDropZone::Right: dockTo(ui, dock, DockSide::Right); break;
    case DockDropZone::Bottom: dockTo(ui, dock, DockSide::Bottom); break;
    case DockDropZone::Center: dockTo(ui, dock, DockSide::Center); break;
    case DockDropZone::Tab:
        dockTabify(ui, dock, with);
        dockRaise(ui, dock); // the dropped dock is the one in view
        break;
    case DockDropZone::None: break;
    }
    return true;
}

// ---- table view: widgets_modelview.cpp (one item view class behind tableView / modelListView / treeModelView) ----

void registerTierB2(UiContext& ui) {
    WidgetRegistry& r = ui.registry();
    if (r.find("tableview") != 0xFFFFFFFFu) return;
    r.registerWidget<SplitHandle>();
    r.registerWidget<DockTitle>();
    r.registerWidget<DockTabs>();
    r.registerWidget<DockDrop>();
    detail::registerModelView(ui); // tableview, treemodelview (+ their accessibility hooks)
    detail::a11yTierB2(r);
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
    ui.addClass(dock, "os-window"); // a tool window of its own when the host enabled them (UiContext::setOsWindows)
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

// ---- dock drag and drop API -------------------------------------------------------------------------------------------
NodeHandle dockDragged(const UiContext& ui) {
    const DockDrag* d = activeDrag(ui);
    return d ? d->dock : NodeHandle{};
}
DockDropZone dockDropZone(const UiContext& ui) {
    const DockDrag* d = activeDrag(ui);
    return d ? d->zone : DockDropZone::None;
}
NodeHandle dockDropTarget(const UiContext& ui) {
    const DockDrag* d = activeDrag(ui);
    return d ? d->zoneDock : NodeHandle{};
}
Rect dockDropPreview(const UiContext& ui) {
    const DockDrag* d = activeDrag(ui);
    return d ? d->preview : Rect{};
}
Rect dockDropIndicator(const UiContext& ui, NodeHandle area, DockDropZone zone, NodeHandle tabWith) {
    return ui.alive(area) && ui.hasClass(area, "dockarea") ? dropIndicator(ui, area, zone, tabWith) : Rect{};
}
bool dockCancelDrag(UiContext& ui) { return dragCancel(ui); }

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
            if (!ts || ts->members.empty()) continue; // (a dissolved strip lives until its press is released)
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

#include "widgets_internal.hpp"
#include <cstdio>
// ---- accessibility (plan §4.14): dock tab strip (the item views: widgets_modelview.cpp) -----------------------------------
namespace fuse::ui::widgets {
namespace {
using detail::a11yItem;
// Dock tab strip: a tab list with one tab per open dock of the group (id = the dock's node index + 1).
void dockTabsA11y(const UiContext& cui, NodeHandle n, A11ySnapshotNode& self, std::vector<A11ySnapshotNode>& items) {
    UiContext& ui = const_cast<UiContext&>(cui);
    const DockTabsState* s = tabsState(ui, n);
    if (!s) return;
    u32 open = 0, pos = 0;
    for (NodeHandle m : s->members) open += DockTabs::open(ui, m);
    for (NodeHandle m : s->members) {
        if (!DockTabs::open(ui, m)) continue;
        A11ySnapshotNode it = a11yItem(m.index + 1, A11yRole::Tab, DockTabs::title(ui, m), DockTabs::tabRect(ui, n, *s, m),
                                       self.disabled ? 0u : detail::a11yBits({A11yAction::Select, A11yAction::Click}));
        it.selectable = true;
        it.selected = m == s->current;
        it.posInSet = ++pos;
        it.setSize = open;
        items.push_back(std::move(it));
    }
}
bool dockTabsA11yAction(UiContext& ui, NodeHandle n, u32 child, A11yAction a, f32, std::string_view) {
    const DockTabsState* s = tabsState(ui, n);
    if (!s || !child || (a != A11yAction::Select && a != A11yAction::Click)) return false;
    for (NodeHandle m : s->members)
        if (m.index + 1 == child && DockTabs::open(ui, m)) { dockRaise(ui, m); return true; }
    return false;
}
} // namespace
void detail::a11yTierB2(WidgetRegistry& r) {
    a11yHook(r, "docktabs", A11yRole::TabList, &dockTabsA11y, &dockTabsA11yAction);
}
} // namespace fuse::ui::widgets
