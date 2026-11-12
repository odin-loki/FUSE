// FUSE UI Tier C (model / view: sort-filter proxy, grid view; delegates live with the item view in widgets_modelview.cpp)
// and Tier D (property grid, colour picker, calendar, chart, markdown view). Plan §5.
#include "fuse/ui/widgets/widgets.hpp"
#include "fuse/text/from_chars.hpp"
#include "widgets_internal.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace fuse::ui::widgets {
namespace {

NodeHandle make(UiContext& ui, std::string_view cls, NodeHandle parent, std::string_view id = {}) {
    auto h = ui.create(cls, parent, id);
    return h ? *h : NodeHandle{};
}
f32 lineH(const UiContext& ui, NodeHandle n) {
    const f32 px = ui.computed(n).fontSize;
    return std::ceil(ui.font() ? ui.font()->metrics(px).lineHeight() : px * 1.25f);
}
Color accentOf(const PaintContext& pc) { return pc.tint(pc.style.background2.a > 0.f ? pc.style.background2 : Color::rgb(0x3D8BFD)); }
NodeHandle nthChild(const UiContext& ui, NodeHandle n, u32 i) {
    NodeHandle c = ui.firstChild(n);
    while (i-- && ui.alive(c)) c = ui.nextSibling(c);
    return c;
}
bool parseNum(std::string_view s, double& out) {
    const auto [p, ec] = fuse::text::fromChars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size() && !s.empty();
}

/// Per-node state pool keyed by the node's user data (slot + 1); a slot is reused once its node is gone. One pool per
/// state type; the UI is single-threaded.
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

// ---- grid view ------------------------------------------------------------------------------------------------------
struct GridState {
    const ItemModel* model = nullptr;
    f32 tileW = 96.f, tileH = 72.f;
    u32 first = 0; ///< first visible tile row
    std::string scratch;
    // multi-selection (modelview.hpp): null = plain single selection, value() is the selected item
    SelectionModel* sel = nullptr;
    std::unique_ptr<SelectionModel> ownSel;
    SelectionMode mode = SelectionMode::Single;
};
Pool<GridState>& gridPool() { static Pool<GridState> p; return p; }

struct GridView {
    static constexpr std::string_view kName = "gridview";
    // touch: tile rows follow the finger (the Wheel handler scrolls 1 row per notch)
    static TouchDrag touchDrag(UiContext& ui, NodeHandle n, Vec2) {
        const GridState* s = gridPool().get(ui, n);
        if (!s || s->tileH <= 0.f || rowsTotal(ui, n, *s) <= visRows(ui, n, *s)) return {};
        return {TouchDrag::Mode::Wheel, {0.f, s->tileH}, {0.f, 1.f}};
    }
    static constexpr A11yRole kRole = A11yRole::List;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static constexpr f32 kBar = 8.f;
    static u32 count(const GridState& s) { return s.model && s.model->rows ? s.model->rows(s.model->user) : 0u; }
    static Size measure(UiContext&, NodeHandle, Size) { return {320.f, 220.f}; }
    static u32 cols(const UiContext& ui, NodeHandle n, const GridState& s) {
        return std::max(1u, u32((ui.contentRect(n).w - kBar) / s.tileW));
    }
    static u32 rowsTotal(const UiContext& ui, NodeHandle n, const GridState& s) {
        const u32 c = cols(ui, n, s);
        return (count(s) + c - 1) / c;
    }
    static u32 visRows(const UiContext& ui, NodeHandle n, const GridState& s) {
        return std::max(1u, u32(ui.contentRect(n).h / s.tileH));
    }
    static void setFirst(UiContext& ui, NodeHandle n, GridState& s, i64 f) {
        const u32 r = rowsTotal(ui, n, s), v = visRows(ui, n, s);
        const u32 nf = u32(std::clamp<i64>(f, 0, r > v ? i64(r - v) : 0));
        if (nf != s.first) { s.first = nf; ui.invalidate(n); }
    }
    static void select(UiContext& ui, NodeHandle n, GridState& s, i64 idx, u8 mods = 0, bool pointer = false) {
        const u32 total = count(s);
        if (!total) return;
        idx = std::clamp<i64>(idx, 0, i64(total) - 1);
        const u32 c = cols(ui, n, s), row = u32(idx) / c, v = visRows(ui, n, s);
        if (row < s.first) setFirst(ui, n, s, row);
        else if (row >= s.first + v) setFirst(ui, n, s, i64(row) - v + 1);
        if (!s.sel) ui.setValue(n, f32(idx), true);
        else if (pointer) s.sel->pointerSelect(u32(idx), 0, mods, s.mode);
        else s.sel->keySelect(u32(idx), 0, mods, s.mode);
    }
    static i64 current(const UiContext& ui, NodeHandle n, const GridState& s) { return s.sel ? s.sel->currentRow() : i64(ui.value(n)); }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        GridState* s = gridPool().get(ui, pc.node);
        if (!s) return;
        const Rect c = pc.content;
        const u32 nc = cols(ui, pc.node, *s), total = count(*s), vis = visRows(ui, pc.node, *s);
        const Color acc = accentOf(pc), text = pc.tint(pc.style.color);
        const i32 sel = i32(ui.value(pc.node));
        dl.pushClip(c);
        for (u32 r = 0; r <= vis; ++r)
            for (u32 k = 0; k < nc; ++k) {
                const u32 idx = (s->first + r) * nc + k;
                if (idx >= total) break;
                const Rect t{c.x + f32(k) * s->tileW, c.y + f32(r) * s->tileH, s->tileW, s->tileH};
                if (t.y >= c.bottom()) break;
                if (s->sel ? s->sel->isSelected(idx) : i32(idx) == sel) dl.rect({t.x + 2.f, t.y + 2.f, t.w - 4.f, t.h - 4.f}, acc.withAlpha(acc.a * 0.45f), 5.f);
                if (s->sel && i64(idx) == s->sel->currentRow() && (pc.states & kStateFocus)) // current item
                    dl.roundedRect({t.x + 2.f, t.y + 2.f, t.w - 4.f, t.h - 4.f}, {}, {5.f, 5.f, 5.f, 5.f}, acc, 1.f);
                // icon: a rounded tile tinted by index
                const f32 hue = f32((idx * 47u) % 360u) / 360.f;
                const Color ic{0.35f + 0.3f * std::abs(std::sin(hue * 6.283f)), 0.35f + 0.3f * std::abs(std::sin(hue * 6.283f + 2.f)),
                               0.45f + 0.3f * std::abs(std::sin(hue * 6.283f + 4.f)), 1.f};
                const f32 iw = std::min(t.w - 24.f, t.h - 30.f);
                dl.rect({t.x + (t.w - iw) * 0.5f, t.y + 6.f, iw, iw}, pc.tint(ic), 6.f);
                const std::string_view label = s->model->data(s->model->user, idx, 0, s->scratch);
                const Rect lr{t.x + 4.f, t.y + 8.f + iw, t.w - 8.f, t.h - iw - 10.f};
                dl.pushClip(lr);
                ui.paintString(dl, label, lr, text, pc.style.fontSize * 0.86f, TextAlignment::Center, pc.node.index);
                dl.popClip();
            }
        const u32 rt = rowsTotal(ui, pc.node, *s);
        if (rt > vis) {
            const Rect tr{c.right() - kBar, c.y, kBar, c.h};
            const f32 tl = std::max(16.f, tr.h * f32(vis) / f32(rt));
            dl.rect(tr, pc.tint(Color::rgb(0x000000, 0.15f)), kBar * 0.5f);
            dl.rect({tr.x, tr.y + (tr.h - tl) * f32(s->first) / f32(rt - vis), kBar, tl}, pc.tint(Color::rgb(0x8A8A96, 0.85f)), kBar * 0.5f);
        }
        dl.popClip();
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        GridState* s = gridPool().get(ui, ec.node);
        if (!s) return false;
        const Rect c = ui.contentRect(ec.node);
        const i64 nc = cols(ui, ec.node, *s);
        switch (e.type) {
        case EventType::PointerDown: {
            if (e.pos.x >= c.right() - kBar) return true;
            const i64 k = i64((e.pos.x - c.x) / s->tileW), r = i64((e.pos.y - c.y) / s->tileH) + s->first;
            if (k < nc && k >= 0 && r >= 0) {
                const i64 idx = r * nc + k;
                if (idx >= i64(count(*s))) return true;
                // a right / middle press makes the tile current and keeps a selection the tile is part of
                if (e.button != MouseButton::Left && s->sel && s->sel->rowSelected(u32(idx))) s->sel->setCurrent(idx);
                else select(ui, ec.node, *s, idx, e.button == MouseButton::Left ? e.mods : u8(0), true);
            }
            return true;
        }
        case EventType::Wheel: setFirst(ui, ec.node, *s, i64(s->first) - i64(std::lround(e.wheel.y))); return true;
        case EventType::KeyDown: {
            const i64 cur = current(ui, ec.node, *s);
            const u8 m = e.mods;
            switch (e.key) {
            case Key::Right: select(ui, ec.node, *s, cur + 1, m); return true;
            case Key::Left: select(ui, ec.node, *s, cur < 0 ? 0 : cur - 1, m); return true;
            case Key::Down: select(ui, ec.node, *s, cur < 0 ? 0 : cur + nc, m); return true;
            case Key::Up: select(ui, ec.node, *s, cur - nc, m); return true;
            case Key::PageDown: select(ui, ec.node, *s, cur + nc * visRows(ui, ec.node, *s), m); return true;
            case Key::PageUp: select(ui, ec.node, *s, cur - nc * visRows(ui, ec.node, *s), m); return true;
            case Key::Home: select(ui, ec.node, *s, 0, m); return true;
            case Key::End: select(ui, ec.node, *s, i64(count(*s)) - 1, m); return true;
            case Key::Enter: if (cur >= 0) ui.emit(ec.node, SignalKind::Submitted, {.value = f32(cur)}); return true;
            case Key::Space: // multi / extended selection: toggles the current item
                if (!s->sel || cur < 0 || (s->mode != SelectionMode::Multi && s->mode != SelectionMode::Extended)) return false;
                s->sel->toggle(u32(cur));
                return true;
            case Key::A:
                if (!(m & kModCtrl) || !s->sel || (s->mode != SelectionMode::Multi && s->mode != SelectionMode::Extended)) return false;
                s->sel->selectAll(count(*s));
                return true;
            default: return false;
            }
        }
        case EventType::DoubleClick: { // double click on a tile activates it, like Enter
            const i64 cur = current(ui, ec.node, *s);
            const i64 k = i64((e.pos.x - c.x) / s->tileW), r = i64((e.pos.y - c.y) / s->tileH) + s->first;
            if (cur >= 0 && k >= 0 && k < nc && r * nc + k == cur) ui.emit(ec.node, SignalKind::Submitted, {.value = f32(cur)});
            return true;
        }
        default: return false;
        }
    }
};

