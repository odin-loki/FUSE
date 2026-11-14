// FUSE UI extra widgets (see widgets/extra.hpp): tool box, input dialog, LCD number, key sequence edit, MDI area,
// graphics scene / view, on-screen keyboard, dialog default / reject buttons. Ported from the gui-framework branch's
// Extras/src/widgets_extra.cpp onto main's widget conventions (function-table classes, state pools keyed by user
// data, accessibility hooks, touch capture, cursors). The dialog button box is widgets_buttonbox.cpp.
#include "fuse/ui/widgets/widgets.hpp"
#include "fuse/ui/path.hpp"
#include "widgets_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fuse::ui::widgets {
namespace {

NodeHandle make(UiContext& ui, std::string_view cls, NodeHandle parent, std::string_view id = {}) {
    auto h = ui.create(cls, parent, id);
    return h ? *h : NodeHandle{};
}
void px(UiContext& ui, NodeHandle n, Prop p, f32 v) { ui.set(n, p, PropValue::ofLength(Length::px(std::round(v)))); }
NodeHandle nth(const UiContext& ui, NodeHandle n, u32 i) {
    NodeHandle c = ui.firstChild(n);
    while (i-- && ui.alive(c)) c = ui.nextSibling(c);
    return ui.alive(c) ? c : NodeHandle{};
}
template <class F>
void eachLine(std::string_view s, F&& f) {
    u32 i = 0;
    for (std::size_t at = 0; !s.empty();) {
        const std::size_t nl = s.find('\n', at);
        f(i++, s.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at));
        if (nl == std::string_view::npos) return;
        at = nl + 1;
    }
}
f32 lineH(const UiContext& ui, NodeHandle n) {
    const f32 p = ui.computed(n).fontSize;
    return std::ceil(ui.font() ? ui.font()->metrics(p).lineHeight() : p * 1.25f);
}
bool inSubtree(const UiContext& ui, NodeHandle root, NodeHandle n) {
    for (; ui.alive(n); n = ui.parent(n))
        if (n == root) return true;
    return false;
}
NodeHandle findClass(const UiContext& ui, NodeHandle n, std::string_view cls) {
    for (NodeHandle c = ui.firstChild(n); ui.alive(c); c = ui.nextSibling(c)) {
        if (ui.hasClass(c, cls)) return c;
        if (const NodeHandle d = findClass(ui, c, cls); d.valid()) return d;
    }
    return {};
}

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
bool isClass(const UiContext& ui, NodeHandle n, std::string_view cls) { return ui.alive(n) && ui.classOf(n) == ui.registry().find(cls); }

// =====================================================================================================================
// LCD number
struct LcdState { u32 digits = 6, base = 10; std::string text; bool explicitText = false; };
Pool<LcdState>& lcdPool() { static Pool<LcdState> p; return p; }
LcdState* lcdOf(const UiContext& ui, NodeHandle n) { return isClass(ui, n, "lcdnumber") ? lcdPool().get(ui, n) : nullptr; }
u32 lcdCells(std::string_view t) { return u32(std::count_if(t.begin(), t.end(), [](char c) { return c != '.'; })); }
/// What the display shows; the view points into `buf` (at least 80 bytes) or the state's text. No allocation.
std::string_view lcdString(const UiContext& ui, NodeHandle n, const LcdState& s, char* buf, std::size_t cap) {
    if (s.explicitText) return s.text;
    const double v = ui.value(n);
    std::string_view r;
    if (s.base == 10) {
        const int len = std::snprintf(buf, cap, "%.*g", int(std::max(1u, s.digits)), v);
        r = std::string_view(buf, std::size_t(std::clamp(len, 0, int(cap) - 1)));
        if (r.find('e') != std::string_view::npos) return "E"; // does not fit (QLCDNumber::overflow)
    } else { // digits written from the end of the buffer
        long long iv = std::llround(v);
        const bool neg = iv < 0;
        unsigned long long u = neg ? 0ull - static_cast<unsigned long long>(iv) : static_cast<unsigned long long>(iv);
        std::size_t at = cap;
        do {
            const int d = int(u % s.base);
            buf[--at] = char(d < 10 ? '0' + d : 'a' + d - 10);
            u /= s.base;
        } while (u && at > 1);
        if (neg) buf[--at] = '-';
        r = std::string_view(buf + at, cap - at);
    }
    return lcdCells(r) > s.digits ? std::string_view("E") : r;
}
struct LcdW {
    static constexpr std::string_view kName = "lcdnumber";
    static constexpr A11yRole kRole = A11yRole::Text;
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        const LcdState* s = lcdPool().get(ui, n);
        return {f32(s ? s->digits : 6u) * 22.f, 36.f};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const LcdState* s = lcdPool().get(pc.ui, pc.node);
        if (!s) return;
        char buf[80];
        const std::string_view t = lcdString(pc.ui, pc.node, *s, buf, sizeof buf);
        // cells: '.' attaches to the digit before it
        const u32 used = lcdCells(t), cells = std::max<u32>(s->digits, used);
        const Rect c = pc.content;
        const f32 cw = std::min(c.w / f32(std::max(1u, cells)), c.h * 0.75f), h = c.h, w = cw * 0.72f, th = std::max(1.5f, h * 0.09f);
        const Color on = pc.tint(pc.style.color), off = pc.tint(pc.style.color.withAlpha(pc.style.color.a * 0.08f));
        f32 x = c.right() - f32(std::min(cells, used)) * cw; // right-aligned
        for (std::size_t i = 0; i < t.size(); ++i) {
            const char ch = t[i];
            if (ch == '.') continue;
            const u8 m = sevenSegment(ch);
            const f32 ox = x + (cw - w) * 0.5f, oy = c.y, hh = h * 0.5f;
            const Rect seg[7] = {
                {ox + th, oy, w - 2 * th, th},                          // a
                {ox + w - th, oy + th, th, hh - 1.5f * th},             // b
                {ox + w - th, oy + hh + th * 0.5f, th, hh - 1.5f * th}, // c
                {ox + th, oy + h - th, w - 2 * th, th},                 // d
                {ox, oy + hh + th * 0.5f, th, hh - 1.5f * th},          // e
                {ox, oy + th, th, hh - 1.5f * th},                      // f
                {ox + th, oy + hh - th * 0.5f, w - 2 * th, th},         // g
            };
            if (ch == ':') {
                dl.rect({ox + w * 0.5f - th * 0.5f, oy + h * 0.3f, th, th}, on, th * 0.5f);
                dl.rect({ox + w * 0.5f - th * 0.5f, oy + h * 0.65f, th, th}, on, th * 0.5f);
            } else {
                for (int k = 0; k < 7; ++k) dl.rect(seg[k], (m >> k) & 1 ? on : off, th * 0.4f);
            }
            if (i + 1 < t.size() && t[i + 1] == '.') dl.rect({ox + w + th * 0.3f, oy + h - th, th, th}, on, th * 0.5f);
            x += cw;
        }
    }
};

// =====================================================================================================================
// Key sequence edit: text = the chord as shortcutToString writes it
struct KeySeqW {
    static constexpr std::string_view kName = "keysequenceedit";
    static constexpr A11yRole kRole = A11yRole::TextField;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) { return {140.f, lineH(ui, n)}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const std::string_view t = pc.ui.text(pc.node);
        const bool focus = pc.states & kStateFocus;
        const Color col = pc.tint(t.empty() ? placeholderColor(pc) : pc.style.color);
        pc.ui.paintString(dl, t.empty() ? std::string_view(focus ? "Press shortcut" : "None") : t, pc.content, col, pc.style.fontSize,
                          TextAlignment::Start, pc.node.index);
    }
    static void record(UiContext& ui, NodeHandle n, Shortcut s) {
        const std::string t = shortcutToString(s);
        ui.setText(n, t);
        ui.invalidate(n);
        ui.emit(n, SignalKind::TextChanged, {.text = t});
        if (t.empty() || !ui.alive(n)) return;
        ui.emit(n, SignalKind::Submitted, {.text = t});
        if (ui.alive(n)) ui.emit(n, SignalKind::EditingFinished, {.text = t});
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        if (e.type == EventType::FocusIn || e.type == EventType::FocusOut) { ec.ui.invalidate(ec.node); return false; } // hint text
        if (e.type == EventType::PointerDown) return true;
        if (e.type != EventType::KeyDown) return false;
        if (e.key == Key::Tab && !(e.mods & ~kModShift)) return false; // focus navigation
        if ((e.key == Key::Escape || e.key == Key::Backspace) && !e.mods) {
            if (!ec.ui.text(ec.node).empty()) record(ec.ui, ec.node, {});
            return true;
        }
        const Shortcut s{e.key, e.mods};
        if (!s.valid() || shortcutToString(s).empty()) return true; // a lone modifier / unnamed key: keep waiting
        record(ec.ui, ec.node, s);
        return true;
    }
};

