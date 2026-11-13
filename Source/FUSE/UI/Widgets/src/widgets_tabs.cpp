// FUSE UI tab bar options (QTabBar parity; the gui-framework branch's feature set written against main's tab bar):
// closable tabs, movable tabs (live reorder; the pages of a tab widget follow), disabled tabs, scroll buttons when
// the tabs overflow (or shrink-to-fit without them), wheel / Home / End, add / remove pages of a tab widget.
// A tab bar with options measures, paints and handles events here; one without keeps the Tier B path.
#include "fuse/ui/widgets/widgets.hpp"
#include "widgets_internal.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::ui::widgets {
namespace {

constexpr f32 kPad = 12.f, kClose = 18.f, kArrow = 18.f, kDots = 12.f, kDragSlop = 10.f;

struct TabBarState {
    u32 gen = 0;
    bool closable = false, movable = false, scrollButtons = true;
    std::vector<u8> disabled; ///< per tab (missing = enabled)
    f32 scroll = 0.f;
    i32 lastCur = -1;
    i32 drag = -1; ///< tab being dragged (its current index)
    f32 pressX = 0.f;
    bool dragging = false;
};
// Keyed by UiContext::uid(), then node index (generation-checked). Empty until the first option is set.
std::unordered_map<u64, std::unordered_map<u32, TabBarState>>& stores() {
    static std::unordered_map<u64, std::unordered_map<u32, TabBarState>> m;
    return m;
}
TabBarState* find(const UiContext& ui, NodeHandle n) {
    auto& m = stores();
    if (m.empty()) return nullptr;
    const auto it = m.find(ui.uid());
    if (it == m.end()) return nullptr;
    const auto j = it->second.find(n.index);
    return j != it->second.end() && j->second.gen == n.generation ? &j->second : nullptr;
}
bool isTabBar(const UiContext& ui, NodeHandle n) { return ui.alive(n) && ui.classOf(n) == ui.registry().find("tabbar"); }
TabBarState* ensure(UiContext& ui, NodeHandle n) {
    if (!isTabBar(ui, n)) return nullptr;
    if (TabBarState* s = find(ui, n)) return s;
    auto& m = stores();
    std::erase_if(m, [](const auto& kv) { return !UiContext::exists(kv.first); });
    auto& mine = m[ui.uid()];
    std::erase_if(mine, [&](const auto& kv) { return !ui.alive(NodeHandle{kv.first, kv.second.gen}); });
    TabBarState& s = mine[n.index];
    s = TabBarState{};
    s.gen = n.generation;
    return &s; // lastCur = -1: the first paint brings the current tab into view
}

u32 count(std::string_view s) { return s.empty() ? 0u : u32(std::count(s.begin(), s.end(), '\n') + 1); }
template <class F>
void eachLine(std::string_view s, F&& f) {
    u32 i = 0;
    for (std::size_t at = 0; !s.empty();) {
        const std::size_t nl = s.find('\n', at);
        if (!f(i++, s.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at))) return;
        if (nl == std::string_view::npos) return;
        at = nl + 1;
    }
}
f32 rowHeight(UiContext& ui, NodeHandle n) {
    const f32 px = ui.computed(n).fontSize;
    return std::ceil(ui.font() ? ui.font()->metrics(px).lineHeight() : px * 1.25f) + 6.f;
}
bool off(const TabBarState& s, u32 i) { return i < s.disabled.size() && s.disabled[i]; }
f32 chrome(const TabBarState& s) { return 2.f * kPad + (s.closable ? kClose : 0.f); }
f32 natural(UiContext& ui, NodeHandle n, const TabBarState& s, std::string_view t) {
    return std::ceil(ui.measureString(t, ui.computed(n).fontSize)) + chrome(s);
}
f32 natTotal(UiContext& ui, NodeHandle n, const TabBarState& s) {
    f32 m = 0.f;
    eachLine(ui.text(n), [&](u32, std::string_view t) { m += natural(ui, n, s, t); return true; });
    return m;
}
bool tooLong(UiContext& ui, NodeHandle n, const TabBarState& s) { return natTotal(ui, n, s) > ui.contentRect(n).w + 0.5f; }
/// Tabs scroll when they overflow (QTabBar::usesScrollButtons): two arrow buttons take the far end.
bool overflow(UiContext& ui, NodeHandle n, const TabBarState& s) { return s.scrollButtons && tooLong(ui, n, s); }
/// Without scroll buttons an overflowing bar shrinks its tabs (titles clip behind an ellipsis); tabs already at their
/// minimum (chrome + ellipsis) stay there and the rest share what is left.
f32 shrink(UiContext& ui, NodeHandle n, const TabBarState& s) {
    if (s.scrollButtons || !tooLong(ui, n, s)) return 1.f;
    const f32 len = std::max(0.f, ui.contentRect(n).w);
    f32 k = len / std::max(1.f, natTotal(ui, n, s));
    for (int it = 0; it < 4; ++it) {
        f32 pinned = 0.f, free = 0.f;
        eachLine(ui.text(n), [&](u32, std::string_view t) {
            const f32 nat = natural(ui, n, s, t), mn = chrome(s) + kDots;
            if (nat * k < mn) pinned += mn;
            else free += nat;
            return true;
        });
        if (free <= 0.f) break;
        k = std::max(0.f, len - pinned) / free;
    }
    return std::min(k, 1.f);
}
f32 tabLen(UiContext& ui, NodeHandle n, const TabBarState& s, std::string_view t, f32 k) {
    const f32 nat = natural(ui, n, s, t);
    return k >= 1.f ? nat : std::max(chrome(s) + kDots, nat * k);
}
f32 viewW(UiContext& ui, NodeHandle n, const TabBarState& s) { return ui.contentRect(n).w - (overflow(ui, n, s) ? 2.f * kArrow : 0.f); }
Rect tabRect(UiContext& ui, NodeHandle n, const TabBarState& s, u32 index) {
    const Rect c = ui.contentRect(n);
    const f32 k = shrink(ui, n, s);
    f32 x = c.x - s.scroll;
    Rect out{};
    eachLine(ui.text(n), [&](u32 i, std::string_view t) {
        const f32 w = tabLen(ui, n, s, t, k);
        if (i == index) { out = {x, c.y, w, c.h}; return false; }
        x += w;
        return true;
    });
    return out;
}
Rect closeBox(const Rect& r) { return {r.right() - kPad * 0.5f - 14.f, r.y + r.h * 0.5f - 7.f, 14.f, 14.f}; }
i32 tabAt(UiContext& ui, NodeHandle n, const TabBarState& s, Vec2 p) {
    const Rect c = ui.contentRect(n);
    if (!c.contains(p) || p.x >= c.x + viewW(ui, n, s)) return -1;
    for (u32 i = 0, k = count(ui.text(n)); i < k; ++i)
        if (tabRect(ui, n, s, i).contains(p)) return i32(i);
    return -1;
}
void setScroll(UiContext& ui, NodeHandle n, TabBarState& s, f32 v) {
    f32 mx = 0.f;
    if (overflow(ui, n, s)) mx = std::max(0.f, natTotal(ui, n, s) - viewW(ui, n, s));
    const f32 nv = std::clamp(v, 0.f, mx);
    if (nv != s.scroll) { s.scroll = nv; ui.invalidate(n); }
}
void ensureVisible(UiContext& ui, NodeHandle n, TabBarState& s, u32 i) {
    const Rect c = ui.contentRect(n), r = tabRect(ui, n, s, i);
    const f32 x0 = r.x - c.x + s.scroll, x1 = x0 + r.w, vw = viewW(ui, n, s);
    if (x0 < s.scroll) setScroll(ui, n, s, x0);
    else if (x1 > s.scroll + vw) setScroll(ui, n, s, x1 - vw);
}
/// The scroll stays in range and the current tab in view (after a resize, a title change, a programmatic setValue).
void settle(UiContext& ui, NodeHandle n, TabBarState& s) {
    setScroll(ui, n, s, s.scroll);
    const i32 cur = i32(ui.value(n));
    if (cur != s.lastCur) {
        s.lastCur = cur;
        if (cur >= 0) ensureVisible(ui, n, s, u32(cur));
    }
}
/// Next enabled tab from `from` stepping `dir` (no wrap); -1 if none.
i32 step(UiContext& ui, NodeHandle n, const TabBarState& s, i32 from, i32 dir) {
    const i32 c = i32(count(ui.text(n)));
    for (i32 i = from + dir; i >= 0 && i < c; i += dir)
        if (!off(s, u32(i))) return i;
    return -1;
}
/// The page stack of the tab widget `bar` heads (invalid for a bare tab bar).
NodeHandle stackOf(const UiContext& ui, NodeHandle bar) {
    const NodeHandle w = ui.parent(bar);
    return ui.alive(w) && ui.hasClass(w, "tabwidget") && ui.firstChild(w) == bar ? ui.nextSibling(bar) : NodeHandle{};
}
void showPage(UiContext& ui, NodeHandle stack, u32 current) {
    u32 i = 0;
    for (NodeHandle p = ui.firstChild(stack); ui.alive(p); p = ui.nextSibling(p), ++i)
        ui.setCss(p, "display", i == current ? "flex" : "none");
}
void setTitles(UiContext& ui, NodeHandle n, const std::vector<std::string>& t) {
    std::string joined;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (i) joined += '\n';
        joined += t[i];
    }
    ui.setText(n, joined);
    ui.setRange(n, 0.f, f32(std::max<std::size_t>(1, t.size()) - 1), 1.f);
    ui.invalidateLayout(n);
}
/// Moves tab `from` to index `to`: titles, current tab, disabled flags and, in a tab widget, the page.
void moveTab(UiContext& ui, NodeHandle n, TabBarState* s, u32 from, u32 to) {
    const u32 c = count(ui.text(n));
    if (from >= c || to >= c || from == to) return;
    std::vector<std::string> t;
    eachLine(ui.text(n), [&](u32, std::string_view l) { t.emplace_back(l); return true; });
    std::string moved = std::move(t[from]);
    t.erase(t.begin() + from);
    t.insert(t.begin() + to, std::move(moved));
    auto remap = [&](u32 i) -> u32 {
        if (i == from) return to;
        if (from < to && i > from && i <= to) return i - 1;
        if (to < from && i >= to && i < from) return i + 1;
        return i;
    };
    const u32 cur = remap(u32(std::max(0.f, ui.value(n))));
    setTitles(ui, n, t);
    if (s) {
        std::vector<u8> dis(c, 0);
        for (u32 i = 0; i < c; ++i)
            if (off(*s, i)) dis[remap(i)] = 1;
        s->disabled = std::move(dis);
        s->lastCur = i32(cur);
    }
    ui.setValue(n, f32(cur)); // the same tab stays current: no signal
    if (const NodeHandle stack = stackOf(ui, n); ui.alive(stack)) { // the page follows its tab
        NodeHandle pg = ui.firstChild(stack);
        for (u32 i = 0; i < from && ui.alive(pg); ++i) pg = ui.nextSibling(pg);
        if (ui.alive(pg)) {
            ui.moveTo(pg, stack); // to the end, then before the page now at `to`
            NodeHandle before = ui.firstChild(stack);
            for (u32 i = 0; i < to && ui.alive(before); ++i) before = ui.nextSibling(before);
            if (ui.alive(before) && before != pg) ui.moveTo(pg, stack, before);
        }
        showPage(ui, stack, cur);
    }
    ui.emit(n, SignalKind::Event, {.value = f32(to), .text = "tabMoved", .value2 = f32(from)});
    ui.invalidate(n);
}