// ---- colour picker ----------------------------------------------------------------------------------------------------
// HSVA in the node's user data, 16 bits each (h in [0, 1)).
struct Hsva { f32 h, s, v, a; };
Hsva unpack(u64 u) {
    auto f = [&](int sh) { return f32((u >> sh) & 0xFFFFu) / 65535.f; };
    return {f(48), f(32), f(16), f(0)};
}
u64 pack(Hsva c) {
    auto q = [](f32 x) { return u64(std::lround(std::clamp(x, 0.f, 1.f) * 65535.f)); };
    return (q(c.h) << 48) | (q(c.s) << 32) | (q(c.v) << 16) | q(c.a);
}
Color hsvToRgb(f32 h, f32 s, f32 v, f32 a = 1.f) {
    h = (h - std::floor(h)) * 6.f;
    const int i = int(h) % 6;
    const f32 f = h - std::floor(h), p = v * (1.f - s), q = v * (1.f - s * f), t = v * (1.f - s * (1.f - f));
    switch (i) {
    case 0: return {v, t, p, a};
    case 1: return {q, v, p, a};
    case 2: return {p, v, t, a};
    case 3: return {p, q, v, a};
    case 4: return {t, p, v, a};
    default: return {v, p, q, a};
    }
}
Hsva rgbToHsv(Color c) {
    const f32 mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b}), d = mx - mn;
    f32 h = 0.f;
    if (d > 0.f) {
        if (mx == c.r) h = std::fmod((c.g - c.b) / d, 6.f);
        else if (mx == c.g) h = (c.b - c.r) / d + 2.f;
        else h = (c.r - c.g) / d + 4.f;
        h /= 6.f;
        if (h < 0.f) h += 1.f;
    }
    return {h, mx > 0.f ? d / mx : 0.f, mx, c.a};
}
u8 pickZone = 0; // 1 square, 2 hue, 3 alpha (single pointer, single thread)

struct ColorPickerW {
    static constexpr std::string_view kName = "colorpicker";
    static constexpr bool kCapturesDrag = true; // touch: the drag picks a colour
    static constexpr A11yRole kRole = A11yRole::Group;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {232.f, 196.f}; }
    struct Geo { Rect sq, hue, alpha, swatch; };
    static Geo geo(Rect c) {
        Geo g;
        g.sq = {c.x, c.y, c.w - 26.f, c.h - 22.f};
        g.hue = {c.right() - 16.f, c.y, 16.f, g.sq.h};
        g.alpha = {c.x, c.bottom() - 14.f, g.sq.w, 14.f};
        g.swatch = {c.right() - 16.f, c.bottom() - 14.f, 16.f, 14.f};
        return g;
    }
    static void publish(UiContext& ui, NodeHandle n, Hsva h) {
        ui.setUserData(n, pack(h));
        const Color c = hsvToRgb(h.h, h.s, h.v, h.a);
        char b[16];
        auto q = [](f32 x) { return unsigned(std::lround(std::clamp(x, 0.f, 1.f) * 255.f)); };
        std::snprintf(b, sizeof b, "#%02X%02X%02X%02X", q(c.r), q(c.g), q(c.b), q(c.a));
        ui.setText(n, b);
        ui.setValue(n, h.h * 360.f, true);
        ui.emit(n, SignalKind::TextChanged, {.text = ui.text(n)});
        ui.invalidate(n);
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Hsva h = unpack(pc.ui.userData(pc.node));
        const Geo g = geo(pc.content);
        const Color hueCol = hsvToRgb(h.h, 1.f, 1.f);
        dl.gradientRect(g.sq, pc.tint(Color::rgb(0xFFFFFF)), pc.tint(hueCol), false, {3.f, 3.f, 3.f, 3.f});
        dl.gradientRect(g.sq, pc.tint(Color::rgb(0x000000, 0.f)), pc.tint(Color::rgb(0x000000)), true, {3.f, 3.f, 3.f, 3.f});
        for (int i = 0; i < 6; ++i) {
            const Rect seg{g.hue.x, g.hue.y + g.hue.h * f32(i) / 6.f, g.hue.w, g.hue.h / 6.f + 0.5f};
            dl.gradientRect(seg, pc.tint(hsvToRgb(f32(i) / 6.f, 1.f, 1.f)), pc.tint(hsvToRgb(f32(i + 1) / 6.f, 1.f, 1.f)), true);
        }
        const Color rgb = hsvToRgb(h.h, h.s, h.v);
        for (int i = 0; i < int(g.alpha.w / 7.f); ++i) // checker under the alpha ramp
            dl.rect({g.alpha.x + f32(i) * 7.f, g.alpha.y + ((i & 1) ? 0.f : 7.f), 7.f, 7.f}, pc.tint(Color::rgb(0x55555F)));
        dl.gradientRect(g.alpha, pc.tint(rgb.withAlpha(0.f)), pc.tint(rgb), false, {3.f, 3.f, 3.f, 3.f});
        dl.rect(g.swatch, pc.tint(hsvToRgb(h.h, h.s, h.v, h.a)), 3.f);
        auto ring = [&](Vec2 p) {
            dl.roundedRect({p.x - 6.f, p.y - 6.f, 12.f, 12.f}, {}, {6.f, 6.f, 6.f, 6.f}, pc.tint(Color::rgb(0x000000, 0.8f)), 3.f);
            dl.roundedRect({p.x - 5.f, p.y - 5.f, 10.f, 10.f}, {}, {5.f, 5.f, 5.f, 5.f}, pc.tint(Color::rgb(0xFFFFFF)), 1.5f);
        };
        ring({g.sq.x + h.s * g.sq.w, g.sq.y + (1.f - h.v) * g.sq.h});
        dl.roundedRect({g.hue.x - 2.f, g.hue.y + h.h * g.hue.h - 3.f, g.hue.w + 4.f, 6.f}, {}, {2.f, 2.f, 2.f, 2.f}, pc.tint(Color::rgb(0xFFFFFF)), 1.5f);
        dl.roundedRect({g.alpha.x + h.a * g.alpha.w - 3.f, g.alpha.y - 2.f, 6.f, g.alpha.h + 4.f}, {}, {2.f, 2.f, 2.f, 2.f}, pc.tint(Color::rgb(0xFFFFFF)), 1.5f);
        if (pc.states & kStateFocus) dl.roundedRect(pc.rect, {}, {5.f, 5.f, 5.f, 5.f}, accentOf(pc), 1.f);
    }
    static void apply(UiContext& ui, NodeHandle n, Vec2 p) {
        const Geo g = geo(ui.contentRect(n));
        Hsva h = unpack(ui.userData(n));
        auto t = [](f32 a, f32 lo, f32 len) { return std::clamp((a - lo) / std::max(1.f, len), 0.f, 1.f); };
        if (pickZone == 1) { h.s = t(p.x, g.sq.x, g.sq.w); h.v = 1.f - t(p.y, g.sq.y, g.sq.h); }
        else if (pickZone == 2) h.h = std::min(t(p.y, g.hue.y, g.hue.h), 0.9999f);
        else if (pickZone == 3) h.a = t(p.x, g.alpha.x, g.alpha.w);
        else return;
        publish(ui, n, h);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        if (e.type == EventType::PointerDown) {
            const Geo g = geo(ui.contentRect(ec.node));
            pickZone = g.sq.contains(e.pos) ? 1 : g.hue.contains(e.pos) ? 2 : g.alpha.contains(e.pos) ? 3 : 0;
            apply(ui, ec.node, e.pos);
            return true;
        }
        if (e.type == EventType::PointerMove && ui.pressed() == ec.node) { apply(ui, ec.node, e.pos); return true; }
        if (e.type == EventType::PointerUp) { pickZone = 0; return false; }
        if (e.type == EventType::KeyDown) {
            Hsva h = unpack(ui.userData(ec.node));
            switch (e.key) {
            case Key::Left: h.s = std::max(0.f, h.s - 0.05f); break;
            case Key::Right: h.s = std::min(1.f, h.s + 0.05f); break;
            case Key::Up: h.v = std::min(1.f, h.v + 0.05f); break;
            case Key::Down: h.v = std::max(0.f, h.v - 0.05f); break;
            case Key::PageUp: h.h = std::fmod(h.h + 1.f - 1.f / 36.f, 1.f); break;
            case Key::PageDown: h.h = std::fmod(h.h + 1.f / 36.f, 1.f); break;
            default: return false;
            }
            publish(ui, ec.node, h);
            return true;
        }
        return false;
    }
};