// =====================================================================================================================
// Tool box: [header button, page]... as children; value = open page
// (headers are core "button" nodes with class "toolbox-header"; the open page's header is checked)
u32 toolBoxPages(const UiContext& ui, NodeHandle box) { return ui.childCount(box) / 2; }
bool isToolBox(const UiContext& ui, NodeHandle n) { return ui.alive(n) && ui.hasClass(n, "toolbox"); }
void toolBoxApply(UiContext& ui, NodeHandle box, u32 index) {
    const u32 pages = toolBoxPages(ui, box);
    for (u32 k = 0; k < pages; ++k) {
        ui.setCss(nth(ui, box, k * 2 + 1), "display", k == index ? "flex" : "none");
        const NodeHandle h = nth(ui, box, k * 2);
        if (k == index) ui.addClass(h, "open");
        else ui.removeClass(h, "open");
        ui.setChecked(h, k == index);
    }
}

// =====================================================================================================================
// MDI
constexpr f32 kMdiMinW = 80.f, kMdiMinH = 48.f;
struct MdiState {
    Rect geo;
    Vec2 grab;
    Rect grabGeo;
    u8 drag = 0; // 1 move, 2 resize
};
Pool<MdiState>& mdiPool() { static Pool<MdiState> p; return p; }
MdiState* mdiOf(const UiContext& ui, NodeHandle sub) { return isClass(ui, sub, "mdisubwindow") ? mdiPool().get(ui, sub) : nullptr; }
/// The title stays reachable: at least 40 px of it inside the area, never above its top edge.
Rect mdiClamp(const UiContext& ui, NodeHandle sub, Rect r) {
    r.w = std::max(kMdiMinW, std::round(r.w));
    r.h = std::max(kMdiMinH, std::round(r.h));
    const Rect a = ui.contentRect(ui.parent(sub));
    if (a.w > 0.f && a.h > 0.f) {
        r.x = std::clamp(r.x, 40.f - r.w, std::max(40.f - r.w, a.w - 40.f));
        r.y = std::clamp(r.y, 0.f, std::max(0.f, a.h - 24.f));
    }
    r.x = std::round(r.x);
    r.y = std::round(r.y);
    return r;
}
void mdiApply(UiContext& ui, NodeHandle sub, MdiState& s, Rect r) {
    s.geo = mdiClamp(ui, sub, r);
    px(ui, sub, Prop::Left, s.geo.x);
    px(ui, sub, Prop::Top, s.geo.y);
    px(ui, sub, Prop::Width, s.geo.w);
    px(ui, sub, Prop::Height, s.geo.h);
}
void mdiRaise(UiContext& ui, NodeHandle sub) {
    const NodeHandle area = ui.parent(sub);
    if (!ui.alive(area)) return;
    NodeHandle last, wasActive;
    for (NodeHandle c = ui.firstChild(area); ui.alive(c); c = ui.nextSibling(c)) {
        last = c;
        if (ui.hasClass(c, "active")) wasActive = c;
    }
    if (last != sub) ui.moveTo(sub, area);
    if (wasActive == sub) return;
    if (wasActive.valid()) {
        ui.removeClass(wasActive, "active");
        ui.emit(wasActive, SignalKind::Toggled, {.flag = false});
    }
    if (!ui.alive(sub)) return;
    ui.addClass(sub, "active");
    ui.emit(sub, SignalKind::Toggled, {.flag = true});
}
/// Title bar ("mdititle") and resize grip ("mdititle" with class "mdigrip") of a sub-window.
struct MdiTitleW {
    static constexpr std::string_view kName = "mdititle";
    static constexpr bool kCapturesDrag = true; // touch: the drag moves / resizes the sub-window
    static constexpr A11yRole kRole = A11yRole::None;
    static constexpr bool kInteractive = true;
    static bool grip(const UiContext& ui, NodeHandle n) { return ui.hasClass(n, "mdigrip"); }
    static Cursor cursor(UiContext& ui, NodeHandle n, Vec2) { return grip(ui, n) ? Cursor::ResizeNWSE : Cursor::Auto; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        if (!grip(pc.ui, pc.node)) return;
        const Rect r = pc.rect;
        const Color c = pc.tint(pc.style.color.withAlpha(pc.style.color.a * ((pc.states & (kStateHover | kStatePressed)) ? 0.9f : 0.45f)));
        for (int k = 0; k < 3; ++k) { // diagonal hatch in the corner
            const f32 d = 3.f + f32(k) * 4.f;
            dl.line({r.right() - d, r.bottom() - 2.f}, {r.right() - 2.f, r.bottom() - d}, c, 1.f);
        }
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        UiContext& ui = ec.ui;
        if (ec.phase != Phase::Target) return false;
        const NodeHandle sub = ui.parent(ec.node);
        MdiState* s = mdiOf(ui, sub);
        if (!s) return false;
        if (e.type == EventType::PointerDown) {
            if (e.button != MouseButton::Left) return false;
            mdiRaise(ui, sub);
            s = mdiOf(ui, sub); // (a Toggled slot may have closed it)
            if (!s) return true;
            s->drag = grip(ui, ec.node) ? 2 : 1;
            s->grab = e.pos;
            s->grabGeo = s->geo;
            return true;
        }
        if (e.type == EventType::PointerMove && s->drag && ui.pressed() == ec.node) {
            Rect r = s->grabGeo;
            const f32 dx = e.pos.x - s->grab.x, dy = e.pos.y - s->grab.y;
            if (s->drag == 1) { r.x += dx; r.y += dy; }
            else { r.w = std::max(kMdiMinW, r.w + dx); r.h = std::max(kMdiMinH, r.h + dy); }
            mdiApply(ui, sub, *s, r);
            return true;
        }
        if (e.type == EventType::PointerUp) {
            if (e.cancel && s->drag) mdiApply(ui, sub, *s, s->grabGeo); // a cancelled touch puts it back
            s->drag = 0;
            return false;
        }
        return false;
    }
};
struct MdiSubW {
    static constexpr std::string_view kName = "mdisubwindow";
    static constexpr A11yRole kRole = A11yRole::Window;
    static constexpr bool kInteractive = true;
    static bool onEvent(EventContext& ec, const Event& e) { // a press anywhere inside raises (seen before the target)
        if (e.type == EventType::PointerDown && ec.phase != Phase::Bubble) mdiRaise(ec.ui, ec.node);
        return false;
    }
};
struct MdiAreaW {
    static constexpr std::string_view kName = "mdiarea";
    static constexpr A11yRole kRole = A11yRole::Group;
    static ComputedStyle defaults() {
        ComputedStyle s;
        s.layout = LayoutKind::Absolute;
        s.overflow = Overflow::Clip;
        return s;
    }
};