Color accent(const PaintContext& pc) { return pc.tint(pc.style.background2.a > 0.f ? pc.style.background2 : Color::rgb(0x3D8BFD)); }
void dots(DrawList& dl, f32 x, f32 cy, Color col) { // three-dot ellipsis (no glyph metrics needed)
    for (int k = 0; k < 3; ++k) dl.rect({x + 1.f + f32(k) * 4.f, cy - 1.f, 2.f, 2.f}, col, 1.f);
}

} // namespace

namespace detail {

bool tabBarHasState(const UiContext& ui, NodeHandle n) { return find(ui, n) != nullptr; }
Size tabBarMeasure(UiContext& ui, NodeHandle n) {
    const TabBarState* s = find(ui, n);
    return {s ? natTotal(ui, n, *s) : 0.f, rowHeight(ui, n)};
}
Rect tabBarTabBounds(UiContext& ui, NodeHandle n, u32 index) {
    TabBarState* s = find(ui, n);
    if (!s) return {};
    settle(ui, n, *s);
    return tabRect(ui, n, *s, index);
}
bool tabBarTabDisabled(const UiContext& ui, NodeHandle n, u32 index) {
    const TabBarState* s = find(ui, n);
    return s && off(*s, index);
}

void tabBarPaint(const PaintContext& pc, DrawList& dl) {
    UiContext& ui = pc.ui;
    TabBarState* sp = find(ui, pc.node);
    if (!sp) return;
    TabBarState& s = *sp;
    settle(ui, pc.node, s);
    const Rect c = pc.content;
    const u32 sel = u32(ui.value(pc.node));
    const bool ov = overflow(ui, pc.node, s);
    const f32 vw = viewW(ui, pc.node, s);
    dl.pushClip({c.x, c.y, vw, c.h});
    eachLine(ui.text(pc.node), [&](u32 i, std::string_view t) {
        const Rect r = tabRect(ui, pc.node, s, i);
        if (r.right() < c.x || r.x > c.x + vw) return true;
        if (i == sel) {
            dl.rect(r, pc.tint(Color::rgb(s.dragging && i32(i) == s.drag ? 0x40414C : 0x34353E)), 4.f);
            dl.rect({r.x + 4.f, c.bottom() - 2.f, r.w - 8.f, 2.f}, accent(pc));
        }
        const bool dim = off(s, i);
        const Color col = pc.tint(dim ? pc.style.color.withAlpha(pc.style.color.a * 0.4f) : pc.style.color);
        const Rect tr{r.x + kPad, r.y, r.w - 2.f * kPad - (s.closable ? kClose : 0.f), r.h};
        const f32 tw = std::ceil(ui.measureString(t, pc.style.fontSize));
        if (tw <= tr.w + 0.5f) {
            ui.paintString(dl, t, tr, col, pc.style.fontSize, TextAlignment::Center, pc.node.index);
        } else { // shrunk tab: the title clips behind an ellipsis
            const f32 avail = std::max(0.f, tr.w - kDots);
            dl.pushClip({tr.x, tr.y, avail, tr.h});
            ui.paintString(dl, t, {tr.x, tr.y, tw, tr.h}, col, pc.style.fontSize, TextAlignment::Start, pc.node.index);
            dl.popClip();
            dots(dl, tr.x + avail, tr.y + tr.h * 0.5f, col);
        }
        if (s.closable) {
            const Rect x = closeBox(r);
            const bool hot = ui.hovered() == pc.node && x.contains(ui.pointer()) && !dim;
            const Color xc = pc.tint(hot ? Color::rgb(0xFF6B6B) : Color::rgb(0x9A9AA6));
            const f32 m = 4.f;
            dl.line({x.x + m, x.y + m}, {x.right() - m, x.bottom() - m}, xc, 1.5f);
            dl.line({x.right() - m, x.y + m}, {x.x + m, x.bottom() - m}, xc, 1.5f);
        }
        return true;
    });
    dl.popClip();
    if (ov) { // scroll buttons at the far end; dimmed at the end of their travel
        const f32 mx = std::max(0.f, natTotal(ui, pc.node, s) - vw);
        for (int k = 0; k < 2; ++k) {
            const bool can = k ? s.scroll < mx - 0.5f : s.scroll > 0.5f;
            const Color col = pc.tint(Color::rgb(0x9A9AA6).withAlpha(can ? 1.f : 0.35f));
            const Rect b{c.x + vw + f32(k) * kArrow, c.y, kArrow, c.h};
            const f32 cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.5f, d = k ? 3.f : -3.f;
            dl.line({cx - d, cy - 5.f}, {cx + d, cy}, col, 1.5f);
            dl.line({cx + d, cy}, {cx - d, cy + 5.f}, col, 1.5f);
        }
    }
    dl.rect({c.x, c.bottom() - 1.f, c.w, 1.f}, pc.tint(Color::rgb(0x474955)));
}

bool tabBarEvent(EventContext& ec, const Event& e) {
    if (ec.phase != Phase::Target) return false;
    UiContext& ui = ec.ui;
    const NodeHandle n = ec.node;
    TabBarState* sp = find(ui, n);
    if (!sp) return false;
    TabBarState& s = *sp;
    const u32 c = count(ui.text(n)), cur = u32(std::max(0.f, ui.value(n)));
    auto pick = [&](i32 i) {
        if (c && i >= 0 && u32(i) < c && u32(i) != cur && !off(s, u32(i))) {
            ui.setValue(n, f32(i), true);
            if (TabBarState* st = find(ui, n)) { st->lastCur = i; ensureVisible(ui, n, *st, u32(i)); } // (a slot may have removed tabs)
        }
    };
    const Rect cr = ui.contentRect(n);
    switch (e.type) {
    case EventType::PointerDown: {
        if (e.button != MouseButton::Left) return false; // (a context-menu press does not switch tabs)
        if (overflow(ui, n, s) && e.pos.x >= cr.x + viewW(ui, n, s)) {
            setScroll(ui, n, s, s.scroll + (e.pos.x >= cr.x + viewW(ui, n, s) + kArrow ? 80.f : -80.f));
            return true;
        }
        const i32 i = tabAt(ui, n, s, e.pos);
        if (i < 0 || off(s, u32(i))) return true;
        if (s.closable && closeBox(tabRect(ui, n, s, u32(i))).contains(e.pos)) { // QTabBar::tabCloseRequested
            ui.emit(n, SignalKind::Event, {.value = f32(i), .text = "tabCloseRequested"});
            return true;
        }
        s.drag = s.movable ? i : -1;
        s.pressX = e.pos.x;
        s.dragging = false;
        pick(i);
        return true;
    }
    case EventType::PointerMove: {
        if (ui.pressed() != n || s.drag < 0) {
            if (s.closable) ui.invalidate(n); // close box hover
            return false;
        }
        if (!s.dragging && std::abs(e.pos.x - s.pressX) < kDragSlop) return true;
        s.dragging = true;
        // live reorder (QTabBar::setMovable): swap with a neighbour once the pointer passes its middle
        for (i32 to : {s.drag - 1, s.drag + 1}) {
            if (to < 0 || u32(to) >= c) continue;
            const Rect r = tabRect(ui, n, s, u32(to));
            const f32 mid = r.x + r.w * 0.5f;
            if ((to < s.drag && e.pos.x < mid) || (to > s.drag && e.pos.x > mid)) {
                const i32 from = s.drag;
                s.drag = to;
                moveTab(ui, n, &s, u32(from), u32(to));
                break;
            }
        }
        ui.invalidate(n);
        return true;
    }
    case EventType::PointerUp:
        if (s.dragging) ui.invalidate(n);
        s.drag = -1;
        s.dragging = false;
        return false;
    case EventType::Wheel: { // QTabBar: the wheel changes the current tab
        const i32 t = step(ui, n, s, i32(cur), e.wheel.y > 0.f || e.wheel.x > 0.f ? -1 : 1);
        if (t >= 0) pick(t);
        return true;
    }
    case EventType::KeyDown:
        switch (e.key) {
        case Key::Right: if (const i32 t = step(ui, n, s, i32(cur), 1); t >= 0) pick(t); return true;
        case Key::Left: if (const i32 t = step(ui, n, s, i32(cur), -1); t >= 0) pick(t); return true;
        case Key::Home: if (const i32 t = step(ui, n, s, -1, 1); t >= 0) pick(t); return true;
        case Key::End: if (const i32 t = step(ui, n, s, i32(c), -1); t >= 0) pick(t); return true;
        default: return false;
        }
    default: return false;
    }
}

} // namespace detail