// ---- calendar ---------------------------------------------------------------------------------------------------------
constexpr const char* kMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September",
                                   "October", "November", "December"};
constexpr const char* kDays[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
i32 weekday(i32 z) { return ((z + 3) % 7 + 7) % 7; } // Monday = 0 (1970-01-01 was a Thursday)
int daysIn(int y, int m) {
    static constexpr int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : d[m - 1];
}

struct CalendarW {
    static constexpr std::string_view kName = "calendar";
    static constexpr A11yRole kRole = A11yRole::Group;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) { return {7.f * 34.f, lineH(ui, n) + 8.f + 7.f * 26.f}; }
    static i32 shownMonth(const UiContext& ui, NodeHandle n) { return i32(ui.userData(n)) - 1000000; } // y * 12 + (m - 1)
    static void setShown(UiContext& ui, NodeHandle n, i32 ym) { ui.setUserData(n, u64(ym + 1000000)); ui.invalidate(n); }
    struct Geo { Rect head, prev, next; f32 cellW, cellH, gridY; };
    static Geo geo(const UiContext& ui, NodeHandle n) {
        const Rect c = ui.contentRect(n);
        Geo g;
        const f32 hh = lineH(ui, n) + 8.f;
        g.head = {c.x, c.y, c.w, hh};
        g.prev = {c.x, c.y, hh, hh};
        g.next = {c.right() - hh, c.y, hh, hh};
        g.cellW = c.w / 7.f;
        g.cellH = (c.h - hh) / 7.f;
        g.gridY = c.y + hh + g.cellH; // first week row (below the weekday names)
        return g;
    }
    /// The locale data (UiContext::setLocale hooks), or null for English / Monday first.
    static const LocaleHooks* hooks(const UiContext& ui) {
        const LocaleHooks* h = ui.localeHooks();
        return h && !ui.locale().empty() ? h : nullptr;
    }
    /// First column's ISO weekday (1 = Monday).
    static int firstDay(const UiContext& ui) {
        const LocaleHooks* h = hooks(ui);
        const int d = h && h->firstDayOfWeek ? h->firstDayOfWeek(ui.locale()) : 1;
        return d >= 1 && d <= 7 ? d : 1;
    }
    static i32 firstCellDay(const UiContext& ui, i32 ym) { // day number of the grid's top-left cell
        const i32 first = daysFromCivil(ym / 12, ym % 12 + 1, 1);
        return first - (weekday(first) - (firstDay(ui) - 1) + 7) % 7;
    }
    static void select(UiContext& ui, NodeHandle n, i32 z) {
        int y, m, d;
        civilFromDays(z, y, m, d);
        setShown(ui, n, y * 12 + m - 1);
        ui.setValue(n, f32(z), true);
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        const Geo g = geo(ui, pc.node);
        const i32 ym = shownMonth(ui, pc.node), sel = i32(ui.value(pc.node));
        const Color text = pc.tint(pc.style.color), dim = pc.tint(pc.style.color.withAlpha(pc.style.color.a * 0.38f)), acc = accentOf(pc);
        const f32 fs = pc.style.fontSize;
        char b[48];
        const LocaleHooks* lh = hooks(ui);
        std::string head;
        if (lh && lh->monthYear) head = lh->monthYear(ui.locale(), ym / 12, ym % 12 + 1);
        else std::snprintf(b, sizeof b, "%s %d", kMonths[ym % 12], ym / 12);
        ui.paintString(dl, lh && lh->monthYear ? std::string_view(head) : std::string_view(b), g.head, text, fs,
                       TextAlignment::Center, pc.node.index);
        for (int side = 0; side < 2; ++side) { // chevrons
            const Rect r = side ? g.next : g.prev;
            const f32 cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, d = side ? 3.f : -3.f;
            dl.line({cx - d, cy - 5.f}, {cx + d, cy}, text, 1.5f);
            dl.line({cx + d, cy}, {cx - d, cy + 5.f}, text, 1.5f);
        }
        for (int k = 0; k < 7; ++k) {
            const int iso = (firstDay(ui) - 1 + k) % 7 + 1;
            const std::string_view name =
                lh && lh->weekdayShort ? lh->weekdayShort(ui.locale(), iso) : std::string_view(kDays[iso - 1]);
            ui.paintString(dl, name, {g.head.x + f32(k) * g.cellW, g.head.bottom(), g.cellW, g.cellH}, iso >= 6 ? dim : text,
                           fs * 0.85f, TextAlignment::Center, pc.node.index);
        }
        const i32 z0 = firstCellDay(ui, ym);
        for (int i = 0; i < 42; ++i) {
            const i32 z = z0 + i;
            int y, m, d;
            civilFromDays(z, y, m, d);
            const Rect cell{g.head.x + f32(i % 7) * g.cellW, g.gridY + f32(i / 7) * g.cellH, g.cellW, g.cellH};
            const bool inMonth = y * 12 + m - 1 == ym;
            if (z == sel) dl.rect({cell.x + 2.f, cell.y + 1.f, cell.w - 4.f, cell.h - 2.f}, acc, 4.f);
            std::snprintf(b, sizeof b, "%d", d);
            ui.paintString(dl, b, cell, z == sel ? pc.tint(Color::rgb(0xFFFFFF)) : inMonth ? text : dim, fs * 0.9f, TextAlignment::Center,
                           pc.node.index);
        }
        if (pc.states & kStateFocus) dl.roundedRect(pc.rect, {}, {5.f, 5.f, 5.f, 5.f}, acc.withAlpha(acc.a * 0.6f), 1.f);
    }
    static void pageMonth(UiContext& ui, NodeHandle n, i32 delta, bool moveSelection) {
        const i32 ym = shownMonth(ui, n) + delta;
        if (!moveSelection) { setShown(ui, n, ym); return; }
        int y, m, d;
        civilFromDays(i32(ui.value(n)), y, m, d);
        const int ny = ym / 12, nm = ym % 12 + 1;
        select(ui, n, daysFromCivil(ny, nm, std::min(d, daysIn(ny, nm))));
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        if (e.type == EventType::PointerDown) {
            const Geo g = geo(ui, ec.node);
            if (g.prev.contains(e.pos)) { pageMonth(ui, ec.node, -1, false); return true; }
            if (g.next.contains(e.pos)) { pageMonth(ui, ec.node, 1, false); return true; }
            if (e.pos.y >= g.gridY) {
                const i32 row = i32((e.pos.y - g.gridY) / g.cellH), col = i32((e.pos.x - g.head.x) / g.cellW);
                if (row >= 0 && row < 6 && col >= 0 && col < 7) select(ui, ec.node, firstCellDay(ui, shownMonth(ui, ec.node)) + row * 7 + col);
            }
            return true;
        }
        if (e.type == EventType::KeyDown) {
            const i32 cur = i32(ui.value(ec.node));
            switch (e.key) {
            case Key::Left: select(ui, ec.node, cur - 1); return true;
            case Key::Right: select(ui, ec.node, cur + 1); return true;
            case Key::Up: select(ui, ec.node, cur - 7); return true;
            case Key::Down: select(ui, ec.node, cur + 7); return true;
            case Key::PageUp: pageMonth(ui, ec.node, -1, true); return true;
            case Key::PageDown: pageMonth(ui, ec.node, 1, true); return true;
            case Key::Enter: ui.emit(ec.node, SignalKind::Submitted, {.value = f32(cur)}); return true;
            default: return false;
            }
        }
        return false;
    }
};

// ---- chart ----------------------------------------------------------------------------------------------------------------
struct Series { std::string name; std::vector<f32> v; Color color; };
struct Flame { std::string name; f32 start = 0.f, duration = 0.f; u32 depth = 0; Color color; };
struct ChartState { ChartKind kind = ChartKind::Line; std::vector<Series> series; std::vector<Flame> flame; };
Pool<ChartState>& chartPool() { static Pool<ChartState> p; return p; }