// =====================================================================================================================
// Graphics view
struct ViewState {
    GraphicsScene* scene = nullptr;
    f32 zoom = 1.f;
    Vec2 pan;
    u8 mode = 0; // 1 move items, 2 pan, 3 rubber band
    Vec2 last, bandA, bandB;
    Path path; ///< scratch for ellipse / polygon items (kept, so a paint does not allocate)
};
Pool<ViewState>& viewPool() { static Pool<ViewState> p; return p; }
ViewState* viewOf(const UiContext& ui, NodeHandle n) { return isClass(ui, n, "graphicsview") ? viewPool().get(ui, n) : nullptr; }
Rect itemBounds(const GraphicsItem& it) {
    if (it.shape == GraphicsShape::Line)
        return {std::min(it.rect.x, it.rect.right()), std::min(it.rect.y, it.rect.bottom()), std::abs(it.rect.w), std::abs(it.rect.h)};
    if (it.shape == GraphicsShape::Polygon && !it.points.empty()) {
        f32 x0 = it.points[0].x, y0 = it.points[0].y, x1 = x0, y1 = y0;
        for (const Vec2& p : it.points) { x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y); }
        return {x0, y0, x1 - x0, y1 - y0};
    }
    return it.rect;
}
/// Strict overlap; a degenerate rect (a horizontal / vertical line's bounds) counts as a hairline.
bool overlaps(const Rect& a, const Rect& b) {
    const f32 aw = std::max(a.w, 1e-3f), ah = std::max(a.h, 1e-3f), bw = std::max(b.w, 1e-3f), bh = std::max(b.h, 1e-3f);
    return a.x < b.x + bw && b.x < a.x + aw && a.y < b.y + bh && b.y < a.y + ah;
}
struct GraphicsViewW {
    static constexpr std::string_view kName = "graphicsview";
    static constexpr bool kCapturesDrag = true; // touch: the drag moves items / selects
    static constexpr A11yRole kRole = A11yRole::Group;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {400.f, 300.f}; }
    static Cursor cursor(UiContext& ui, NodeHandle n, Vec2) {
        const ViewState* v = viewPool().get(ui, n);
        return v && (v->mode == 1 || v->mode == 2) ? Cursor::Move : v && v->mode == 3 ? Cursor::Crosshair : Cursor::Auto;
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        ViewState* v = viewPool().get(pc.ui, pc.node);
        if (!v || !v->scene) return;
        const Rect c = pc.content;
        const f32 z = v->zoom;
        auto T = [&](Vec2 s) { return Vec2{c.x + (s.x - v->pan.x) * z, c.y + (s.y - v->pan.y) * z}; };
        auto TR = [&](const Rect& r) { const Vec2 a = T({r.x, r.y}); return Rect{a.x, a.y, r.w * z, r.h * z}; };
        dl.pushClip(c);
        const Rect visible{v->pan.x, v->pan.y, c.w / z, c.h / z};
        Path& path = v->path;
        for (u32 id : v->scene->orderedItems()) {
            const GraphicsItem& it = *v->scene->item(id);
            if (!it.visible) continue;
            const Rect bb = itemBounds(it);
            if (!overlaps(bb, visible)) continue;
            const Rect r = TR(it.rect);
            const Color fill = pc.tint(it.fill), stroke = pc.tint(it.stroke);
            const f32 sw = std::max(1.f, it.strokeWidth * z);
            switch (it.shape) {
            case GraphicsShape::Rect: dl.roundedRect(r, fill, {0, 0, 0, 0}, stroke, it.strokeWidth > 0 ? sw : 0.f); break;
            case GraphicsShape::Ellipse: {
                const f32 rad = std::min(r.w, r.h) * 0.5f;
                if (std::abs(r.w - r.h) < 0.5f) { // a circle is a rounded rect (one SDF command)
                    dl.roundedRect(r, fill, {rad, rad, rad, rad}, stroke, it.strokeWidth > 0 ? sw : 0.f);
                } else {
                    path.clear();
                    path.addEllipse({r.x + r.w * 0.5f, r.y + r.h * 0.5f}, r.w * 0.5f, r.h * 0.5f);
                    if (fill.a > 0.f) dl.fillPath(path, Brush(fill));
                    if (it.strokeWidth > 0.f && stroke.a > 0.f) dl.strokePath(path, StrokeStyle{.width = sw}, Brush(stroke));
                }
                break;
            }
            case GraphicsShape::Line: dl.line(T({it.rect.x, it.rect.y}), T({it.rect.right(), it.rect.bottom()}), stroke, sw); break;
            case GraphicsShape::Text:
                pc.ui.paintString(dl, it.text, r, fill, std::max(4.f, pc.style.fontSize * z), TextAlignment::Start, pc.node.index);
                break;
            case GraphicsShape::Polygon: {
                if (it.points.size() < 2) break;
                path.clear();
                path.moveTo(T(it.points[0]));
                for (std::size_t i = 1; i < it.points.size(); ++i) path.lineTo(T(it.points[i]));
                path.close();
                if (fill.a > 0.f && it.points.size() > 2) dl.fillPath(path, Brush(fill), FillRule::EvenOdd); // as the hit test
                if (it.strokeWidth > 0.f && stroke.a > 0.f) dl.strokePath(path, StrokeStyle{.width = sw, .join = LineJoin::Round}, Brush(stroke));
                break;
            }
            }
            if (it.selected) {
                const Rect b = TR(bb);
                dl.roundedRect({b.x - 3, b.y - 3, b.w + 6, b.h + 6}, {}, {0, 0, 0, 0}, pc.tint(themed(pc, tok::item_focus, 0xFFD54F)), 1.5f);
            }
        }
        if (v->mode == 3) {
            const Vec2 a = v->bandA, b = v->bandB;
            const Rect band{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(a.x - b.x), std::abs(a.y - b.y)};
            const Color acc = pc.style.background2.a > 0.f ? pc.style.background2 : themed(pc, tok::accent, 0x3D8BFD);
            dl.roundedRect(band, pc.tint(acc.withAlpha(0.2f)), {0, 0, 0, 0}, pc.tint(acc), 1.f);
        }
        dl.popClip();
        if ((pc.states & kStateFocus) && pc.ui.focusVisible()) {
            const Color acc = pc.style.background2.a > 0.f ? pc.style.background2 : themed(pc, tok::accent, 0x3D8BFD);
            dl.roundedRect(pc.rect, {}, {2.f, 2.f, 2.f, 2.f}, pc.tint(acc), 1.f);
        }
    }
    static void selectionChanged(UiContext& ui, NodeHandle n, const GraphicsScene& sc) {
        const u32 last = sc.lastSelected();
        ui.emit(n, SignalKind::ValueChanged, {.value = last == ~0u ? -1.f : f32(last)});
    }
    /// Moves the selected movable items (no temporary list: the paint order does not change while items move).
    static void moveSelection(GraphicsScene& sc, Vec2 d) {
        for (u32 id : sc.orderedItems())
            if (const GraphicsItem* it = sc.item(id); it && it->selected && it->movable) sc.move(id, d);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        ViewState* v = viewPool().get(ui, ec.node);
        if (!v || !v->scene) return false;
        GraphicsScene& sc = *v->scene;
        const Vec2 sp = viewToScene(ui, ec.node, e.pos);
        switch (e.type) {
        case EventType::Wheel: {
            const f32 nz = std::clamp(v->zoom * std::pow(1.15f, e.wheel.y), 0.05f, 40.f);
            const Rect c = ui.contentRect(ec.node); // the scene point under the pointer stays where it is
            v->pan = {sp.x - (e.pos.x - c.x) / nz, sp.y - (e.pos.y - c.y) / nz};
            v->zoom = nz;
            ui.invalidate(ec.node);
            return true;
        }
        case EventType::PointerDown: {
            v->last = e.pos;
            if (e.button == MouseButton::Middle || (e.button == MouseButton::Left && (e.mods & kModAlt))) { v->mode = 2; return true; }
            if (e.button != MouseButton::Left) return false;
            const u32 hit = sc.itemAt(sp);
            if (hit != ~0u) {
                const GraphicsItem* it = sc.item(hit);
                if (it->selectable) {
                    const bool was = it->selected;
                    if (e.mods & kModCtrl) sc.select(hit, !was);
                    else if (!was) sc.select(hit, true, true);
                    if ((e.mods & kModCtrl) || !was) selectionChanged(ui, ec.node, sc);
                }
                if (e.clicks >= 2) ui.emit(ec.node, SignalKind::Submitted, {.value = f32(hit)});
                if ((v = viewPool().get(ui, ec.node))) v->mode = 1;
            } else {
                if (!(e.mods & kModCtrl) && sc.selectedCount()) { sc.clearSelection(); selectionChanged(ui, ec.node, sc); }
                if ((v = viewPool().get(ui, ec.node))) {
                    v->mode = 3;
                    v->bandA = v->bandB = e.pos;
                }
            }
            if (ui.alive(ec.node)) ui.invalidate(ec.node);
            return true;
        }
        case EventType::PointerMove: {
            if (!v->mode || ui.pressed() != ec.node) return false;
            const Vec2 d{(e.pos.x - v->last.x) / v->zoom, (e.pos.y - v->last.y) / v->zoom};
            v->last = e.pos;
            if (v->mode == 2) v->pan = {v->pan.x - d.x, v->pan.y - d.y};
            else if (v->mode == 1) moveSelection(sc, d);
            else if (v->mode == 3) v->bandB = e.pos;
            ui.invalidate(ec.node);
            return true;
        }
        case EventType::PointerUp: {
            const u8 mode = v->mode;
            v->mode = 0;
            ui.invalidate(ec.node);
            if (mode == 3 && !e.cancel) {
                const Vec2 a = viewToScene(ui, ec.node, v->bandA), b = viewToScene(ui, ec.node, v->bandB);
                const Rect r{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(a.x - b.x), std::abs(a.y - b.y)};
                bool any = false;
                if (r.w > 1 && r.h > 1)
                    for (u32 id : sc.orderedItems()) { // (as itemsIn(r), without the list)
                        const GraphicsItem* it = sc.item(id);
                        if (!it->visible || !it->selectable || it->selected || !overlaps(itemBounds(*it), r)) continue;
                        sc.select(id, true);
                        any = true;
                    }
                if (any) selectionChanged(ui, ec.node, sc);
            }
            return false;
        }
        case EventType::KeyDown: {
            const f32 step = 10.f / v->zoom;
            Vec2 d{};
            switch (e.key) {
            case Key::Left: d.x = -step; break;
            case Key::Right: d.x = step; break;
            case Key::Up: d.y = -step; break;
            case Key::Down: d.y = step; break;
            case Key::Delete: {
                const std::vector<u32> sel = sc.selection();
                if (sel.empty()) return false;
                for (u32 id : sel) sc.remove(id);
                ui.invalidate(ec.node);
                selectionChanged(ui, ec.node, sc);
                return true;
            }
            case Key::A:
                if (!(e.mods & kModCtrl)) return false;
                for (u32 id : sc.paintOrder())
                    if (sc.item(id)->selectable && sc.item(id)->visible) sc.select(id, true);
                ui.invalidate(ec.node);
                selectionChanged(ui, ec.node, sc);
                return true;
            default: return false;
            }
            if (!sc.selectedCount()) return false;
            moveSelection(sc, d);
            ui.invalidate(ec.node);
            return true;
        }
        default: return false;
        }
    }
};