void setTabsClosable(UiContext& ui, NodeHandle bar, bool on) {
    if (TabBarState* s = ensure(ui, bar)) {
        s->closable = on;
        ui.invalidateLayout(bar); // tab widths include the close box
        ui.invalidate(bar);
    }
}
bool tabsClosable(const UiContext& ui, NodeHandle bar) { const TabBarState* s = find(ui, bar); return s && s->closable; }
void setTabsMovable(UiContext& ui, NodeHandle bar, bool on) {
    if (TabBarState* s = ensure(ui, bar)) s->movable = on;
}
bool tabsMovable(const UiContext& ui, NodeHandle bar) { const TabBarState* s = find(ui, bar); return s && s->movable; }
void setTabEnabled(UiContext& ui, NodeHandle bar, u32 index, bool on) {
    TabBarState* s = ensure(ui, bar);
    if (!s || index >= count(ui.text(bar))) return;
    if (s->disabled.size() <= index) s->disabled.resize(index + 1, 0);
    s->disabled[index] = on ? 0 : 1;
    if (!on && u32(std::max(0.f, ui.value(bar))) == index) { // QTabBar moves off a disabled current tab
        i32 t = step(ui, bar, *s, i32(index), 1);
        if (t < 0) t = step(ui, bar, *s, i32(index), -1);
        if (t >= 0) ui.setValue(bar, f32(t), true);
    }
    ui.invalidate(bar);
}
bool tabEnabled(const UiContext& ui, NodeHandle bar, u32 index) {
    const TabBarState* s = find(ui, bar);
    return !(s && off(*s, index));
}
void setTabUsesScrollButtons(UiContext& ui, NodeHandle bar, bool on) {
    if (TabBarState* s = ensure(ui, bar)) {
        s->scrollButtons = on;
        if (!on) s->scroll = 0.f;
        ui.invalidate(bar);
    }
}
bool tabUsesScrollButtons(const UiContext& ui, NodeHandle bar) { const TabBarState* s = find(ui, bar); return !s || s->scrollButtons; }
void tabBarMoveTab(UiContext& ui, NodeHandle bar, u32 from, u32 to) {
    if (isTabBar(ui, bar)) moveTab(ui, bar, find(ui, bar), from, to);
}
Rect tabBarTabRect(UiContext& ui, NodeHandle bar, u32 index) {
    if (!isTabBar(ui, bar) || index >= count(ui.text(bar))) return {};
    if (find(ui, bar)) return detail::tabBarTabBounds(ui, bar, index);
    // plain tab bar: titles side by side at their natural width
    const Rect c = ui.contentRect(bar);
    f32 x = c.x;
    Rect out{};
    eachLine(ui.text(bar), [&](u32 i, std::string_view t) {
        const f32 w = std::ceil(ui.measureString(t, ui.computed(bar).fontSize)) + 2.f * kPad;
        if (i == index) { out = {x, c.y, w, c.h}; return false; }
        x += w;
        return true;
    });
    return out;
}
f32 tabBarScroll(const UiContext& ui, NodeHandle bar) { const TabBarState* s = find(ui, bar); return s ? s->scroll : 0.f; }