struct ChartW {
    static constexpr std::string_view kName = "chart";
    static constexpr A11yRole kRole = A11yRole::Image;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {360.f, 200.f}; }
    static u32 points(const ChartState& s) {
        u32 n = 0;
        for (const Series& se : s.series) n = std::max(n, u32(se.v.size()));
        return n;
    }
    static Rect plot(const UiContext& ui, NodeHandle n) {
        const Rect c = ui.contentRect(n);
        const f32 lh = lineH(ui, n);
        return {c.x + 40.f, c.y + lh + 6.f, c.w - 46.f, c.h - lh - 12.f};
    }
    static void range(const ChartState& s, f32& lo, f32& hi) {
        lo = 1e30f; hi = -1e30f;
        for (const Series& se : s.series) for (f32 v : se.v) { lo = std::min(lo, v); hi = std::max(hi, v); }
        if (lo > hi) { lo = 0.f; hi = 1.f; }
        if (s.kind == ChartKind::Bar || s.kind == ChartKind::Area) { lo = std::min(lo, 0.f); hi = std::max(hi, 0.f); }
        if (hi - lo < 1e-6f) hi = lo + 1.f;
        // nice step: 1, 2 or 5 x 10^k for ~4 intervals
        const f32 raw = (hi - lo) / 4.f, mag = std::pow(10.f, std::floor(std::log10(raw)));
        const f32 st = raw / mag <= 1.f ? mag : raw / mag <= 2.f ? 2.f * mag : raw / mag <= 5.f ? 5.f * mag : 10.f * mag;
        lo = std::floor(lo / st) * st;
        hi = std::ceil(hi / st) * st;
    }
    static f32 xAt(const Rect& p, u32 i, u32 n, bool bars) {
        return bars ? p.x + (f32(i) + 0.5f) * p.w / f32(std::max(1u, n)) : p.x + (n > 1 ? f32(i) * p.w / f32(n - 1) : p.w * 0.5f);
    }
    // ---- flame graph: spans stacked by depth (0 at the top) over the total time range ----
    static Rect flamePlot(const UiContext& ui, NodeHandle n) {
        const Rect c = ui.contentRect(n);
        const f32 lh = lineH(ui, n);
        return {c.x, c.y + lh + 4.f, c.w, std::max(0.f, c.h - lh - 4.f)};
    }
    static void flameRange(const ChartState& s, f32& t0, f32& t1, u32& rows) {
        t0 = 1e30f; t1 = -1e30f; rows = 0;
        for (const Flame& f : s.flame) {
            t0 = std::min(t0, f.start);
            t1 = std::max(t1, f.start + std::max(0.f, f.duration));
            rows = std::max(rows, f.depth + 1);
        }
        if (!(t1 > t0)) { t0 = s.flame.empty() ? 0.f : t0; t1 = t0 + 1.f; }
    }
    static f32 flameRowH(const UiContext& ui, NodeHandle n, const Rect& p, u32 rows) {
        return std::min(lineH(ui, n) + 6.f, p.h / f32(std::max(1u, rows)));
    }
    static Rect flameRect(const UiContext& ui, NodeHandle n, const ChartState& s, u32 index) {
        if (index >= s.flame.size()) return {};
        const Rect p = flamePlot(ui, n);
        f32 t0, t1;
        u32 rows;
        flameRange(s, t0, t1, rows);
        const Flame& f = s.flame[index];
        const f32 rh = flameRowH(ui, n, p, rows), k = p.w / (t1 - t0);
        return {p.x + (f.start - t0) * k, p.y + f32(f.depth) * rh, std::max(1.f, std::max(0.f, f.duration) * k), rh};
    }
    static i32 flameAt(const UiContext& ui, NodeHandle n, const ChartState& s, Vec2 pos) {
        for (u32 i = u32(s.flame.size()); i-- > 0;) // later spans paint over earlier ones
            if (flameRect(ui, n, s, i).contains(pos)) return i32(i);
        return -1;
    }
    static Color flameColor(const Flame& f) {
        if (f.color.a > 0.f) return f.color;
        static constexpr u32 kWarm[] = {0xE4572E, 0xF0803C, 0xF3A712, 0xD95D39, 0xE9724C, 0xC8553D, 0xF2B134, 0xDB7F3B};
        return Color::rgb(kWarm[hashId(f.name) % 8u]);
    }
    static void paintFlame(const PaintContext& pc, DrawList& dl, const ChartState& s) {
        UiContext& ui = pc.ui;
        const Rect c = pc.content, p = flamePlot(ui, pc.node);
        const f32 fs = pc.style.fontSize * 0.8f;
        f32 t0, t1;
        u32 rows;
        flameRange(s, t0, t1, rows);
        const i32 hov = i32(ui.value(pc.node));
        dl.pushClip(p);
        for (u32 i = 0; i < s.flame.size(); ++i) {
            const Rect r = flameRect(ui, pc.node, s, i);
            const Rect bar{r.x, r.y, std::max(1.f, r.w - 1.f), std::max(1.f, r.h - 1.f)};
            const Color col = flameColor(s.flame[i]);
            dl.roundedRect(bar, pc.tint(col), {2.f, 2.f, 2.f, 2.f}, i32(i) == hov ? pc.tint(Color::rgb(0xFFFFFF)) : Color{}, i32(i) == hov ? 1.f : 0.f);
            if (bar.w < 14.f) continue; // too narrow for a label
            // dark text on the (warm, light) bars; the label never leaves its bar
            const f32 lum = 0.299f * col.r + 0.587f * col.g + 0.114f * col.b;
            dl.pushClip(bar);
            ui.paintString(dl, s.flame[i].name, {bar.x + 4.f, bar.y, bar.w - 6.f, bar.h}, pc.tint(Color::rgb(lum > 0.5f ? 0x14141A : 0xFFFFFF)), fs,
                           TextAlignment::Start, pc.node.index);
            dl.popClip();
        }
        dl.popClip();
        char b[96];
        if (hov >= 0 && u32(hov) < s.flame.size()) { // readout: name, duration, share of the whole range
            const Flame& f = s.flame[u32(hov)];
            std::snprintf(b, sizeof b, "  %.4g (%.1f%%)", double(f.duration), double(f.duration / (t1 - t0) * 100.f));
            ui.paintString(dl, f.name + b, {c.x, c.y, c.w, 20.f}, pc.tint(pc.style.color), fs, TextAlignment::Start, pc.node.index);
        } else {
            std::snprintf(b, sizeof b, "%.4g .. %.4g", double(t0), double(t1));
            ui.paintString(dl, b, {c.x, c.y, c.w, 20.f}, pc.tint(pc.style.color.withAlpha(pc.style.color.a * 0.55f)), fs, TextAlignment::Start,
                           pc.node.index);
        }
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        ChartState* s = chartPool().get(ui, pc.node);
        if (!s) return;
        if (s->kind == ChartKind::Flame) { paintFlame(pc, dl, *s); return; }
        const Rect p = plot(ui, pc.node), c = pc.content;
        const Color text = pc.tint(pc.style.color), dim = pc.tint(pc.style.color.withAlpha(pc.style.color.a * 0.55f));
        const f32 fs = pc.style.fontSize * 0.8f;
        f32 lo, hi;
        range(*s, lo, hi);
        auto yOf = [&](f32 v) { return p.bottom() - (v - lo) / (hi - lo) * p.h; };
        char b[32];
        for (int i = 0; i <= 4; ++i) {
            const f32 v = lo + (hi - lo) * f32(i) / 4.f, y = std::round(yOf(v));
            dl.rect({p.x, y, p.w, 1.f}, pc.tint(Color::rgb(0xFFFFFF, i == 0 ? 0.25f : 0.07f)));
            std::snprintf(b, sizeof b, "%.4g", double(v));
            ui.paintString(dl, b, {c.x, y - 8.f, 36.f, 16.f}, dim, fs, TextAlignment::End, pc.node.index);
        }
        const u32 n = points(*s);
        const bool bars = s->kind == ChartKind::Bar;
        const u32 ns = u32(s->series.size());
        dl.pushClip({p.x, p.y - 4.f, p.w, p.h + 5.f});
        for (u32 k = 0; k < ns; ++k) {
            const Series& se = s->series[k];
            const Color col = pc.tint(se.color);
            for (u32 i = 0; i < se.v.size(); ++i) {
                const f32 x = xAt(p, i, n, bars), y = yOf(se.v[i]);
                switch (s->kind) {
                case ChartKind::Bar: {
                    const f32 slot = p.w / f32(std::max(1u, n)), bw = slot * 0.8f / f32(std::max(1u, ns));
                    const f32 bx = x - slot * 0.4f + f32(k) * bw, y0 = yOf(0.f);
                    dl.rect({bx + 1.f, std::min(y, y0), bw - 2.f, std::max(1.f, std::abs(y0 - y))}, col, 2.f);
                    break;
                }
                case ChartKind::Scatter:
                    dl.rect({x - 3.f, y - 3.f, 6.f, 6.f}, col, 3.f);
                    break;
                case ChartKind::Area:
                    if (i + 1 < se.v.size()) { // filled with 2 px columns under the segment
                        const f32 x1 = xAt(p, i + 1, n, false), y1 = yOf(se.v[i + 1]), y0 = yOf(0.f);
                        for (f32 cx = x; cx < x1; cx += 2.f) {
                            const f32 t = (cx - x) / std::max(1.f, x1 - x), cy = y + (y1 - y) * t;
                            dl.rect({cx, std::min(cy, y0), std::min(2.f, x1 - cx), std::abs(y0 - cy)}, col.withAlpha(col.a * 0.35f));
                        }
                    }
                    [[fallthrough]];
                case ChartKind::Line:
                    if (i + 1 < se.v.size()) dl.line({x, y}, {xAt(p, i + 1, n, false), yOf(se.v[i + 1])}, col, 2.f);
                    break;
                case ChartKind::Flame: break; // painted by paintFlame
                }
            }
        }
        dl.popClip();
        // legend (top right) and hover readout (top left)
        f32 lx = c.right();
        for (u32 k = ns; k-- > 0;) {
            const Series& se = s->series[k];
            const f32 w = ui.measureString(se.name, fs) + 22.f;
            lx -= w;
            dl.rect({lx, c.y + 5.f, 10.f, 10.f}, pc.tint(se.color), 2.f);
            ui.paintString(dl, se.name, {lx + 14.f, c.y, w - 14.f, 20.f}, text, fs, TextAlignment::Start, pc.node.index);
        }
        const i32 hov = i32(ui.value(pc.node));
        if (hov >= 0 && u32(hov) < n) {
            const f32 x = std::round(xAt(p, u32(hov), n, bars));
            dl.rect({x, p.y, 1.f, p.h}, pc.tint(Color::rgb(0xFFFFFF, 0.3f)));
            std::string r = "#" + std::to_string(hov);
            for (const Series& se : s->series)
                if (u32(hov) < se.v.size()) { std::snprintf(b, sizeof b, "  %s %.4g", se.name.c_str(), double(se.v[hov])); r += b; }
            ui.paintString(dl, r, {c.x, c.y, c.w, 20.f}, text, fs, TextAlignment::Start, pc.node.index);
        }
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        ChartState* s = chartPool().get(ui, ec.node);
        if (!s) return false;
        if (s->kind == ChartKind::Flame) {
            if (e.type == EventType::PointerMove || e.type == EventType::PointerLeave) {
                const i32 idx = e.type == EventType::PointerMove ? flameAt(ui, ec.node, *s, e.pos) : -1;
                if (f32(idx) != ui.value(ec.node)) { ui.setValue(ec.node, f32(idx), true); ui.invalidate(ec.node); }
            } else if (e.type == EventType::Click) {
                const i32 idx = flameAt(ui, ec.node, *s, e.pos);
                if (idx < 0) return false;
                ui.emit(ec.node, SignalKind::Submitted, {.value = f32(idx), .text = s->flame[u32(idx)].name});
                return true;
            }
            return false;
        }
        if (e.type == EventType::PointerMove) {
            const Rect p = plot(ui, ec.node);
            const u32 n = points(*s);
            i32 idx = -1;
            if (n && p.contains(e.pos)) {
                const bool bars = s->kind == ChartKind::Bar;
                const f32 t = bars ? (e.pos.x - p.x) / p.w * f32(n) - 0.5f : (e.pos.x - p.x) / p.w * f32(n - 1);
                idx = std::clamp(i32(std::lround(t)), 0, i32(n) - 1);
            }
            if (f32(idx) != ui.value(ec.node)) { ui.setValue(ec.node, f32(idx), true); ui.invalidate(ec.node); }
            return false;
        }
        return false;
    }
};