// =====================================================================================================================
// Virtual keyboard
struct VkState {
    NodeHandle kb, target;
    bool shift = false, caps = false, symbols = false;
    std::vector<NodeHandle> keys;                           ///< the character keys, row-major
    std::map<std::string, NodeHandle, std::less<>> named;   ///< Shift, Backspace, ?123, Space, Enter
};
Pool<VkState>& vkPool() { static Pool<VkState> p; return p; }
VkState* vkOf(const UiContext& ui, NodeHandle kb) { return isClass(ui, kb, "vkeyboard") ? vkPool().get(ui, kb) : nullptr; }
constexpr std::string_view kVkLetters[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
constexpr std::string_view kVkSymbols[3] = {"1234567890", "@#$%&-+()/", "*\"':;!?,."};
std::string vkLabel(const VkState& st, std::size_t row, std::size_t col) {
    const std::string_view r = st.symbols ? kVkSymbols[row] : kVkLetters[row];
    if (col >= r.size()) return {};
    char c = r[col];
    if (!st.symbols && (st.shift || st.caps) && c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return std::string(1, c);
}
void vkRelabel(UiContext& ui, VkState& st) {
    std::size_t i = 0;
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t col = 0; col < std::max(kVkLetters[row].size(), kVkSymbols[row].size()); ++col, ++i) {
            if (i >= st.keys.size()) return;
            const std::string l = vkLabel(st, row, col);
            ui.setText(st.keys[i], l);
            ui.setCss(st.keys[i], "display", l.empty() ? "none" : "flex");
        }
    ui.setText(st.named["?123"], st.symbols ? "ABC" : "?123");
    const NodeHandle sh = st.named["Shift"];
    ui.setChecked(sh, st.shift || st.caps);
    if (st.caps) ui.addClass(sh, "caps");
    else ui.removeClass(sh, "caps");
}
/// True for a focused node that takes text: it showed a text caret, or it is a text field by role.
bool vkTakesText(const UiContext& ui, NodeHandle f) { return ui.alive(f) && (ui.wantsTextInput() || ui.role(f) == A11yRole::TextField); }
/// Remembers the focused node when the keyboard can type into it (it takes text and is outside the keyboard).
void vkNoteFocus(const UiContext& ui, VkState& st) {
    const NodeHandle f = ui.focused();
    if (vkTakesText(ui, f) && !inSubtree(ui, st.kb, f)) st.target = f;
}
/// Focus back on the target (a press on a key took it away: keys are not focusable); false when there is none.
bool vkResolve(UiContext& ui, NodeHandle kb) {
    VkState* st = vkOf(ui, kb);
    if (!st) return false;
    vkNoteFocus(ui, *st);
    if (!ui.alive(st->target)) return false;
    if (ui.focused() != st->target) ui.setFocus(st->target, FocusReason::Programmatic);
    return ui.focused() == st->target;
}
struct VKeyboardW {
    static constexpr std::string_view kName = "vkeyboard";
    static constexpr A11yRole kRole = A11yRole::Group;
    static void paint(const PaintContext& pc, DrawList&) { // (remembers who is being typed into, while it is shown)
        if (VkState* st = vkPool().get(pc.ui, pc.node)) vkNoteFocus(pc.ui, *st);
    }
};

// =====================================================================================================================
// accessibility
void lcdA11y(const UiContext& ui, NodeHandle n, A11ySnapshotNode& self, std::vector<A11ySnapshotNode>&) {
    char buf[80];
    if (const LcdState* s = lcdPool().get(ui, n)) self.valueText.assign(lcdString(ui, n, *s, buf, sizeof buf));
}
void keySeqA11y(const UiContext& ui, NodeHandle n, A11ySnapshotNode& self, std::vector<A11ySnapshotNode>&) {
    detail::a11yDropTextName(ui, n, self);
    self.valueText.assign(ui.text(n));
    if (!self.disabled) self.actions |= detail::a11yBits({A11yAction::SetValue});
}
bool keySeqA11yAction(UiContext& ui, NodeHandle n, u32 child, A11yAction a, f32, std::string_view text) {
    if (child || a != A11yAction::SetValue) return false;
    Shortcut s;
    if (!text.empty() && !parseShortcut(text, s)) return true; // not a shortcut: ignored (never stored as raw text)
    KeySeqW::record(ui, n, s);
    return true;
}
// Graphics view: one child per visible item (id = item id + 1).
void graphicsA11y(const UiContext& cui, NodeHandle n, A11ySnapshotNode& self, std::vector<A11ySnapshotNode>& items) {
    UiContext& ui = const_cast<UiContext&>(cui);
    const ViewState* v = viewPool().get(ui, n);
    if (!v || !v->scene) return;
    const Rect c = ui.contentRect(n);
    const std::vector<u32> order = v->scene->paintOrder();
    u32 pos = 0, shown = 0;
    for (u32 id : order) shown += v->scene->item(id)->visible ? 1u : 0u;
    for (u32 id : order) {
        const GraphicsItem& it = *v->scene->item(id);
        if (!it.visible) continue;
        const Rect bb = itemBounds(it);
        const Vec2 a = sceneToView(ui, n, {bb.x, bb.y});
        A11ySnapshotNode item = detail::a11yItem(id + 1, it.shape == GraphicsShape::Text ? A11yRole::Text : A11yRole::Image, it.text,
                                                 Rect{a.x, a.y, bb.w * v->zoom, bb.h * v->zoom}.intersect(c),
                                                 !self.disabled && it.selectable ? detail::a11yBits({A11yAction::Select, A11yAction::Click}) : 0u);
        item.selectable = it.selectable;
        item.selected = it.selected;
        item.posInSet = ++pos;
        item.setSize = shown;
        items.push_back(std::move(item));
    }
}
bool graphicsA11yAction(UiContext& ui, NodeHandle n, u32 child, A11yAction a, f32, std::string_view) {
    ViewState* v = viewPool().get(ui, n);
    if (!v || !v->scene || !child || (a != A11yAction::Select && a != A11yAction::Click)) return false;
    const GraphicsItem* it = v->scene->item(child - 1);
    if (!it || !it->selectable) return false;
    v->scene->select(child - 1, true, true);
    ui.invalidate(n);
    GraphicsViewW::selectionChanged(ui, n, *v->scene);
    return true;
}
} // namespace

void registerExtraWidgets(UiContext& ui) {
    WidgetRegistry& r = ui.registry();
    // class by class: a context trimmed to a document's registration list may hold only some of them
    bool added = detail::registerMissing<LcdW>(r);
    added = detail::registerMissing<KeySeqW>(r) || added;
    added = detail::registerMissing<MdiTitleW>(r) || added;
    added = detail::registerMissing<MdiSubW>(r) || added;
    added = detail::registerMissing<MdiAreaW>(r) || added;
    added = detail::registerMissing<GraphicsViewW>(r) || added;
    added = detail::registerMissing<VKeyboardW>(r) || added;
    if (!added) return;
    detail::a11yHook(r, "lcdnumber", A11yRole::Text, &lcdA11y);
    detail::a11yHook(r, "keysequenceedit", A11yRole::TextField, &keySeqA11y, &keySeqA11yAction);
    detail::a11yHook(r, "graphicsview", A11yRole::Group, &graphicsA11y, &graphicsA11yAction);
}