NodeHandle tabWidgetAddPage(UiContext& ui, NodeHandle tabs, std::string_view title) {
    if (!ui.alive(tabs) || !ui.hasClass(tabs, "tabwidget")) return {};
    const NodeHandle bar = tabWidgetBar(ui, tabs), stack = ui.nextSibling(bar);
    if (!isTabBar(ui, bar) || !ui.alive(stack)) return {};
    std::vector<std::string> t;
    eachLine(ui.text(bar), [&](u32, std::string_view l) { t.emplace_back(l); return true; });
    t.emplace_back(title);
    const f32 cur = t.size() == 1 ? 0.f : ui.value(bar);
    setTitles(ui, bar, t);
    ui.setValue(bar, cur);
    const NodeHandle p = panel(ui, stack);
    ui.setA11y(p, A11yRole::TabPanel, title);
    showPage(ui, stack, u32(std::max(0.f, ui.value(bar))));
    ui.invalidate(bar);
    return p;
}
void tabWidgetRemovePage(UiContext& ui, NodeHandle tabs, u32 index) {
    if (!ui.alive(tabs) || !ui.hasClass(tabs, "tabwidget")) return;
    const NodeHandle bar = tabWidgetBar(ui, tabs), stack = ui.nextSibling(bar);
    if (!isTabBar(ui, bar) || !ui.alive(stack)) return;
    const u32 c = count(ui.text(bar));
    if (index >= c) return;
    std::vector<std::string> t;
    eachLine(ui.text(bar), [&](u32 i, std::string_view l) { if (i != index) t.emplace_back(l); return true; });
    if (const NodeHandle pg = tabPage(ui, tabs, index); ui.alive(pg)) ui.destroy(pg);
    const u32 was = u32(std::max(0.f, ui.value(bar)));
    TabBarState* s = find(ui, bar);
    if (s) {
        if (index < s->disabled.size()) s->disabled.erase(s->disabled.begin() + index);
        s->drag = -1;
        s->dragging = false;
    }
    setTitles(ui, bar, t);
    // Qt: removing a tab before the current one keeps the same page current; removing the current one shows the
    // next (the previous at the end)
    u32 cur = was > index ? was - 1 : was;
    if (!t.empty()) cur = std::min<u32>(cur, u32(t.size()) - 1);
    if (s && !t.empty() && off(*s, cur)) {
        i32 alt = step(ui, bar, *s, i32(cur), 1);
        if (alt < 0) alt = step(ui, bar, *s, i32(cur), -1);
        if (alt >= 0) cur = u32(alt);
    }
    const bool changed = was == index || t.empty();
    ui.setValue(bar, t.empty() ? 0.f : f32(cur));
    if (s) s->lastCur = -1; // settle() brings the current tab back into view
    showPage(ui, stack, cur);
    if (changed && !t.empty()) ui.emit(bar, SignalKind::ValueChanged, {.value = f32(cur)});
    ui.invalidate(bar);
}

} // namespace fuse::ui::widgets