// ---- property grid ------------------------------------------------------------------------------------------------------
void stripInline(std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '`') continue;
        if ((s[i] == '*' || s[i] == '_') && i + 1 < s.size() && s[i + 1] == s[i]) { ++i; continue; }
        o += s[i];
    }
    s = std::move(o);
}

/// Inline Markdown emphasis to rich-text markup: **bold** / __bold__, *italic* / _italic_ (an underscore only at a word
/// boundary, so snake_case stays), `code` (its text kept as is). False when the text has no emphasis: it stays a label.
bool inlineToRich(std::string_view s, std::string& out) {
    auto alnum = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    auto esc = [&](char c) {
        if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '&') out += "&amp;";
        else out += c;
    };
    out.clear();
    bool any = false, bold = false, italic = false;
    char italicMark = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '`') {
            const std::size_t e = s.find('`', i + 1);
            if (e != std::string_view::npos) {
                for (std::size_t k = i + 1; k < e; ++k) esc(s[k]);
                i = e;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < s.size() && s[i + 1] == c) {
            const char pair[3] = {c, c, 0};
            if (bold || s.find(pair, i + 2) != std::string_view::npos) {
                out += bold ? "</b>" : "<b>";
                bold = !bold;
                any = true;
            }
            ++i;
            continue;
        }
        if (c == '*' || c == '_') {
            const bool leftEdge = i == 0 || !alnum(s[i - 1]), rightEdge = i + 1 >= s.size() || !alnum(s[i + 1]);
            if (italic && c == italicMark && i > 0 && s[i - 1] != ' ' && (c == '*' || rightEdge)) {
                out += "</i>";
                italic = false;
                continue;
            }
            if (!italic && i + 1 < s.size() && s[i + 1] != ' ' && (c == '*' || leftEdge)) {
                // open only when a closer follows: a lone '*' (a product sign, a footnote mark) is text
                bool closes = false;
                for (std::size_t k = i + 2; k < s.size() && !closes; ++k)
                    closes = s[k] == c && s[k - 1] != ' ' && s[k - 1] != c && (k + 1 >= s.size() || s[k + 1] != c) &&
                             (c == '*' || k + 1 >= s.size() || !alnum(s[k + 1]));
                if (closes) {
                    out += "<i>";
                    italic = true;
                    italicMark = c;
                    any = true;
                    continue;
                }
            }
        }
        esc(c);
    }
    if (italic) out += "</i>";
    if (bold) out += "</b>";
    return any;
}

} // namespace

void registerTierCD(UiContext& ui) {
    WidgetRegistry& r = ui.registry();
    if (r.find("gridview") != 0xFFFFFFFFu) return;
    r.registerWidget<GridView>();
    r.registerWidget<ColorPickerW>();
    r.registerWidget<CalendarW>();
    r.registerWidget<ChartW>();
    detail::a11yTierCD(r);
}

// ---- civil dates (H. Hinnant's algorithms) ---------------------------------------------------------------------------
i32 daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153u * unsigned(m + (m > 2 ? -3 : 9)) + 2u) / 5u + unsigned(d) - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + i32(doe) - 719468;
}
void civilFromDays(i32 z, int& y, int& m, int& d) {
    z += 719468;
    const i32 era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    d = int(doy - (153u * mp + 2u) / 5u + 1u);
    m = int(mp < 10 ? mp + 3 : mp - 9);
    y = int(yoe) + era * 400 + (m <= 2);
}

// ---- sort / filter proxy -------------------------------------------------------------------------------------------------
namespace {
/// Case-sensitive glob over already lower-cased text: '*' = any run, '?' = one character, whole-string match.
bool globMatch(std::string_view text, std::string_view pat) {
    std::size_t t = 0, p = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == text[t])) { ++t; ++p; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; mark = t; }
        else if (star != std::string_view::npos) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}
} // namespace