// ---- dialog default / reject buttons ---------------------------------------------------------------------------------
namespace {
void setDialogButtonClass(UiContext& ui, NodeHandle dlg, NodeHandle button, std::string_view cls) {
    const NodeHandle scope = ui.parent(dlg);
    if (!ui.alive(scope) || !ui.hasClass(dlg, "dialog")) return;
    for (NodeHandle old = findClass(ui, scope, cls); old.valid(); old = findClass(ui, scope, cls)) ui.removeClass(old, cls);
    if (ui.alive(button) && inSubtree(ui, dlg, button)) ui.addClass(button, cls);
}
/// The enabled, shown button of class `cls` in a dialog scope.
NodeHandle dialogButton(const UiContext& ui, NodeHandle scope, std::string_view cls) {
    const NodeHandle b = ui.alive(scope) ? findClass(ui, scope, cls) : NodeHandle{};
    if (!b.valid() || ui.disabled(b)) return {};
    return ui.computed(b).visible ? b : NodeHandle{};
}
} // namespace
void setDialogDefaultButton(UiContext& ui, NodeHandle dlg, NodeHandle button) { setDialogButtonClass(ui, dlg, button, "dialog-default"); }
void setDialogRejectButton(UiContext& ui, NodeHandle dlg, NodeHandle button) { setDialogButtonClass(ui, dlg, button, "dialog-reject"); }
NodeHandle dialogDefaultButton(const UiContext& ui, NodeHandle dlg) { return ui.alive(dlg) ? findClass(ui, dlg, "dialog-default") : NodeHandle{}; }
NodeHandle dialogRejectButton(const UiContext& ui, NodeHandle dlg) { return ui.alive(dlg) ? findClass(ui, dlg, "dialog-reject") : NodeHandle{}; }
namespace detail {
bool dialogKey(UiContext& ui, NodeHandle scope, const Event& e, Phase phase) {
    if (e.type != EventType::KeyDown || !ui.hasClass(scope, "modal")) return false;
    if (e.key == Key::Escape) { // QDialog::reject: the reject button's slots decide; without one the popup closes
        const NodeHandle r = dialogButton(ui, scope, "dialog-reject");
        if (!r.valid()) return false;
        ui.emit(r, SignalKind::Clicked, {});
        return true;
    }
    if (e.key == Key::Enter && phase != Phase::Capture && !(e.mods & (kModCtrl | kModAlt | kModSuper))) {
        const NodeHandle d = dialogButton(ui, scope, "dialog-default");
        if (!d.valid()) return false;
        // a focused push button presses itself (QPushButton::autoDefault): the core activates it after this
        if (const NodeHandle f = ui.focused(); ui.alive(f)) {
            const std::string_view cls = ui.registry().get(ui.classOf(f)).name;
            if (cls == "button" || cls == "toolbutton" || cls == "link" || cls == "menubutton" || cls == "combodrop") return false;
        }
        ui.emit(d, SignalKind::Clicked, {});
        return true;
    }
    return false;
}
} // namespace detail

// ---- wizard validator ------------------------------------------------------------------------------------------------------
namespace {
struct WizardCheck { u64 uid = 0; NodeHandle wizard; WizardValidator fn = nullptr; void* user = nullptr; };
std::vector<WizardCheck>& wizardChecks() { static std::vector<WizardCheck> v; return v; }
} // namespace
void setWizardValidator(UiContext& ui, NodeHandle wizard, WizardValidator fn, void* user) {
    auto& all = wizardChecks();
    std::erase_if(all, [&](const WizardCheck& c) { return c.uid == ui.uid() ? (c.wizard == wizard || !ui.alive(c.wizard)) : !UiContext::exists(c.uid); });
    if (fn && ui.alive(wizard) && ui.hasClass(wizard, "wizard")) all.push_back({ui.uid(), wizard, fn, user});
}
namespace detail {
bool wizardCanLeave(UiContext& ui, NodeHandle wizard, u32 page) {
    for (const WizardCheck& c : wizardChecks())
        if (c.uid == ui.uid() && c.wizard == wizard) return c.fn(ui, wizard, page, c.user);
    return true;
}
} // namespace detail

// ---- input dialog --------------------------------------------------------------------------------------------------------
NodeHandle inputDialog(UiContext& ui, std::string_view title, std::string_view prompt, std::string_view initial, std::string_view id) {
    const NodeHandle dlg = dialog(ui, title, id);
    if (!ui.alive(dlg)) return dlg;
    ui.addClass(dlg, "inputdialog");
    const NodeHandle body = dialogBody(ui, dlg);
    label(ui, body, prompt);
    const NodeHandle le = lineEdit(ui, body, initial);
    ui.addClass(le, "inputdialog-edit");
    ui.setA11y(le, A11yRole::TextField, prompt);
    const NodeHandle btns = dialogButtonBox(ui, body, StdButton::Ok | StdButton::Cancel);
    const NodeHandle ok = dialogButtonBoxButton(ui, btns, StdButton::Ok), cancel = dialogButtonBoxButton(ui, btns, StdButton::Cancel);
    auto accept = [dlg, le](UiContext& c, NodeHandle, const SignalArgs&) {
        if (!c.alive(dlg) || !c.alive(le)) return;
        if (lineEditState(c, le) != Validation::Acceptable) return; // a validator on the field gates OK
        const std::string t(lineEditText(c, le));
        closeDialog(c, dlg);
        c.emit(dlg, SignalKind::Submitted, {.value = 1.f, .text = t});
    };
    ui.connect(ok, SignalKind::Clicked, accept);
    ui.connect(le, SignalKind::Submitted, accept);
    ui.connect(cancel, SignalKind::Clicked, [dlg](UiContext& c, NodeHandle, const SignalArgs&) {
        if (!c.alive(dlg)) return;
        closeDialog(c, dlg);
        c.emit(dlg, SignalKind::Submitted, {.value = 0.f});
    });
    openDialog(ui, dlg);
    ui.setFocus(le, FocusReason::Keyboard);
    ui.editBuffer(le).selectAll(); // QInputDialog: the initial text is selected
    return dlg;
}
NodeHandle inputDialogEdit(const UiContext& ui, NodeHandle dlg) {
    return ui.alive(dlg) && ui.hasClass(dlg, "inputdialog") ? findClass(ui, dlg, "inputdialog-edit") : NodeHandle{};
}

// ---- tool box ----------------------------------------------------------------------------------------------------------------
NodeHandle toolBoxAddPage(UiContext& ui, NodeHandle box, std::string_view title) {
    if (!isToolBox(ui, box)) return {};
    const u32 index = toolBoxPages(ui, box);
    const NodeHandle h = button(ui, box, title);
    ui.addClass(h, "toolbox-header");
    const NodeHandle p = panel(ui, box);
    ui.addClass(p, "toolbox-page");
    ui.setA11y(p, A11yRole::Group, title);
    ui.connect(h, SignalKind::Clicked, [box](UiContext& c, NodeHandle self, const SignalArgs&) {
        u32 i = 0;
        for (NodeHandle k = c.firstChild(box); c.alive(k); k = c.nextSibling(k), ++i)
            if (k == self) { setToolBoxIndex(c, box, i / 2); return; }
    });
    // Up / Down walk the headers (the pages in between are skipped)
    ui.connect(h, SignalKind::Event, [box](UiContext& c, NodeHandle self, const SignalArgs& a) {
        if (!a.event || a.event->type != EventType::KeyDown || (a.event->key != Key::Up && a.event->key != Key::Down)) return;
        NodeHandle prev, next;
        bool seen = false;
        for (NodeHandle k = c.firstChild(box); c.alive(k); k = c.nextSibling(k)) {
            if (!c.hasClass(k, "toolbox-header")) continue;
            if (k == self) seen = true;
            else if (!seen) prev = k;
            else if (!next.valid()) next = k;
        }
        const NodeHandle to = a.event->key == Key::Up ? prev : next;
        if (to.valid()) c.setFocus(to, FocusReason::Keyboard);
    });
    toolBoxApply(ui, box, index == 0 ? 0u : u32(std::max(0.f, ui.value(box))));
    ui.setRange(box, 0.f, f32(index), 1.f);
    return p;
}
NodeHandle toolBox(UiContext& ui, NodeHandle parent, std::string_view titles, std::string_view id) {
    const NodeHandle box = panel(ui, parent, id);
    if (!ui.alive(box)) return box;
    ui.addClass(box, "toolbox");
    ui.setRange(box, 0.f, 0.f, 1.f);
    eachLine(titles, [&](u32, std::string_view t) { toolBoxAddPage(ui, box, t); });
    return box;
}
NodeHandle toolBoxPage(const UiContext& ui, NodeHandle box, u32 index) { return isToolBox(ui, box) ? nth(ui, box, index * 2 + 1) : NodeHandle{}; }
NodeHandle toolBoxHeader(const UiContext& ui, NodeHandle box, u32 index) { return isToolBox(ui, box) ? nth(ui, box, index * 2) : NodeHandle{}; }
u32 toolBoxCount(const UiContext& ui, NodeHandle box) { return isToolBox(ui, box) ? toolBoxPages(ui, box) : 0u; }
void setToolBoxIndex(UiContext& ui, NodeHandle box, u32 index) {
    if (!isToolBox(ui, box) || index >= toolBoxPages(ui, box)) return;
    toolBoxApply(ui, box, index);
    if (u32(ui.value(box)) != index) ui.setValue(box, f32(index), true);
}