SortFilterProxy::SortFilterProxy(const ItemModel* source) : m_src(source), m_notifier(std::make_unique<ModelNotifier>()) {
    using P = SortFilterProxy;
    m_model.user = this;
    m_model.columns = source ? source->columns : 1;
    m_model.notifier = m_notifier.get();
    m_model.rows = [](const void* u) { return static_cast<const SortFilterProxy*>(u)->rows(); };
    m_model.data = [](const void* u, u32 r, u32 c, std::string& scratch) {
        const auto* p = static_cast<const SortFilterProxy*>(u);
        return p->m_src->data(p->m_src->user, p->sourceRow(r), c, scratch);
    };
    if (source && source->header)
        m_model.header = [](const void* u, u32 c) {
            const auto* p = static_cast<const SortFilterProxy*>(u);
            return p->m_src->header(p->m_src->user, c);
        };
    if (!source) return;
    // roles, flags and edits go to the source row
    if (source->role)
        m_model.role = [](const void* u, ItemId i, u32 c, ItemRole r, ItemValue& o, std::string& sc) {
            const P* p = static_cast<const P*>(u);
            return i < p->rows() && p->m_src->role(p->m_src->user, p->sourceRow(u32(i)), c, r, o, sc);
        };
    if (source->flags)
        m_model.flags = [](const void* u, ItemId i, u32 c) -> u32 {
            const P* p = static_cast<const P*>(u);
            return i < p->rows() ? p->m_src->flags(p->m_src->user, p->sourceRow(u32(i)), c) : 0u;
        };
    if (source->setData)
        m_model.setData = [](const void* u, ItemId i, u32 c, ItemRole r, const ItemValue& v) {
            const P* p = static_cast<const P*>(u);
            return i < p->rows() && p->m_src->setData(p->m_src->user, p->sourceRow(u32(i)), c, r, v);
        };
    if (source->notifier)
        m_srcSub = source->notifier->subscribe([](void* u, u64, const ModelChange& c) {
            auto* p = static_cast<P*>(u);
            if (p->m_identity) { p->m_notifier->notify(c); return true; } // same rows: pass the change on as it is
            p->invalidate(); // a filtered / sorted map cannot be patched: rebuild and report a reset
            return true;
        }, this);
}
SortFilterProxy::~SortFilterProxy() {
    if (m_src && m_src->notifier && m_srcSub) m_src->notifier->unsubscribe(m_srcSub);
}
u32 SortFilterProxy::rows() const {
    if (!m_src || !m_src->rows) return 0;
    return m_identity ? m_src->rows(m_src->user) : u32(m_map.size());
}
void SortFilterProxy::setFilter(u32 col, std::string_view needle) {
    m_filterCol = col;
    m_allCols = m_wildcard = false;
    m_needle.assign(needle);
    for (char& c : m_needle) c = char(std::tolower(static_cast<unsigned char>(c)));
    invalidate();
}
void SortFilterProxy::setFilterAllColumns(std::string_view needle) {
    setFilter(0, {});
    m_allCols = true;
    m_needle.assign(needle);
    for (char& c : m_needle) c = char(std::tolower(static_cast<unsigned char>(c)));
    invalidate();
}
void SortFilterProxy::setFilterWildcard(i32 col, std::string_view pattern) {
    m_filterCol = col < 0 ? 0u : u32(col);
    m_allCols = col < 0;
    m_needle.assign(pattern);
    for (char& c : m_needle) c = char(std::tolower(static_cast<unsigned char>(c)));
    m_wildcard = m_needle.find_first_of("*?") != std::string::npos; // no wildcard: a substring match
    invalidate();
}
void SortFilterProxy::sortBy(i32 col, bool ascending) {
    m_sortCol = col;
    m_asc = ascending;
    m_sortKeys = col < 0 ? 0u : 1u;
    invalidate();
}
void SortFilterProxy::addSortKey(u32 col, bool ascending) {
    if (m_sortCol < 0) { sortBy(i32(col), ascending); return; }
    if (m_sortKeys >= kMaxSortKeys) return;
    m_extraKeys[m_sortKeys - 1] = {col, ascending};
    ++m_sortKeys;
    invalidate();
}
bool SortFilterProxy::accepts(u32 r, std::string& scratch, std::string& low) const {
    const u32 c0 = m_allCols ? 0u : m_filterCol, c1 = m_allCols ? m_src->columns : m_filterCol + 1;
    for (u32 c = c0; c < c1; ++c) {
        const std::string_view d = m_src->data(m_src->user, r, c, scratch);
        low.assign(d);
        for (char& ch : low) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (m_wildcard ? globMatch(low, m_needle) : low.find(m_needle) != std::string::npos) return true;
    }
    return false;
}
void SortFilterProxy::invalidate() {
    m_identity = m_needle.empty() && m_sortCol < 0;
    m_map.clear();
    if (m_identity || !m_src || !m_src->rows) { m_map.shrink_to_fit(); m_notifier->reset(); return; }
    const u32 n = m_src->rows(m_src->user);
    std::string scratch, low;
    m_map.reserve(m_needle.empty() ? n : n / 4);
    for (u32 r = 0; r < n; ++r) {
        if (!m_needle.empty() && !accepts(r, scratch, low)) continue;
        m_map.push_back(r);
    }
    if (m_sortCol >= 0 && u32(m_sortCol) < m_src->columns) {
        // sort keys extracted once per key column: numeric when every key parses, else strings
        struct Key { bool asc = true, numeric = true; std::vector<double> num; std::vector<std::string> str; };
        Key keys[kMaxSortKeys];
        u32 nk = 0;
        for (u32 q = 0; q < std::max(1u, m_sortKeys); ++q) {
            const u32 col = q == 0 ? u32(m_sortCol) : m_extraKeys[q - 1].col;
            if (col >= m_src->columns) continue;
            Key& key = keys[nk++];
            key.asc = q == 0 ? m_asc : m_extraKeys[q - 1].asc;
            key.num.resize(m_map.size());
            for (std::size_t i = 0; i < m_map.size() && key.numeric; ++i) key.numeric = parseNum(m_src->data(m_src->user, m_map[i], col, scratch), key.num[i]);
            if (key.numeric) continue;
            key.str.resize(m_map.size());
            for (std::size_t i = 0; i < m_map.size(); ++i) key.str[i].assign(m_src->data(m_src->user, m_map[i], col, scratch));
        }
        std::vector<u32> idx(m_map.size());
        std::iota(idx.begin(), idx.end(), 0u);
        if (nk == 1 && keys[0].numeric) {
            const auto& num = keys[0].num;
            std::stable_sort(idx.begin(), idx.end(), [&](u32 a, u32 b) { return m_asc ? num[a] < num[b] : num[b] < num[a]; });
        } else if (nk == 1) {
            const auto& str = keys[0].str;
            std::stable_sort(idx.begin(), idx.end(), [&](u32 a, u32 b) { return m_asc ? str[a] < str[b] : str[b] < str[a]; });
        } else {
            std::stable_sort(idx.begin(), idx.end(), [&](u32 a, u32 b) {
                for (u32 q = 0; q < nk; ++q) {
                    const Key& key = keys[q];
                    const u32 x = key.asc ? a : b, y = key.asc ? b : a;
                    if (key.numeric) {
                        if (key.num[x] < key.num[y]) return true;
                        if (key.num[y] < key.num[x]) return false;
                    } else if (const int c = key.str[x].compare(key.str[y]); c != 0) {
                        return c < 0;
                    }
                }
                return false;
            });
        }
        std::vector<u32> sorted(m_map.size());
        for (std::size_t i = 0; i < idx.size(); ++i) sorted[i] = m_map[idx[i]];
        m_map = std::move(sorted);
    }
    m_notifier->reset();
}

// ---- grid view selection (behind setViewSelectionMode / setViewSelectionModel) -------------------------------------------
namespace {
u64 packNode(NodeHandle h) { return (u64(h.generation) << 32) | u64(h.index); }
bool gridSelectionChanged(void* user, u64 tag, const SelectionModel& sel) {
    UiContext& ui = *static_cast<UiContext*>(user);
    const NodeHandle n{u32(tag & 0xFFFFFFFFu), u32(tag >> 32)};
    GridState* s = gridPool().get(ui, n);
    if (!s || s->sel != &sel) return false;
    const f32 cur = f32(sel.currentRow());
    ui.invalidate(n);
    if (ui.value(n) != cur) ui.setValue(n, cur, true);
    else ui.emit(n, SignalKind::ValueChanged, {.value = cur, .flag = true});
    return true;
}
} // namespace
namespace detail {
bool gridSetSelection(UiContext& ui, NodeHandle grid, SelectionModel* shared, bool setMode, SelectionMode mode) {
    GridState* s = gridPool().get(ui, grid);
    if (!s || ui.classOf(grid) != ui.registry().find("gridview")) return false;
    if (setMode) s->mode = mode;
    if (setMode && s->sel) { // the model stays; only the rules change
        if (mode == SelectionMode::None) s->sel->clear();
        ui.invalidate(grid);
        return true;
    }
    if (setMode && mode == SelectionMode::Single) return true; // plain single selection needs no model
    if (shared) {
        s->ownSel.reset();
        s->sel = shared;
        if (s->model && s->model->notifier && !shared->tracking()) shared->track(s->model->notifier);
    } else {
        s->ownSel = std::make_unique<SelectionModel>();
        s->sel = s->ownSel.get();
        if (const i64 v = i64(ui.value(grid)); v >= 0 && s->mode != SelectionMode::None) {
            s->sel->selectRow(u32(v));
            s->sel->setCurrent(v);
            s->sel->setAnchor(v);
        }
    }
    s->sel->subscribe(&gridSelectionChanged, &ui, packNode(grid));
    ui.setValue(grid, f32(s->sel->currentRow()), true);
    ui.invalidate(grid);
    return true;
}
SelectionModel* gridSelection(UiContext& ui, NodeHandle grid) {
    GridState* s = gridPool().get(ui, grid);
    return s && ui.classOf(grid) == ui.registry().find("gridview") ? s->sel : nullptr;
}
} // namespace detail

// ---- grid view API -----------------------------------------------------------------------------------------------------
NodeHandle gridView(UiContext& ui, NodeHandle parent, const ItemModel* model, f32 tileW, f32 tileH, std::string_view id) {
    registerTierCD(ui);
    const NodeHandle n = make(ui, "gridview", parent, id);
    if (!ui.alive(n)) return n;
    GridState& s = gridPool().create(ui, n);
    s.model = model;
    s.tileW = std::max(16.f, tileW);
    s.tileH = std::max(16.f, tileH);
    ui.setRange(n, -1.f, 1e9f, 1.f);
    ui.setValue(n, -1.f);
    return n;
}
u32 gridColumns(const UiContext& ui, NodeHandle grid) {
    const GridState* s = gridPool().get(ui, grid);
    return s ? GridView::cols(ui, grid, *s) : 0u;
}
const ItemModel* gridModel(const UiContext& ui, NodeHandle grid) {
    const GridState* s = gridPool().get(ui, grid);
    return s ? s->model : nullptr;
}
Rect gridItemRect(const UiContext& ui, NodeHandle grid, u32 index) {
    const GridState* s = gridPool().get(ui, grid);
    if (!s) return {};
    const Rect c = ui.contentRect(grid);
    const u32 nc = GridView::cols(ui, grid, *s);
    return {c.x + f32(index % nc) * s->tileW, c.y + (f32(index / nc) - f32(s->first)) * s->tileH, s->tileW, s->tileH};
}
i32 gridItemAt(const UiContext& ui, NodeHandle grid, Vec2 p) {
    const GridState* s = gridPool().get(ui, grid);
    if (!s) return -1;
    const Rect c = ui.contentRect(grid);
    if (!c.contains(p) || p.x >= c.right() - GridView::kBar) return -1;
    const i64 nc = GridView::cols(ui, grid, *s);
    const i64 k = i64((p.x - c.x) / s->tileW), r = i64((p.y - c.y) / s->tileH) + s->first;
    const i64 idx = r * nc + k;
    return k < nc && idx < i64(GridView::count(*s)) ? i32(idx) : -1;
}
u32 gridFirstVisibleRow(const UiContext& ui, NodeHandle grid) {
    const GridState* s = gridPool().get(ui, grid);
    return s ? s->first : 0u;
}