// ---- LCD API ------------------------------------------------------------------------------------------------------------------
u8 sevenSegment(char c) {
    switch (std::tolower(static_cast<unsigned char>(c))) {
    case '0': case 'o': return 0x3F; case '1': return 0x06; case '2': return 0x5B; case '3': return 0x4F; case '4': return 0x66;
    case '5': case 's': return 0x6D; case '6': return 0x7D; case '7': return 0x07; case '8': return 0x7F; case '9': return 0x6F;
    case 'a': return 0x77; case 'b': return 0x7C; case 'c': return 0x39; case 'd': return 0x5E; case 'e': return 0x79;
    case 'f': return 0x71; case 'h': return 0x76; case 'l': return 0x38; case 'p': return 0x73; case 'u': return 0x3E;
    case 'r': return 0x50; case 'n': return 0x54; case 'y': return 0x6E; case '-': return 0x40; case '_': return 0x08;
    default: return 0;
    }
}
NodeHandle lcdNumber(UiContext& ui, NodeHandle parent, u32 digits, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle n = make(ui, "lcdnumber", parent, id);
    if (!ui.alive(n)) return n;
    lcdPool().create(ui, n).digits = std::clamp(digits, 1u, 32u);
    ui.setRange(n, -1e15f, 1e15f, 0.f);
    return n;
}
void setLcdText(UiContext& ui, NodeHandle lcd, std::string_view t) {
    if (LcdState* s = lcdOf(ui, lcd)) { s->text = std::string(t); s->explicitText = true; ui.invalidate(lcd); }
}
void setLcdValue(UiContext& ui, NodeHandle lcd, f32 value) {
    if (LcdState* s = lcdOf(ui, lcd)) { s->explicitText = false; ui.setValue(lcd, value, true); ui.invalidate(lcd); }
}
void setLcdBase(UiContext& ui, NodeHandle lcd, u32 base) {
    if (LcdState* s = lcdOf(ui, lcd)) {
        s->base = (base == 2 || base == 8 || base == 16) ? base : 10;
        s->explicitText = false;
        ui.invalidate(lcd);
    }
}
std::string lcdText(const UiContext& ui, NodeHandle lcd) {
    const LcdState* s = lcdOf(ui, lcd);
    char buf[80];
    return s ? std::string(lcdString(ui, lcd, *s, buf, sizeof buf)) : std::string();
}

// ---- key sequence API ------------------------------------------------------------------------------------------------------
NodeHandle keySequenceEdit(UiContext& ui, NodeHandle parent, Shortcut initial, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle n = make(ui, "keysequenceedit", parent, id);
    if (ui.alive(n)) ui.setText(n, shortcutToString(initial));
    return n;
}
Shortcut keySequence(const UiContext& ui, NodeHandle edit) {
    Shortcut s;
    if (isClass(ui, edit, "keysequenceedit")) (void)parseShortcut(ui.text(edit), s);
    return s;
}
void setKeySequence(UiContext& ui, NodeHandle edit, Shortcut s) {
    if (!isClass(ui, edit, "keysequenceedit")) return;
    ui.setText(edit, shortcutToString(s));
    ui.invalidate(edit);
}

// ---- MDI API ------------------------------------------------------------------------------------------------------------------
NodeHandle mdiArea(UiContext& ui, NodeHandle parent, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle a = make(ui, "mdiarea", parent, id);
    if (ui.alive(a)) ui.setCss(a, "flex-grow", "1");
    return a;
}
NodeHandle mdiSubWindow(UiContext& ui, NodeHandle area, std::string_view title, Rect g, std::string_view id) {
    registerExtraWidgets(ui);
    if (!isClass(ui, area, "mdiarea")) return {};
    const NodeHandle sub = make(ui, "mdisubwindow", area, id);
    if (!ui.alive(sub)) return sub;
    ui.setA11y(sub, A11yRole::Window, title);
    MdiState& st = mdiPool().create(ui, sub);
    const NodeHandle bar = make(ui, "mdititle", sub);
    const NodeHandle t = label(ui, bar, title);
    ui.setCss(t, "flex-grow", "1");
    ui.setCss(t, "pointer-events", "none");
    const NodeHandle x = toolButton(ui, bar, "x");
    ui.addClass(x, "mdiclose");
    ui.setA11y(x, A11yRole::Button, "Close");
    ui.connect(x, SignalKind::Clicked, [sub](UiContext& c, NodeHandle, const SignalArgs&) { mdiClose(c, sub); });
    const NodeHandle body = panel(ui, sub);
    ui.addClass(body, "mdibody");
    ui.setCss(body, "flex-grow", "1");
    const NodeHandle grip = make(ui, "mdititle", sub);
    ui.addClass(grip, "mdigrip");
    mdiApply(ui, sub, st, g);
    mdiRaise(ui, sub);
    return sub;
}
NodeHandle mdiBody(const UiContext& ui, NodeHandle sub) { return mdiOf(ui, sub) ? nth(ui, sub, 1) : NodeHandle{}; }
Rect mdiGeometry(const UiContext& ui, NodeHandle sub) {
    const MdiState* s = mdiOf(ui, sub);
    return s ? s->geo : Rect{};
}
void setMdiGeometry(UiContext& ui, NodeHandle sub, Rect r) {
    if (MdiState* s = mdiOf(ui, sub)) mdiApply(ui, sub, *s, r);
}
void mdiActivate(UiContext& ui, NodeHandle sub) {
    if (mdiOf(ui, sub)) mdiRaise(ui, sub);
}
void mdiClose(UiContext& ui, NodeHandle sub) {
    if (!mdiOf(ui, sub)) return;
    const NodeHandle area = ui.parent(sub);
    ui.emit(sub, SignalKind::Submitted, {.value = 0.f});
    if (!ui.alive(sub)) return;
    if (inSubtree(ui, sub, ui.focused())) ui.setFocus({});
    ui.destroy(sub);
    if (const NodeHandle next = mdiActive(ui, area); next.valid()) mdiRaise(ui, next); // the one below takes over
}
std::vector<NodeHandle> mdiSubWindows(const UiContext& ui, NodeHandle area) {
    std::vector<NodeHandle> v;
    if (!isClass(ui, area, "mdiarea")) return v;
    for (NodeHandle c = ui.firstChild(area); ui.alive(c); c = ui.nextSibling(c))
        if (mdiOf(ui, c)) v.push_back(c);
    return v;
}
NodeHandle mdiActive(const UiContext& ui, NodeHandle area) {
    const auto v = mdiSubWindows(ui, area);
    return v.empty() ? NodeHandle{} : v.back();
}
void mdiTile(UiContext& ui, NodeHandle area) {
    const auto v = mdiSubWindows(ui, area);
    if (v.empty()) return;
    const Rect c = ui.contentRect(area);
    const u32 n = u32(v.size()), cols = u32(std::ceil(std::sqrt(f32(n)))), rows = (n + cols - 1) / cols;
    const f32 w = std::floor(c.w / f32(cols)), h = std::floor(c.h / f32(rows));
    for (u32 i = 0; i < n; ++i) setMdiGeometry(ui, v[i], {f32(i % cols) * w, f32(i / cols) * h, w, h});
}
void mdiCascade(UiContext& ui, NodeHandle area) {
    const auto v = mdiSubWindows(ui, area);
    const Rect c = ui.contentRect(area);
    for (u32 i = 0; i < v.size(); ++i)
        setMdiGeometry(ui, v[i], {f32(i) * 24.f, f32(i) * 24.f, std::max(160.f, c.w * 0.6f), std::max(120.f, c.h * 0.6f)});
}

// ---- graphics scene ----------------------------------------------------------------------------------------------------------
std::vector<u32>* GraphicsScene::cell(i32 cx, i32 cy, bool create) {
    const u64 key = (u64(u32(cx)) << 32) | u32(cy);
    for (auto& c : m_cells)
        if (c.first == key) return &c.second;
    if (!create) return nullptr;
    m_cells.emplace_back(key, std::vector<u32>{});
    return &m_cells.back().second;
}
void GraphicsScene::index(u32 id, bool insert) {
    const Rect b = itemBounds(m_items[id]);
    const i32 x0 = i32(std::floor(b.x / kCell)), x1 = i32(std::floor(b.right() / kCell));
    const i32 y0 = i32(std::floor(b.y / kCell)), y1 = i32(std::floor(b.bottom() / kCell));
    if (i64(x1 - x0 + 1) * i64(y1 - y0 + 1) > 4096) return; // a huge item is not indexed: itemAt / itemsIn scan for those
    for (i32 cy = y0; cy <= y1; ++cy)
        for (i32 cx = x0; cx <= x1; ++cx) {
            std::vector<u32>* c = cell(cx, cy, insert);
            if (!c) continue;
            if (insert) c->push_back(id);
            else std::erase(*c, id);
        }
}
namespace {
bool indexed(const GraphicsItem& it) {
    const Rect b = itemBounds(it);
    return (std::floor(b.right() / 128.f) - std::floor(b.x / 128.f) + 1.f) * (std::floor(b.bottom() / 128.f) - std::floor(b.y / 128.f) + 1.f) <= 4096.f;
}
} // namespace
u32 GraphicsScene::add(GraphicsItem it) {
    const u32 id = u32(m_items.size());
    m_items.push_back(std::move(it));
    m_alive.push_back(1);
    m_order.push_back(id);
    index(id, true);
    ++m_rev;
    ++m_orderRev;
    if (onChanged) onChanged(id);
    return id;
}
void GraphicsScene::remove(u32 id) {
    if (id >= m_items.size() || !m_alive[id]) return;
    index(id, false);
    m_alive[id] = 0;
    std::erase(m_order, id);
    ++m_rev;
    ++m_orderRev;
    if (onChanged) onChanged(id);
}
GraphicsItem* GraphicsScene::item(u32 id) { return id < m_items.size() && m_alive[id] ? &m_items[id] : nullptr; }
const GraphicsItem* GraphicsScene::item(u32 id) const { return id < m_items.size() && m_alive[id] ? &m_items[id] : nullptr; }
void GraphicsScene::move(u32 id, Vec2 d) {
    if (id >= m_items.size() || !m_alive[id]) return;
    index(id, false);
    GraphicsItem& it = m_items[id];
    it.rect.x += d.x;
    it.rect.y += d.y;
    for (Vec2& p : it.points) { p.x += d.x; p.y += d.y; }
    index(id, true);
    ++m_rev;
    if (onChanged) onChanged(id);
}
bool GraphicsScene::hit(const GraphicsItem& it, Vec2 p) {
    switch (it.shape) {
    case GraphicsShape::Rect:
    case GraphicsShape::Text: return it.rect.contains(p);
    case GraphicsShape::Ellipse: {
        const f32 rx = it.rect.w * 0.5f, ry = it.rect.h * 0.5f;
        if (rx <= 0.f || ry <= 0.f) return false;
        const f32 dx = (p.x - it.rect.x - rx) / rx, dy = (p.y - it.rect.y - ry) / ry;
        return dx * dx + dy * dy <= 1.f;
    }
    case GraphicsShape::Line: { // within the stroke (at least 3 px) of the segment
        const Vec2 a{it.rect.x, it.rect.y}, b{it.rect.right(), it.rect.bottom()};
        const f32 vx = b.x - a.x, vy = b.y - a.y, len2 = vx * vx + vy * vy;
        const f32 t = len2 > 0.f ? std::clamp(((p.x - a.x) * vx + (p.y - a.y) * vy) / len2, 0.f, 1.f) : 0.f;
        const f32 dx = p.x - (a.x + vx * t), dy = p.y - (a.y + vy * t);
        const f32 tol = std::max(3.f, it.strokeWidth * 0.5f + 2.f);
        return dx * dx + dy * dy <= tol * tol;
    }
    case GraphicsShape::Polygon: { // even-odd crossing test
        bool in = false;
        const std::size_t n = it.points.size();
        for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
            const Vec2 a = it.points[i], b = it.points[j];
            if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
        }
        return in;
    }
    }
    return false;
}
const std::vector<u32>& GraphicsScene::orderedItems() const {
    // re-sorted only when items came or went or a z changed since (items are edited in place through item())
    bool ok = m_sortedRev == m_orderRev && m_sorted.size() == m_order.size();
    for (std::size_t i = 0; ok && i < m_sorted.size(); ++i) ok = m_items[m_sorted[i]].z == m_sortedZ[i];
    if (!ok) {
        m_sorted = m_order;
        std::stable_sort(m_sorted.begin(), m_sorted.end(), [&](u32 a, u32 b) { return m_items[a].z < m_items[b].z; });
        m_sortedZ.resize(m_sorted.size());
        for (std::size_t i = 0; i < m_sorted.size(); ++i) m_sortedZ[i] = m_items[m_sorted[i]].z;
        m_sortedRev = m_orderRev;
    }
    return m_sorted;
}
std::vector<u32> GraphicsScene::paintOrder() const { return orderedItems(); }
u32 GraphicsScene::itemAt(Vec2 p) const {
    // candidates: the cell under the point plus the items too large for the index
    u32 best = ~0u;
    auto better = [&](u32 id) {
        if (best == ~0u) return true;
        if (m_items[id].z != m_items[best].z) return m_items[id].z > m_items[best].z;
        return std::find(m_order.begin(), m_order.end(), id) > std::find(m_order.begin(), m_order.end(), best);
    };
    auto consider = [&](u32 id) {
        const GraphicsItem& it = m_items[id];
        if (m_alive[id] && it.visible && hit(it, p) && id != best && better(id)) best = id;
    };
    const u64 key = (u64(u32(i32(std::floor(p.x / kCell)))) << 32) | u32(i32(std::floor(p.y / kCell)));
    for (const auto& c : m_cells)
        if (c.first == key)
            for (u32 id : c.second) consider(id);
    for (u32 id : m_order)
        if (!indexed(m_items[id])) consider(id);
    return best;
}
std::vector<u32> GraphicsScene::itemsIn(const Rect& r) const {
    std::vector<u32> out;
    for (u32 id : paintOrder()) {
        const GraphicsItem& it = m_items[id];
        if (it.visible && overlaps(itemBounds(it), r)) out.push_back(id);
    }
    return out;
}
void GraphicsScene::select(u32 id, bool on, bool exclusive) {
    if (exclusive)
        for (u32 k : m_order) m_items[k].selected = false;
    if (GraphicsItem* it = item(id)) it->selected = on;
    ++m_rev;
}
void GraphicsScene::clearSelection() {
    for (u32 id : m_order) m_items[id].selected = false;
    ++m_rev;
}
std::vector<u32> GraphicsScene::selection() const {
    std::vector<u32> out;
    for (u32 id : m_order)
        if (m_items[id].selected) out.push_back(id);
    return out;
}
u32 GraphicsScene::selectedCount() const {
    u32 c = 0;
    for (u32 id : m_order) c += m_items[id].selected ? 1u : 0u;
    return c;
}
u32 GraphicsScene::lastSelected() const {
    for (std::size_t i = m_order.size(); i-- > 0;)
        if (m_items[m_order[i]].selected) return m_order[i];
    return ~0u;
}
Rect GraphicsScene::bounds() const {
    Rect b{};
    bool first = true;
    for (u32 id : m_order) {
        Rect r = itemBounds(m_items[id]);
        r.w = std::max(r.w, 0.001f); // (a horizontal / vertical line still has bounds)
        r.h = std::max(r.h, 0.001f);
        b = first ? r : b.unite(r);
        first = false;
    }
    return b;
}
u32 GraphicsScene::size() const { return u32(m_order.size()); }