// ---- property grid API ------------------------------------------------------------------------------------------------
NodeHandle propertyGrid(UiContext& ui, NodeHandle parent, std::string_view id) {
    const NodeHandle g = panel(ui, parent, id);
    ui.addClass(g, "propgrid");
    ui.setA11y(g, A11yRole::List, "Properties");
    return g;
}
u32 propertyCount(const UiContext& ui, NodeHandle grid) {
    u32 n = 0;
    for (NodeHandle c = ui.firstChild(grid); ui.alive(c); c = ui.nextSibling(c)) ++n;
    return n;
}
NodeHandle propertyRow(UiContext& ui, NodeHandle grid, const PropertyDesc& d, std::string_view value) {
    const u32 index = propertyCount(ui, grid);
    if (d.kind == PropKind::Group) {
        const NodeHandle row = toolButton(ui, grid, "- " + std::string(d.name));
        ui.addClass(row, "propgroup");
        ui.setCss(row, "text-align", "start");
        ui.setUserData(row, u64(PropKind::Group) | (1ull << 8)); // bit 8: open
        ui.connect(row, SignalKind::Clicked, [grid, index](UiContext& u, NodeHandle n, const SignalArgs&) {
            setPropertyGroupOpen(u, grid, index, !(u.userData(n) & (1ull << 8)));
        });
        return row;
    }
    const NodeHandle row = panel(ui, grid);
    ui.addClass(row, "proprow");
    ui.setUserData(row, u64(d.kind));
    const NodeHandle name = label(ui, row, d.name);
    ui.addClass(name, "propname");
    double num = 0.0;
    NodeHandle ed;
    switch (d.kind) {
    case PropKind::Bool:
        ed = checkBox(ui, row, "", value == "true" || value == "1");
        break;
    case PropKind::Number:
        if (!parseNum(value, num)) num = d.lo;
        ed = spinBox(ui, row, d.lo, d.hi, f32(num), d.step);
        break;
    case PropKind::Enum:
        if (!parseNum(value, num)) num = 0.0;
        ed = comboBox(ui, row, d.options, u32(std::max(0.0, num)));
        break;
    case PropKind::Color: {
        const NodeHandle sw = panel(ui, row);
        ui.setCss(sw, "width", "18px");
        ui.setCss(sw, "height", "18px");
        ui.setCss(sw, "border-radius", "3px");
        ui.setCss(sw, "border", "1px #474955");
        ui.setCss(sw, "background", value.empty() ? "#000000" : value);
        ed = lineEdit(ui, row, value);
        ui.connect(ed, SignalKind::TextChanged, [sw](UiContext& u, NodeHandle, const SignalArgs& a) { u.setCss(sw, "background", a.text); });
        break;
    }
    case PropKind::Text:
    default:
        ed = lineEdit(ui, row, value);
        break;
    }
    ui.setCss(ed, "flex-grow", "1");
    const SignalKind k = d.kind == PropKind::Bool ? SignalKind::Toggled : d.kind == PropKind::Number || d.kind == PropKind::Enum ? SignalKind::ValueChanged : SignalKind::TextChanged;
    ui.connect(ed, k, [grid, index, name](UiContext& u, NodeHandle, const SignalArgs&) {
        u.emit(grid, SignalKind::Submitted, {.value = f32(index), .text = u.text(name)});
    });
    return ed;
}
std::string propertyValue(const UiContext& ui, NodeHandle grid, u32 index) {
    const NodeHandle row = nthChild(ui, grid, index);
    if (!ui.alive(row)) return {};
    const PropKind k = PropKind(ui.userData(row) & 0xFF);
    if (k == PropKind::Group) return {};
    NodeHandle ed = nthChild(ui, row, k == PropKind::Color ? 2 : 1);
    char b[32];
    switch (k) {
    case PropKind::Bool: return ui.checked(ed) ? "true" : "false";
    case PropKind::Number: std::snprintf(b, sizeof b, "%g", double(ui.value(ed))); return b;
    case PropKind::Enum: return std::to_string(i32(ui.value(ed)));
    default: return std::string(ui.text(ed));
    }
}
void setPropertyGroupOpen(UiContext& ui, NodeHandle grid, u32 groupIndex, bool open) {
    const NodeHandle g = nthChild(ui, grid, groupIndex);
    if (!ui.alive(g) || PropKind(ui.userData(g) & 0xFF) != PropKind::Group) return;
    ui.setUserData(g, u64(PropKind::Group) | (open ? (1ull << 8) : 0ull));
    std::string t(ui.text(g));
    if (t.size() > 1) t[0] = open ? '-' : '+';
    ui.setText(g, t);
    for (NodeHandle c = ui.nextSibling(g); ui.alive(c) && PropKind(ui.userData(c) & 0xFF) != PropKind::Group; c = ui.nextSibling(c))
        ui.setCss(c, "display", open ? "flex" : "none");
}

// ---- colour picker API --------------------------------------------------------------------------------------------------
NodeHandle colorPicker(UiContext& ui, NodeHandle parent, Color initial, std::string_view id) {
    registerTierCD(ui);
    const NodeHandle n = make(ui, "colorpicker", parent, id);
    if (!ui.alive(n)) return n;
    ui.setRange(n, 0.f, 360.f, 0.f);
    setColorPickerColor(ui, n, initial);
    return n;
}
Color colorPickerColor(const UiContext& ui, NodeHandle picker) {
    const Hsva h = unpack(ui.userData(picker));
    return hsvToRgb(h.h, h.s, h.v, h.a);
}
void setColorPickerColor(UiContext& ui, NodeHandle picker, Color c) {
    Hsva h = rgbToHsv(c);
    h.h = std::min(h.h, 0.9999f);
    ui.setUserData(picker, pack(h));
    char b[16];
    auto q = [](f32 x) { return unsigned(std::lround(std::clamp(x, 0.f, 1.f) * 255.f)); };
    std::snprintf(b, sizeof b, "#%02X%02X%02X%02X", q(c.r), q(c.g), q(c.b), q(c.a));
    ui.setText(picker, b);
    ui.setValue(picker, h.h * 360.f);
    ui.invalidate(picker);
}

// ---- calendar API -------------------------------------------------------------------------------------------------------
NodeHandle calendar(UiContext& ui, NodeHandle parent, int year, int month, int day, std::string_view id) {
    registerTierCD(ui);
    const NodeHandle n = make(ui, "calendar", parent, id);
    if (!ui.alive(n)) return n;
    ui.setRange(n, -1e7f, 1e7f, 1.f);
    month = std::clamp(month, 1, 12);
    day = std::clamp(day, 1, daysIn(year, month));
    CalendarW::setShown(ui, n, year * 12 + month - 1);
    ui.setValue(n, f32(daysFromCivil(year, month, day)));
    return n;
}
void calendarDate(const UiContext& ui, NodeHandle cal, int& year, int& month, int& day) {
    civilFromDays(i32(ui.value(cal)), year, month, day);
}

// ---- chart API ------------------------------------------------------------------------------------------------------------
NodeHandle chart(UiContext& ui, NodeHandle parent, ChartKind kind, std::string_view id) {
    registerTierCD(ui);
    const NodeHandle n = make(ui, "chart", parent, id);
    if (!ui.alive(n)) return n;
    chartPool().create(ui, n).kind = kind;
    ui.setRange(n, -1.f, 1e9f, 1.f);
    ui.setValue(n, -1.f);
    return n;
}
void chartSetSeries(UiContext& ui, NodeHandle c, u32 index, std::string_view name, std::span<const f32> values, Color color) {
    ChartState* s = chartPool().get(ui, c);
    if (!s || index > 64) return;
    if (s->series.size() <= index) s->series.resize(index + 1);
    s->series[index] = {std::string(name), std::vector<f32>(values.begin(), values.end()), color};
    ui.invalidate(c);
}
u32 chartSeriesCount(const UiContext& ui, NodeHandle c) {
    const ChartState* s = chartPool().get(ui, c);
    return s ? u32(s->series.size()) : 0u;
}
void chartSetFlame(UiContext& ui, NodeHandle c, std::span<const FlameSpan> spans) {
    ChartState* s = chartPool().get(ui, c);
    if (!s) return;
    s->kind = ChartKind::Flame;
    s->flame.clear();
    s->flame.reserve(spans.size());
    for (const FlameSpan& f : spans) s->flame.push_back({std::string(f.name), f.start, f.duration, f.depth, f.color});
    ui.setValue(c, -1.f); // the hovered index belonged to the old spans
    ui.invalidate(c);
}
u32 chartFlameSpanCount(const UiContext& ui, NodeHandle c) {
    const ChartState* s = chartPool().get(ui, c);
    return s ? u32(s->flame.size()) : 0u;
}
Rect chartFlameSpanRect(const UiContext& ui, NodeHandle c, u32 index) {
    const ChartState* s = chartPool().get(ui, c);
    return s && s->kind == ChartKind::Flame ? ChartW::flameRect(ui, c, *s, index) : Rect{};
}
i32 chartFlameSpanAt(const UiContext& ui, NodeHandle c, Vec2 pos) {
    const ChartState* s = chartPool().get(ui, c);
    return s && s->kind == ChartKind::Flame ? ChartW::flameAt(ui, c, *s, pos) : -1;
}