// ---- graphics view API ------------------------------------------------------------------------------------------------------
NodeHandle graphicsView(UiContext& ui, NodeHandle parent, GraphicsScene* scene, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle n = make(ui, "graphicsview", parent, id);
    if (!ui.alive(n)) return n;
    viewPool().create(ui, n).scene = scene;
    ui.setRange(n, -1.f, 4e9f, 1.f);
    ui.setValue(n, -1.f);
    return n;
}
void setGraphicsScene(UiContext& ui, NodeHandle view, GraphicsScene* scene) {
    if (ViewState* v = viewOf(ui, view)) { v->scene = scene; v->mode = 0; ui.invalidate(view); }
}
GraphicsScene* graphicsScene(const UiContext& ui, NodeHandle view) {
    const ViewState* v = viewOf(ui, view);
    return v ? v->scene : nullptr;
}
void setViewTransform(UiContext& ui, NodeHandle view, f32 zoom, Vec2 pan) {
    if (ViewState* v = viewOf(ui, view)) { v->zoom = std::clamp(zoom, 0.05f, 40.f); v->pan = pan; ui.invalidate(view); }
}
f32 viewZoom(const UiContext& ui, NodeHandle view) { const ViewState* v = viewOf(ui, view); return v ? v->zoom : 1.f; }
Vec2 viewPan(const UiContext& ui, NodeHandle view) { const ViewState* v = viewOf(ui, view); return v ? v->pan : Vec2{}; }
Vec2 viewToScene(const UiContext& ui, NodeHandle view, Vec2 p) {
    const ViewState* v = viewOf(ui, view);
    if (!v) return p;
    const Rect c = ui.contentRect(view);
    return {(p.x - c.x) / v->zoom + v->pan.x, (p.y - c.y) / v->zoom + v->pan.y};
}
Vec2 sceneToView(const UiContext& ui, NodeHandle view, Vec2 s) {
    const ViewState* v = viewOf(ui, view);
    if (!v) return s;
    const Rect c = ui.contentRect(view);
    return {c.x + (s.x - v->pan.x) * v->zoom, c.y + (s.y - v->pan.y) * v->zoom};
}
void fitInView(UiContext& ui, NodeHandle view) {
    ViewState* v = viewOf(ui, view);
    if (!v || !v->scene) return;
    const Rect b = v->scene->bounds(), c = ui.contentRect(view);
    if (b.w <= 0.f || b.h <= 0.f || c.w <= 0.f || c.h <= 0.f) return;
    v->zoom = std::clamp(std::min(c.w / b.w, c.h / b.h) * 0.95f, 0.05f, 40.f);
    v->pan = {b.x - (c.w / v->zoom - b.w) * 0.5f, b.y - (c.h / v->zoom - b.h) * 0.5f};
    ui.invalidate(view);
}
bool viewRubberBand(const UiContext& ui, NodeHandle view, Rect& band) {
    const ViewState* v = viewOf(ui, view);
    if (!v || v->mode != 3) return false;
    band = {std::min(v->bandA.x, v->bandB.x), std::min(v->bandA.y, v->bandB.y), std::abs(v->bandA.x - v->bandB.x), std::abs(v->bandA.y - v->bandB.y)};
    return true;
}

// ---- virtual keyboard API ------------------------------------------------------------------------------------------------------
NodeHandle virtualKeyboard(UiContext& ui, NodeHandle parent, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle kb = make(ui, "vkeyboard", parent, id);
    if (!ui.alive(kb)) return kb;
    ui.setA11y(kb, A11yRole::Group, "Keyboard");
    VkState& st = vkPool().create(ui, kb);
    st.kb = kb;
    vkNoteFocus(ui, st);
    auto makeRow = [&] {
        const NodeHandle r = panel(ui, kb);
        ui.addClass(r, "vkrow");
        return r;
    };
    auto makeKey = [&](NodeHandle row, std::string_view text, std::string_view name = {}) {
        const NodeHandle k = button(ui, row, text);
        ui.setFocusable(k, false); // typing must not move the keyboard focus onto the keys
        ui.addClass(k, "vkey");
        if (!name.empty()) {
            ui.setA11y(k, A11yRole::Button, name);
            vkPool().get(ui, kb)->named[std::string(name)] = k;
        }
        return k;
    };
    for (std::size_t row = 0; row < 3; ++row) {
        const NodeHandle r = makeRow();
        if (row == 2) {
            const NodeHandle sh = makeKey(r, "Shift", "Shift");
            ui.addClass(sh, "vkwide");
            ui.connect(sh, SignalKind::Clicked, [kb](UiContext& u, NodeHandle, const SignalArgs&) {
                VkState* sp = vkOf(u, kb);
                if (!sp) return;
                if (sp->caps) sp->caps = sp->shift = false;
                else if (sp->shift) sp->caps = true;
                else sp->shift = true;
                vkRelabel(u, *sp);
                u.emit(kb, SignalKind::Toggled, {.flag = sp->shift || sp->caps});
                (void)vkResolve(u, kb); // the press took the focus off the field
            });
        }
        for (std::size_t col = 0; col < std::max(kVkLetters[row].size(), kVkSymbols[row].size()); ++col) {
            const NodeHandle k = makeKey(r, "");
            vkPool().get(ui, kb)->keys.push_back(k);
            ui.connect(k, SignalKind::Clicked, [kb](UiContext& u, NodeHandle n, const SignalArgs&) {
                if (!vkResolve(u, kb)) return;
                const std::string t(u.text(n));
                u.textInput(t);
                VkState* sp = vkOf(u, kb);
                if (sp && sp->shift && !sp->caps) { // one-shot shift
                    sp->shift = false;
                    vkRelabel(u, *sp);
                    u.emit(kb, SignalKind::Toggled, {.flag = false});
                }
            });
        }
        if (row == 2) {
            const NodeHandle bs = makeKey(r, "Bksp", "Backspace");
            ui.addClass(bs, "vkwide");
            ui.connect(bs, SignalKind::Clicked, [kb](UiContext& u, NodeHandle, const SignalArgs&) {
                if (vkResolve(u, kb)) { u.keyDown(Key::Backspace); u.keyUp(Key::Backspace); }
            });
        }
    }
    const NodeHandle last = makeRow();
    const NodeHandle sym = makeKey(last, "?123", "?123");
    ui.addClass(sym, "vkwide");
    ui.connect(sym, SignalKind::Clicked, [kb](UiContext& u, NodeHandle, const SignalArgs&) {
        VkState* sp = vkOf(u, kb);
        if (!sp) return;
        sp->symbols = !sp->symbols;
        vkRelabel(u, *sp);
        u.setValue(kb, sp->symbols ? 1.f : 0.f, true);
        (void)vkResolve(u, kb);
    });
    const NodeHandle space = makeKey(last, " ", "Space");
    ui.addClass(space, "vkspace");
    ui.connect(space, SignalKind::Clicked, [kb](UiContext& u, NodeHandle, const SignalArgs&) {
        if (vkResolve(u, kb)) u.textInput(" ");
    });
    const NodeHandle enter = makeKey(last, "Enter", "Enter");
    ui.addClass(enter, "vkwide");
    ui.connect(enter, SignalKind::Clicked, [kb](UiContext& u, NodeHandle, const SignalArgs&) {
        if (vkResolve(u, kb)) { u.keyDown(Key::Enter); u.keyUp(Key::Enter); }
    });
    ui.setRange(kb, 0.f, 1.f, 1.f);
    vkRelabel(ui, *vkPool().get(ui, kb));
    return kb;
}
NodeHandle virtualKeyboardKey(const UiContext& ui, NodeHandle keyboard, std::string_view label) {
    const VkState* st = vkOf(ui, keyboard);
    if (!st) return {};
    if (const auto n = st->named.find(label); n != st->named.end()) return n->second;
    for (NodeHandle k : st->keys)
        if (ui.text(k) == label) return k;
    return {};
}
void setVirtualKeyboardTarget(UiContext& ui, NodeHandle keyboard, NodeHandle target) {
    if (VkState* st = vkOf(ui, keyboard)) st->target = ui.alive(target) && !inSubtree(ui, keyboard, target) ? target : NodeHandle{};
}
NodeHandle virtualKeyboardTarget(const UiContext& ui, NodeHandle keyboard) {
    const VkState* st = vkOf(ui, keyboard);
    return st && ui.alive(st->target) ? st->target : NodeHandle{};
}
void virtualKeyboardAutoShow(UiContext& ui, NodeHandle keyboard) {
    VkState* st = vkOf(ui, keyboard);
    if (!st) return;
    const NodeHandle f = ui.focused();
    if (ui.alive(f) && !inSubtree(ui, keyboard, f)) {
        if (vkTakesText(ui, f)) st->target = f;
        else st->target = {}; // the focus went to something that takes no text
    }
    // shown while a text widget has the focus, and while one of its own keys is held (the press parks the focus)
    const bool typing = ui.alive(st->target) && (ui.focused() == st->target || inSubtree(ui, keyboard, ui.pressed()));
    ui.setCss(keyboard, "display", typing ? "flex" : "none");
}

} // namespace fuse::ui::widgets