// ---- markdown view --------------------------------------------------------------------------------------------------------
NodeHandle markdownView(UiContext& ui, NodeHandle parent, std::string_view md, std::string_view id) {
    const NodeHandle v = panel(ui, parent, id);
    ui.addClass(v, "md");
    ui.setA11y(v, A11yRole::Group, "Document");
    setMarkdown(ui, v, md);
    return v;
}
void setMarkdown(UiContext& ui, NodeHandle view, std::string_view md) {
    std::vector<NodeHandle> old;
    for (NodeHandle c = ui.firstChild(view); ui.alive(c); c = ui.nextSibling(c)) old.push_back(c);
    for (NodeHandle c : old) ui.destroy(c);
    std::string para, code;
    bool inCode = false;
    std::string markup;
    auto emit = [&](std::string text, std::string_view cls) {
        // a block with **bold** / *italic* is a rich text view (real or synthesised faces); its node text is the
        // plain text, like a label's
        const bool rich = cls != "md-code" && inlineToRich(text, markup);
        if (cls != "md-code") stripInline(text);
        if (rich) {
            for (std::size_t k = 0; (k = text.find_first_of("*_", k)) != std::string::npos;) {
                // single emphasis marks that inlineToRich consumed: the plain text drops them too
                const bool word = k > 0 && k + 1 < text.size() && std::isalnum(static_cast<unsigned char>(text[k - 1])) &&
                                  std::isalnum(static_cast<unsigned char>(text[k + 1])) && text[k] == '_';
                if (word) ++k;
                else text.erase(k, 1);
            }
        }
        const NodeHandle l = rich ? richText(ui, view, markup) : label(ui, view, text);
        if (rich) ui.setText(l, text);
        ui.addClass(l, cls);
    };
    auto flush = [&] { if (!para.empty()) { emit(para, "md-p"); para.clear(); } };
    std::size_t at = 0;
    while (at <= md.size()) {
        std::size_t nl = md.find('\n', at);
        if (nl == std::string_view::npos) nl = md.size();
        std::string_view line = md.substr(at, nl - at);
        at = nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with("```")) {
            if (inCode) { if (!code.empty() && code.back() == '\n') code.pop_back(); emit(code, "md-code"); code.clear(); }
            else flush();
            inCode = !inCode;
            continue;
        }
        if (inCode) { code.append(line); code += '\n'; continue; }
        std::size_t lead = line.find_first_not_of(' ');
        if (lead == std::string_view::npos) { flush(); continue; }
        line.remove_prefix(lead);
        if (line.starts_with("### ")) { flush(); emit(std::string(line.substr(4)), "md-h3"); }
        else if (line.starts_with("## ")) { flush(); emit(std::string(line.substr(3)), "md-h2"); }
        else if (line.starts_with("# ")) { flush(); emit(std::string(line.substr(2)), "md-h1"); }
        else if (line.starts_with("- ") || line.starts_with("* ")) { flush(); emit("\xE2\x80\xA2 " + std::string(line.substr(2)), "md-li"); }
        else if (line.starts_with("> ")) { flush(); emit(std::string(line.substr(2)), "md-quote"); }
        else if (std::size_t d = line.find(". "); d != std::string_view::npos && d > 0 && d < 4 &&
                 line.substr(0, d).find_first_not_of("0123456789") == std::string_view::npos) { flush(); emit(std::string(line), "md-li"); }
        else { if (!para.empty()) para += ' '; para.append(line); }
        if (nl == md.size()) break;
    }
    if (inCode && !code.empty()) emit(code, "md-code");
    flush();
}

} // namespace fuse::ui::widgets

#include "widgets_internal.hpp"
#include <cstdio>
// ---- accessibility (plan §4.14): grid view, calendar, colour picker -----------------------------------------------------------
namespace fuse::ui::widgets {
namespace {
// Grid view: the visible tiles are list items (id = item index + 1).
void gridA11y(const UiContext& ui, NodeHandle n, A11ySnapshotNode& self, std::vector<A11ySnapshotNode>& items) {
    GridState* s = gridPool().get(ui, n);
    if (!s || !s->model) return;
    const Rect c = ui.contentRect(n);
    const u32 nc = GridView::cols(ui, n, *s), total = GridView::count(*s), vr = GridView::visRows(ui, n, *s);
    const i64 sel = i64(ui.value(n));
    self.setSize = total;
    if (GridView::rowsTotal(ui, n, *s) > vr && !self.disabled)
        self.actions |= detail::a11yBits({A11yAction::ScrollUp, A11yAction::ScrollDown});
    for (u32 i = s->first * nc; i < std::min(total, (s->first + vr) * nc); ++i) {
        const Rect r{c.x + f32(i % nc) * s->tileW, c.y + f32(i / nc - s->first) * s->tileH, s->tileW, s->tileH};
        A11ySnapshotNode it = detail::a11yItem(i + 1, A11yRole::ListItem,
                                               s->model->data ? s->model->data(s->model->user, i, 0, s->scratch) : std::string_view{}, r,
                                               self.disabled ? 0u : detail::kA11yItemActions);
        it.selectable = s->mode != SelectionMode::None;
        it.selected = it.selectable && (s->sel ? s->sel->rowSelected(i) : i64(i) == sel); // (a selection model: any mode)
        it.posInSet = i + 1;
        it.setSize = total;
        items.push_back(std::move(it));
    }
}
bool gridA11yAction(UiContext& ui, NodeHandle n, u32 child, A11yAction a, f32, std::string_view) {
    GridState* s = gridPool().get(ui, n);
    if (!s || !child || child > GridView::count(*s)) return false;
    const u32 idx = child - 1, row = idx / GridView::cols(ui, n, *s);
    switch (a) {
    case A11yAction::Select: // idempotent: a selected tile only becomes current; Click follows the mode's pointer rules
        if (s->sel && s->sel->rowSelected(idx)) { s->sel->setCurrent(idx); return true; }
        [[fallthrough]];
    case A11yAction::Click: GridView::select(ui, n, *s, idx, 0, true); return true;
    case A11yAction::ScrollIntoView: {
        const u32 vr = GridView::visRows(ui, n, *s);
        if (row < s->first) GridView::setFirst(ui, n, *s, row);
        else if (row >= s->first + vr) GridView::setFirst(ui, n, *s, i64(row) - i64(vr) + 1);
        return true;
    }
    default: return false;
    }
}
// Calendar: the value is the selected date (ISO); Increment / Decrement move by a day, SetValue takes days since 1970.
void calendarA11y(const UiContext& ui, NodeHandle n, A11ySnapshotNode& self, std::vector<A11ySnapshotNode>&) {
    int y = 0, m = 0, d = 0;
    calendarDate(ui, n, y, m, d);
    char b[32];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d);
    self.valueText = b;
    if (self.name.empty()) self.name = "Calendar";
    if (!self.disabled) self.actions |= detail::a11yBits({A11yAction::Increment, A11yAction::Decrement, A11yAction::SetValue});
}
bool calendarA11yAction(UiContext& ui, NodeHandle n, u32 child, A11yAction a, f32 value, std::string_view) {
    if (child) return false;
    switch (a) {
    case A11yAction::Increment: ui.setValue(n, ui.value(n) + 1.f, true); return true;
    case A11yAction::Decrement: ui.setValue(n, ui.value(n) - 1.f, true); return true;
    case A11yAction::SetValue: ui.setValue(n, std::round(value), true); return true;
    default: return false;
    }
}
// Colour picker: the value is "#RRGGBBAA".
void colorPickerA11y(const UiContext& ui, NodeHandle n, A11ySnapshotNode& self, std::vector<A11ySnapshotNode>&) {
    self.valueText.assign(ui.text(n));
    if (self.name == self.valueText) self.name = "Colour";
}
void chartA11y(const UiContext& ui, NodeHandle n, A11ySnapshotNode& self, std::vector<A11ySnapshotNode>&) {
    if (!self.name.empty()) return;
    char b[48];
    if (const u32 spans = chartFlameSpanCount(ui, n)) std::snprintf(b, sizeof b, "Flame graph, %u spans", spans);
    else std::snprintf(b, sizeof b, "Chart, %u series", chartSeriesCount(ui, n));
    self.name = b;
}
} // namespace
void detail::a11yTierCD(WidgetRegistry& r) {
    a11yHook(r, "gridview", A11yRole::List, &gridA11y, &gridA11yAction);
    a11yHook(r, "calendar", A11yRole::Group, &calendarA11y, &calendarA11yAction);
    a11yHook(r, "colorpicker", A11yRole::Group, &colorPickerA11y);
    a11yHook(r, "chart", A11yRole::Image, &chartA11y);
}
} // namespace fuse::ui::widgets
