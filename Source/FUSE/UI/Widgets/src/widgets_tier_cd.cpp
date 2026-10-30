// FUSE UI Tier C (model / view: sort-filter proxy, grid view; delegates live with the table in widgets_tier_b2.cpp)
// and Tier D (property grid, colour picker, calendar, chart, markdown view). Plan §5.
#include "fuse/ui/from_chars.hpp"
#include "fuse/ui/widgets/widgets.hpp"
#include "widgets_internal.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <numeric>
#include <unordered_map>
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
    const auto [p, ec] = fuse::ui::fromChars(s.data(), s.data() + s.size(), out);
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
};
Pool<GridState>& gridPool() { static Pool<GridState> p; return p; }

struct GridView {
    static constexpr std::string_view kName = "gridview";
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
    static void select(UiContext& ui, NodeHandle n, GridState& s, i64 idx) {
        const u32 total = count(s);
        if (!total) return;
        idx = std::clamp<i64>(idx, 0, i64(total) - 1);
        const u32 c = cols(ui, n, s), row = u32(idx) / c, v = visRows(ui, n, s);
        if (row < s.first) setFirst(ui, n, s, row);
        else if (row >= s.first + v) setFirst(ui, n, s, i64(row) - v + 1);
        ui.setValue(n, f32(idx), true);
    }
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
                if (i32(idx) == sel) dl.rect({t.x + 2.f, t.y + 2.f, t.w - 4.f, t.h - 4.f}, acc.withAlpha(acc.a * 0.45f), 5.f);
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
                if (idx < i64(count(*s))) select(ui, ec.node, *s, idx);
            }
            return true;
        }
        case EventType::Wheel: setFirst(ui, ec.node, *s, i64(s->first) - i64(std::lround(e.wheel.y))); return true;
        case EventType::KeyDown: {
            const i64 cur = i64(ui.value(ec.node));
            switch (e.key) {
            case Key::Right: select(ui, ec.node, *s, cur + 1); return true;
            case Key::Left: select(ui, ec.node, *s, cur < 0 ? 0 : cur - 1); return true;
            case Key::Down: select(ui, ec.node, *s, cur < 0 ? 0 : cur + nc); return true;
            case Key::Up: select(ui, ec.node, *s, cur - nc); return true;
            case Key::PageDown: select(ui, ec.node, *s, cur + nc * visRows(ui, ec.node, *s)); return true;
            case Key::PageUp: select(ui, ec.node, *s, cur - nc * visRows(ui, ec.node, *s)); return true;
            case Key::Home: select(ui, ec.node, *s, 0); return true;
            case Key::End: select(ui, ec.node, *s, i64(count(*s)) - 1); return true;
            case Key::Enter: if (cur >= 0) ui.emit(ec.node, SignalKind::Submitted, {.value = f32(cur)}); return true;
            default: return false;
            }
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

struct CalState { u32 gen = 0; CalendarOptions o; };
std::unordered_map<const UiContext*, std::unordered_map<u32, CalState>>& calStores() {
    static std::unordered_map<const UiContext*, std::unordered_map<u32, CalState>> m;
    return m;
}
const CalendarOptions& calOpts(const UiContext& ui, NodeHandle n) {
    static const CalendarOptions def;
    auto it = calStores().find(&ui);
    if (it == calStores().end()) return def;
    auto j = it->second.find(n.index);
    return j != it->second.end() && j->second.gen == n.generation ? j->second.o : def;
}
/// ISO 8601 week number (weeks start on Monday; week 1 holds the year's first Thursday)
int isoWeek(i32 z) {
    const i32 thu = z - weekday(z) + 3;
    int y, m, d;
    civilFromDays(thu, y, m, d);
    return (thu - daysFromCivil(y, 1, 1)) / 7 + 1;
}

struct CalendarW {
    static constexpr std::string_view kName = "calendar";
    static constexpr A11yRole kRole = A11yRole::Group;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static std::string a11yValueText(const UiContext& ui, NodeHandle n) {
        int y, m, d;
        civilFromDays(i32(ui.value(n)), y, m, d);
        char b[64];
        std::snprintf(b, sizeof b, "%s %d %s %d", kDayNamesFull[weekday(i32(ui.value(n)))], d, kMonths[m - 1], y);
        return b;
    }
    static constexpr const char* kDayNamesFull[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    static int cols(const UiContext& ui, NodeHandle n) { return calOpts(ui, n).weekNumbers ? 8 : 7; }
    static Size measure(UiContext& ui, NodeHandle n, Size) { return {f32(cols(ui, n)) * 34.f, lineH(ui, n) + 8.f + 7.f * 26.f}; }
    static i32 shownMonth(const UiContext& ui, NodeHandle n) { return i32(ui.userData(n)) - 1000000; } // y * 12 + (m - 1)
    static void setShown(UiContext& ui, NodeHandle n, i32 ym) {
        const bool changed = ym != shownMonth(ui, n);
        ui.setUserData(n, u64(ym + 1000000));
        ui.invalidate(n);
        if (changed) ui.emit(n, SignalKind::TextChanged, {.value = f32(ym)}); // QCalendarWidget::currentPageChanged
    }
    struct Geo { Rect head, prev, next; f32 cellW, cellH, gridY, gridX; };
    static Geo geo(const UiContext& ui, NodeHandle n) {
        const Rect c = ui.contentRect(n);
        Geo g;
        const f32 hh = lineH(ui, n) + 8.f;
        g.head = {c.x, c.y, c.w, hh};
        g.prev = {c.x, c.y, hh, hh};
        g.next = {c.right() - hh, c.y, hh, hh};
        g.cellW = c.w / f32(cols(ui, n));
        g.cellH = (c.h - hh) / 7.f;
        g.gridY = c.y + hh + g.cellH; // first week row (below the weekday names)
        g.gridX = c.x + (cols(ui, n) == 8 ? g.cellW : 0.f);
        return g;
    }
    static int col(const UiContext& ui, NodeHandle n, i32 z) { return (weekday(z) - int(calOpts(ui, n).firstDayOfWeek) + 7) % 7; }
    static i32 firstCellDay(const UiContext& ui, NodeHandle n, i32 ym) { // day number of the grid's top-left cell
        const i32 first = daysFromCivil(ym / 12, ym % 12 + 1, 1);
        return first - col(ui, n, first);
    }
    static bool inRange(const UiContext& ui, NodeHandle n, i32 z) { return f32(z) >= ui.rangeMin(n) && f32(z) <= ui.rangeMax(n); }
    static bool pageInRange(const UiContext& ui, NodeHandle n, i32 ym) {
        const i32 first = daysFromCivil(ym / 12, ym % 12 + 1, 1), last = first + daysIn(ym / 12, ym % 12 + 1) - 1;
        return f32(last) >= ui.rangeMin(n) && f32(first) <= ui.rangeMax(n);
    }
    static void select(UiContext& ui, NodeHandle n, i32 z) {
        z = std::clamp(z, i32(ui.rangeMin(n)), i32(ui.rangeMax(n))); // QCalendarWidget: never past min / max
        int y, m, d;
        civilFromDays(z, y, m, d);
        setShown(ui, n, y * 12 + m - 1);
        ui.setValue(n, f32(z), true);
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        const Geo g = geo(ui, pc.node);
        const CalendarOptions& o = calOpts(ui, pc.node);
        const i32 ym = shownMonth(ui, pc.node), sel = i32(ui.value(pc.node));
        const Color text = pc.tint(pc.style.color), dim = pc.tint(pc.style.color.withAlpha(pc.style.color.a * 0.38f)), acc = accentOf(pc);
        const Color off = pc.tint(pc.style.color.withAlpha(pc.style.color.a * 0.16f));
        const f32 fs = pc.style.fontSize;
        char b[48];
        std::snprintf(b, sizeof b, "%s %d", kMonths[ym % 12], ym / 12);
        ui.paintString(dl, b, g.head, text, fs, TextAlignment::Center, pc.node.index);
        for (int side = 0; side < 2; ++side) { // chevrons, dimmed when the page beyond is out of range
            const Rect r = side ? g.next : g.prev;
            const Color cc = pageInRange(ui, pc.node, ym + (side ? 1 : -1)) ? text : off;
            const f32 cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, d = side ? 3.f : -3.f;
            dl.line({cx - d, cy - 5.f}, {cx + d, cy}, cc, 1.5f);
            dl.line({cx + d, cy}, {cx - d, cy + 5.f}, cc, 1.5f);
        }
        for (int k = 0; k < 7; ++k) {
            const int wd = (k + int(o.firstDayOfWeek)) % 7;
            ui.paintString(dl, kDays[wd], {g.gridX + f32(k) * g.cellW, g.head.bottom(), g.cellW, g.cellH}, wd >= 5 ? dim : text,
                           fs * 0.85f, TextAlignment::Center, pc.node.index);
        }
        const i32 z0 = firstCellDay(ui, pc.node, ym);
        for (int i = 0; i < 42; ++i) {
            const i32 z = z0 + i;
            int y, m, d;
            civilFromDays(z, y, m, d);
            if (o.weekNumbers && i % 7 == 0) {
                std::snprintf(b, sizeof b, "%d", isoWeek(z));
                ui.paintString(dl, b, {g.head.x, g.gridY + f32(i / 7) * g.cellH, g.cellW, g.cellH}, dim, fs * 0.8f,
                               TextAlignment::Center, pc.node.index);
            }
            const Rect cell{g.gridX + f32(i % 7) * g.cellW, g.gridY + f32(i / 7) * g.cellH, g.cellW, g.cellH};
            const bool inMonth = y * 12 + m - 1 == ym;
            std::snprintf(b, sizeof b, "%d", d);
            if (!inRange(ui, pc.node, z)) { // out of range: greyed, not selectable
                ui.paintString(dl, b, cell, off, fs * 0.9f, TextAlignment::Center, pc.node.index);
                continue;
            }
            if (z == sel) dl.rect({cell.x + 2.f, cell.y + 1.f, cell.w - 4.f, cell.h - 2.f}, acc, 4.f);
            ui.paintString(dl, b, cell, z == sel ? pc.tint(Color::rgb(0xFFFFFF)) : inMonth ? text : dim, fs * 0.9f, TextAlignment::Center,
                           pc.node.index);
        }
        if (pc.states & kStateFocus) dl.roundedRect(pc.rect, {}, {5.f, 5.f, 5.f, 5.f}, acc.withAlpha(acc.a * 0.6f), 1.f);
    }
    static void pageMonth(UiContext& ui, NodeHandle n, i32 delta, bool moveSelection) {
        const i32 ym = shownMonth(ui, n) + delta;
        if (!moveSelection) { if (pageInRange(ui, n, ym)) setShown(ui, n, ym); return; }
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
            if (e.pos.y >= g.gridY && e.pos.x >= g.gridX) {
                const i32 row = i32((e.pos.y - g.gridY) / g.cellH), c = i32((e.pos.x - g.gridX) / g.cellW);
                if (row >= 0 && row < 6 && c >= 0 && c < 7) {
                    const i32 z = firstCellDay(ui, ec.node, shownMonth(ui, ec.node)) + row * 7 + c;
                    if (inRange(ui, ec.node, z)) select(ui, ec.node, z);
                }
            }
            return true;
        }
        if (e.type == EventType::KeyDown) {
            const i32 cur = i32(ui.value(ec.node));
            const bool ctrl = (e.mods & (kModCtrl | kModSuper)) != 0;
            int y, m, d;
            civilFromDays(cur, y, m, d);
            switch (e.key) {
            case Key::Left: select(ui, ec.node, cur - 1); return true;
            case Key::Right: select(ui, ec.node, cur + 1); return true;
            case Key::Up: select(ui, ec.node, cur - 7); return true;
            case Key::Down: select(ui, ec.node, cur + 7); return true;
            case Key::Home: select(ui, ec.node, ctrl ? daysFromCivil(y, m, 1) : cur - col(ui, ec.node, cur)); return true;
            case Key::End: select(ui, ec.node, ctrl ? daysFromCivil(y, m, daysIn(y, m)) : cur - col(ui, ec.node, cur) + 6); return true;
            case Key::PageUp: pageMonth(ui, ec.node, ctrl ? -12 : -1, true); return true;
            case Key::PageDown: pageMonth(ui, ec.node, ctrl ? 12 : 1, true); return true;
            case Key::Enter: ui.emit(ec.node, SignalKind::Submitted, {.value = f32(cur)}); return true;
            default: return false;
            }
        }
        return false;
    }
};

// ---- chart ----------------------------------------------------------------------------------------------------------------
struct Series { std::string name; std::vector<f32> v; Color color; bool hidden = false; };
struct ChartState {
    ChartKind kind = ChartKind::Line;
    std::vector<Series> series;
    ChartAxis axis[2];
    ChartLegend legend = ChartLegend::Top;
    std::vector<Color> sliceColors;
    std::vector<bool> sliceHidden;
    f32 vx0 = std::numeric_limits<f32>::quiet_NaN(), vx1 = std::numeric_limits<f32>::quiet_NaN(); // zoomed x view
    Vec2 pointer{};
    bool inside = false;
    f32 dragX = 0.f, dragV0 = 0.f, dragV1 = 0.f;
};
Pool<ChartState>& chartPool() { static Pool<ChartState> p; return p; }

constexpr u32 kPalette[] = {0x3D8BFD, 0xFFB347, 0x4CC38A, 0xC77DFF, 0xFF6B6B, 0x2EC4B6, 0xF7D154, 0x9A9CAB};

std::string fmtNum(const std::string& f, double v) {
    // the format is programmer-supplied; only a single float conversion is honoured, anything else falls back
    std::size_t pc = 0;
    for (std::size_t i = 0; i < f.size(); ++i)
        if (f[i] == '%') { if (i + 1 < f.size() && f[i + 1] == '%') ++i; else ++pc; }
    const std::size_t conv = f.find_first_of("gfeGFE", f.find('%') == std::string::npos ? f.size() : f.find('%'));
    char b[64];
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    if (pc == 1 && conv != std::string::npos && f.find_first_of("sdnpcxXuoi*", f.find('%')) > conv) std::snprintf(b, sizeof b, f.c_str(), v);
    else std::snprintf(b, sizeof b, "%.4g", v);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    return b;
}

struct ChartW {
    static constexpr std::string_view kName = "chart";
    static constexpr A11yRole kRole = A11yRole::Image;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {360.f, 200.f}; }
    static bool radial(const ChartState& s) { return s.kind == ChartKind::Pie || s.kind == ChartKind::Donut; }
    static u32 points(const ChartState& s) {
        u32 n = 0;
        for (const Series& se : s.series) n = std::max(n, u32(se.v.size()));
        return n;
    }
    static f32 fontPx(const UiContext& ui, NodeHandle n) { return ui.computed(n).fontSize * 0.8f; }
    // legend entries: series for x/y charts, slices for pie / donut
    static u32 entries(const ChartState& s) { return radial(s) ? (s.series.empty() ? 0u : u32(s.series[0].v.size())) : u32(s.series.size()); }
    static std::string entryName(const ChartState& s, u32 i) {
        if (!radial(s)) return s.series[i].name;
        const auto& cat = s.axis[0].categories;
        return i < cat.size() ? cat[i] : "#" + std::to_string(i);
    }
    static Color sliceColor(const ChartState& s, u32 i) {
        return i < s.sliceColors.size() ? s.sliceColors[i] : Color::rgb(kPalette[i % std::size(kPalette)]);
    }
    static Color entryColor(const ChartState& s, u32 i) { return radial(s) ? sliceColor(s, i) : s.series[i].color; }
    static bool entryHidden(const ChartState& s, u32 i) {
        return radial(s) ? (i < s.sliceHidden.size() && s.sliceHidden[i]) : s.series[i].hidden;
    }
    static std::vector<Rect> legendItems(const UiContext& cui, NodeHandle n, const ChartState& s) {
        UiContext& ui = const_cast<UiContext&>(cui); // measureString only fills the text cache
        std::vector<Rect> out;
        if (s.legend == ChartLegend::None) return out;
        const Rect c = ui.contentRect(n);
        const f32 fs = fontPx(ui, n), lh = lineH(ui, n);
        const u32 ne = entries(s);
        if (s.legend == ChartLegend::Right) {
            f32 w = 0.f;
            for (u32 i = 0; i < ne; ++i) w = std::max(w, ui.measureString(entryName(s, i), fs) + 22.f);
            for (u32 i = 0; i < ne; ++i) out.push_back({c.right() - w, c.y + f32(i) * lh, w, lh});
            return out;
        }
        f32 total = 0.f;
        for (u32 i = 0; i < ne; ++i) total += ui.measureString(entryName(s, i), fs) + 22.f;
        f32 x = s.legend == ChartLegend::Top ? c.right() - total : c.x + (c.w - total) * 0.5f;
        const f32 y = s.legend == ChartLegend::Top ? c.y : c.bottom() - lh;
        for (u32 i = 0; i < ne; ++i) {
            const f32 w = ui.measureString(entryName(s, i), fs) + 22.f;
            out.push_back({x, y, w, lh});
            x += w;
        }
        return out;
    }
    static Rect plot(const UiContext& ui, NodeHandle n, const ChartState& s) {
        const Rect c = ui.contentRect(n);
        const f32 lh = lineH(ui, n);
        f32 l = c.x + 6.f, t = c.y + 4.f, r = c.right() - 6.f, b = c.bottom() - 4.f;
        if (s.legend == ChartLegend::Top) t += lh + 2.f;
        if (s.legend == ChartLegend::Bottom) b -= lh + 2.f;
        if (s.legend == ChartLegend::Right) {
            const auto items = legendItems(ui, n, s);
            if (!items.empty()) r = items[0].x - 8.f;
        }
        if (!radial(s)) {
            const ChartAxis& ax = s.axis[0];
            const ChartAxis& ay = s.axis[1];
            if (ay.visible) l += 36.f;
            if (!ay.title.empty()) t += lh;
            else if (s.legend != ChartLegend::Top) t += lh * 0.5f;
            if (ax.visible) b -= lh + 2.f;
            if (!ax.title.empty()) b -= lh;
        }
        return {l, t, std::max(1.f, r - l), std::max(1.f, b - t)};
    }
    // full x extent in index units (bars are centred on slots)
    static void fullView(const ChartState& s, f32& a, f32& b) {
        const u32 n = points(s);
        if (s.kind == ChartKind::Bar) { a = -0.5f; b = f32(n) - 0.5f; }
        else { a = 0.f; b = n > 1 ? f32(n - 1) : 1.f; }
    }
    static void view(const ChartState& s, f32& a, f32& b) {
        if (std::isnan(s.vx0) || std::isnan(s.vx1)) fullView(s, a, b);
        else { a = s.vx0; b = s.vx1; }
    }
    static void range(const ChartState& s, f32& lo, f32& hi, f32& step) {
        const ChartAxis& ay = s.axis[1];
        f32 a, b;
        view(s, a, b);
        lo = 1e30f; hi = -1e30f;
        for (const Series& se : s.series) {
            if (se.hidden) continue;
            for (u32 i = 0; i < se.v.size(); ++i)
                if (f32(i) >= a - 1.f && f32(i) <= b + 1.f) { lo = std::min(lo, se.v[i]); hi = std::max(hi, se.v[i]); }
        }
        if (lo > hi) { lo = 0.f; hi = 1.f; }
        if (s.kind == ChartKind::Bar || s.kind == ChartKind::Area) { lo = std::min(lo, 0.f); hi = std::max(hi, 0.f); }
        const bool fixLo = !std::isnan(ay.min), fixHi = !std::isnan(ay.max);
        if (fixLo) lo = ay.min;
        if (fixHi) hi = ay.max;
        if (hi - lo < 1e-6f) hi = lo + 1.f;
        const f32 ticks = f32(std::max(1u, ay.ticks));
        // nice step: 1, 2 or 5 x 10^k for ~`ticks` intervals (explicit bounds are kept exactly)
        const f32 raw = (hi - lo) / ticks, mag = std::pow(10.f, std::floor(std::log10(raw)));
        step = raw / mag <= 1.f ? mag : raw / mag <= 2.f ? 2.f * mag : raw / mag <= 5.f ? 5.f * mag : 10.f * mag;
        if (fixLo && fixHi) { step = (hi - lo) / ticks; return; }
        if (!fixLo) lo = std::floor(lo / step) * step;
        if (!fixHi) hi = std::ceil(hi / step) * step;
    }
    static f32 xOf(const Rect& p, f32 i, f32 a, f32 b) { return p.x + (i - a) / std::max(1e-6f, b - a) * p.w; }
    // ---- pie / donut geometry
    struct Wedge { f32 a0, a1; u32 idx; };
    static std::vector<Wedge> wedges(const ChartState& s, f32& total) {
        std::vector<Wedge> out;
        total = 0.f;
        if (s.series.empty()) return out;
        const auto& v = s.series[0].v;
        for (u32 i = 0; i < v.size(); ++i) if (!entryHidden(s, i)) total += std::max(0.f, v[i]);
        if (total <= 0.f) return out;
        f32 a = -kPi * 0.5f;
        for (u32 i = 0; i < v.size(); ++i) {
            if (entryHidden(s, i) || v[i] <= 0.f) continue;
            const f32 sw = v[i] / total * 2.f * kPi;
            out.push_back({a, a + sw, i});
            a += sw;
        }
        return out;
    }
    static constexpr f32 kPi = 3.14159265358979f;
    static void pieGeo(const Rect& p, Vec2& c, f32& r) {
        c = {p.x + p.w * 0.5f, p.y + p.h * 0.5f};
        r = std::max(4.f, std::min(p.w, p.h) * 0.5f - 8.f);
    }
    static Path wedgePath(Vec2 c, f32 r0, f32 r1, f32 a0, f32 a1) {
        Path path;
        auto at = [&](f32 r, f32 a) { return Vec2{c.x + std::cos(a) * r, c.y + std::sin(a) * r}; };
        const u32 chunks = u32(std::ceil((a1 - a0) / (kPi * 0.5f))); // arcs in <= 90 degree pieces (full circle safe)
        const f32 st = (a1 - a0) / f32(std::max(1u, chunks));
        if (r0 > 0.f) path.moveTo(at(r0, a0)); else path.moveTo(c);
        path.lineTo(at(r1, a0));
        for (u32 k = 1; k <= chunks; ++k) path.arcTo({r1, r1}, 0.f, false, true, at(r1, a0 + st * f32(k)));
        if (r0 > 0.f) {
            path.lineTo(at(r0, a1));
            for (u32 k = chunks; k-- > 0;) path.arcTo({r0, r0}, 0.f, false, false, at(r0, a0 + st * f32(k)));
        }
        path.close();
        return path;
    }
    static i32 hitWedge(const ChartState& s, const Rect& p, Vec2 pos) {
        Vec2 c;
        f32 r, total;
        pieGeo(p, c, r);
        const f32 dx = pos.x - c.x, dy = pos.y - c.y, d = std::sqrt(dx * dx + dy * dy);
        if (d > r + 6.f || (s.kind == ChartKind::Donut && d < r * 0.55f)) return -1;
        f32 a = std::atan2(dy, dx);
        if (a < -kPi * 0.5f) a += 2.f * kPi;
        for (const Wedge& w : wedges(s, total)) if (a >= w.a0 && a < w.a1) return i32(w.idx);
        return -1;
    }
    static std::vector<std::string> tooltip(const UiContext& ui, NodeHandle n, const ChartState& s) {
        std::vector<std::string> lines;
        const i32 hov = i32(ui.value(n));
        if (hov < 0) return lines;
        if (radial(s)) {
            f32 total;
            wedges(s, total);
            if (s.series.empty() || u32(hov) >= s.series[0].v.size() || total <= 0.f) return lines;
            const f32 v = s.series[0].v[u32(hov)];
            char pct[32];
            std::snprintf(pct, sizeof pct, " (%.1f%%)", double(v / total * 100.f));
            lines.push_back(entryName(s, u32(hov)) + ": " + fmtNum(s.axis[1].format, v) + pct);
            return lines;
        }
        const auto& cat = s.axis[0].categories;
        lines.push_back(u32(hov) < cat.size() ? cat[u32(hov)] : (s.axis[0].title.empty() ? "#" : s.axis[0].title + " ") + std::to_string(hov));
        for (const Series& se : s.series)
            if (!se.hidden && u32(hov) < se.v.size()) lines.push_back(se.name + ": " + fmtNum(s.axis[1].format, se.v[u32(hov)]));
        return lines;
    }
    static void paintLegend(const PaintContext& pc, DrawList& dl, const ChartState& s, f32 fs) {
        UiContext& ui = pc.ui;
        const auto items = legendItems(ui, pc.node, s);
        const Color text = pc.tint(pc.style.color);
        for (u32 i = 0; i < items.size(); ++i) {
            const Rect& it = items[i];
            const bool off = entryHidden(s, i);
            const Color col = pc.tint(entryColor(s, i));
            const f32 sy = it.y + (it.h - 10.f) * 0.5f;
            if (off) dl.roundedRect({it.x + 2.f, sy, 10.f, 10.f}, {}, {2.f, 2.f, 2.f, 2.f}, col, 1.f);
            else dl.rect({it.x + 2.f, sy, 10.f, 10.f}, col, 2.f);
            ui.paintString(dl, entryName(s, i), {it.x + 16.f, it.y, it.w - 16.f, it.h}, off ? text.withAlpha(text.a * 0.4f) : text, fs,
                           TextAlignment::Start, pc.node.index);
        }
    }
    static void paintTooltip(const PaintContext& pc, DrawList& dl, const ChartState& s, f32 fs) {
        UiContext& ui = pc.ui;
        if (!s.inside) return;
        const auto lines = tooltip(ui, pc.node, s);
        if (lines.empty()) return;
        const f32 lh = fs * 1.35f;
        f32 w = 0.f;
        for (const auto& l : lines) w = std::max(w, ui.measureString(l, fs));
        w += 16.f;
        const f32 h = lh * f32(lines.size()) + 8.f;
        const Rect c = pc.content;
        f32 x = s.pointer.x + 14.f, y = s.pointer.y + 14.f;
        if (x + w > c.right()) x = s.pointer.x - 14.f - w;
        if (y + h > c.bottom()) y = s.pointer.y - 14.f - h;
        x = std::max(c.x, x);
        y = std::max(c.y, y);
        dl.rect({x, y, w, h}, pc.tint(Color::rgb(0x1B1D24, 0.94f)), 4.f);
        dl.roundedRect({x, y, w, h}, {}, {4.f, 4.f, 4.f, 4.f}, pc.tint(Color::rgb(0xFFFFFF, 0.18f)), 1.f);
        for (u32 i = 0; i < lines.size(); ++i)
            ui.paintString(dl, lines[i], {x + 8.f, y + 4.f + f32(i) * lh, w - 16.f, lh}, pc.tint(Color::rgb(0xF2F3F7)), fs,
                           TextAlignment::Start, pc.node.index);
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        ChartState* s = chartPool().get(ui, pc.node);
        if (!s) return;
        const Rect p = plot(ui, pc.node, *s), c = pc.content;
        const Color text = pc.tint(pc.style.color), dim = pc.tint(pc.style.color.withAlpha(pc.style.color.a * 0.55f));
        const f32 fs = pc.style.fontSize * 0.8f, lh = lineH(ui, pc.node);
        const i32 hov = i32(ui.value(pc.node));
        if (radial(*s)) {
            Vec2 ctr;
            f32 r, total;
            pieGeo(p, ctr, r);
            const f32 r0 = s->kind == ChartKind::Donut ? r * 0.55f : 0.f;
            for (const Wedge& wd : wedges(*s, total)) {
                Vec2 o = ctr;
                if (i32(wd.idx) == hov) { // explode the hovered slice
                    const f32 m = (wd.a0 + wd.a1) * 0.5f;
                    o = {ctr.x + std::cos(m) * 6.f, ctr.y + std::sin(m) * 6.f};
                }
                dl.path(wedgePath(o, r0, r, wd.a0, wd.a1), pc.tint(sliceColor(*s, wd.idx)), false, pc.tint(pc.style.background.a > 0.f ? pc.style.background : Color::rgb(0x15171C)), 1.5f);
            }
            if (s->kind == ChartKind::Donut && !s->axis[1].title.empty())
                ui.paintString(dl, s->axis[1].title, {ctr.x - r0, ctr.y - lh * 0.5f, 2.f * r0, lh}, text, fs, TextAlignment::Center, pc.node.index);
            paintLegend(pc, dl, *s, fs);
            paintTooltip(pc, dl, *s, fs);
            return;
        }
        const ChartAxis& ax = s->axis[0];
        const ChartAxis& ay = s->axis[1];
        f32 lo, hi, step;
        range(*s, lo, hi, step);
        f32 va, vb;
        view(*s, va, vb);
        auto yOf = [&](f32 v) { return p.bottom() - (v - lo) / (hi - lo) * p.h; };
        // y grid + labels
        const u32 nt = std::min(64u, u32(std::lround((hi - lo) / step)));
        for (u32 i = 0; i <= nt; ++i) {
            const f32 v = lo + step * f32(i), y = std::round(yOf(v));
            if (ay.grid || i == 0) dl.rect({p.x, y, p.w, 1.f}, pc.tint(Color::rgb(0xFFFFFF, i == 0 ? 0.25f : 0.07f)));
            if (ay.visible) ui.paintString(dl, fmtNum(ay.format, v), {p.x - 40.f, y - 8.f, 36.f, 16.f}, dim, fs, TextAlignment::End, pc.node.index);
        }
        if (!ay.title.empty()) ui.paintString(dl, ay.title, {c.x, p.y - lh - 2.f, p.right() - c.x, lh}, text, fs, TextAlignment::Start, pc.node.index);
        // x labels (skip to keep them apart) + title
        const u32 n = points(*s);
        if (ax.visible && n) {
            auto label = [&](u32 i) { return i < ax.categories.size() ? ax.categories[i] : fmtNum(ax.format, double(i)); };
            f32 maxW = 8.f;
            const u32 i0 = u32(std::max(0.f, std::ceil(va))), i1 = u32(std::clamp(std::floor(vb), 0.f, f32(n - 1)));
            for (u32 i = i0; i <= i1; ++i) maxW = std::max(maxW, ui.measureString(label(i), fs));
            const f32 px = p.w / std::max(1e-3f, vb - va);
            const u32 every = std::max(1u, u32(std::ceil((maxW + 8.f) / std::max(1e-3f, px))));
            for (u32 i = i0; i <= i1; i += every) {
                const f32 x = xOf(p, f32(i), va, vb);
                if (ax.grid) dl.rect({std::round(x), p.y, 1.f, p.h}, pc.tint(Color::rgb(0xFFFFFF, 0.05f)));
                ui.paintString(dl, label(i), {x - maxW * 0.5f - 4.f, p.bottom() + 2.f, maxW + 8.f, lh}, dim, fs, TextAlignment::Center, pc.node.index);
            }
        }
        if (!ax.title.empty())
            ui.paintString(dl, ax.title, {p.x, p.bottom() + (ax.visible ? lh + 2.f : 2.f), p.w, lh}, text, fs, TextAlignment::Center, pc.node.index);
        const bool bars = s->kind == ChartKind::Bar;
        u32 nvis = 0;
        for (const Series& se : s->series) nvis += se.hidden ? 0u : 1u;
        dl.pushClip({p.x, p.y - 4.f, p.w, p.h + 5.f});
        const f32 slot = p.w / std::max(1e-3f, vb - va);
        u32 vk = 0;
        for (const Series& se : s->series) {
            if (se.hidden) continue;
            const Color col = pc.tint(se.color);
            if (s->kind == ChartKind::Area && se.v.size() > 1) {
                Path area;
                area.moveTo({xOf(p, 0.f, va, vb), yOf(0.f)});
                for (u32 i = 0; i < se.v.size(); ++i) area.lineTo({xOf(p, f32(i), va, vb), yOf(se.v[i])});
                area.lineTo({xOf(p, f32(se.v.size() - 1), va, vb), yOf(0.f)}).close();
                dl.path(area, col.withAlpha(col.a * 0.35f));
            }
            for (u32 i = 0; i < se.v.size(); ++i) {
                const f32 x = xOf(p, f32(i), va, vb), y = yOf(se.v[i]);
                switch (s->kind) {
                case ChartKind::Bar: {
                    const f32 bw = slot * 0.8f / f32(std::max(1u, nvis));
                    const f32 bx = x - slot * 0.4f + f32(vk) * bw, y0 = yOf(std::clamp(0.f, lo, hi));
                    Color bc = i32(i) == hov ? col.withAlpha(std::min(1.f, col.a * 1.f)) : col.withAlpha(col.a * 0.85f);
                    dl.rect({bx + 1.f, std::min(y, y0), std::max(1.f, bw - 2.f), std::max(1.f, std::abs(y0 - y))}, bc, 2.f);
                    break;
                }
                case ChartKind::Scatter: {
                    const f32 rr = i32(i) == hov ? 4.5f : 3.f;
                    dl.rect({x - rr, y - rr, 2.f * rr, 2.f * rr}, col, rr);
                    break;
                }
                default:
                    if (i + 1 < se.v.size()) dl.line({x, y}, {xOf(p, f32(i + 1), va, vb), yOf(se.v[i + 1])}, col, 2.f);
                    if (i32(i) == hov) dl.rect({x - 3.5f, y - 3.5f, 7.f, 7.f}, col, 3.5f);
                    break;
                }
            }
            ++vk;
        }
        dl.popClip();
        if (hov >= 0 && u32(hov) < n && s->inside) {
            const f32 x = std::round(xOf(p, f32(hov), va, vb));
            if (x >= p.x && x <= p.right()) dl.rect({x, p.y, 1.f, p.h}, pc.tint(Color::rgb(0xFFFFFF, 0.3f)));
        }
        paintLegend(pc, dl, *s, fs);
        paintTooltip(pc, dl, *s, fs);
    }
    static void setView(UiContext& ui, NodeHandle node, ChartState& s, f32 a, f32 b) {
        f32 fa, fb;
        fullView(s, fa, fb);
        const f32 full = fb - fa, minSpan = std::min(full, s.kind == ChartKind::Bar ? 1.f : 2.f);
        f32 span = std::clamp(b - a, minSpan, full);
        if (span >= full - 1e-4f) { s.vx0 = s.vx1 = std::numeric_limits<f32>::quiet_NaN(); ui.invalidate(node); return; }
        a = std::clamp(a, fa, fb - span);
        s.vx0 = a;
        s.vx1 = a + span;
        ui.invalidate(node);
    }
    static i32 hitIndex(const UiContext& ui, NodeHandle node, const ChartState& s, Vec2 pos) {
        const Rect p = plot(ui, node, s);
        if (radial(s)) return hitWedge(s, p, pos);
        const u32 n = points(s);
        if (!n || !p.contains(pos)) return -1;
        f32 a, b;
        view(s, a, b);
        const f32 t = a + (pos.x - p.x) / p.w * (b - a);
        return std::clamp(i32(std::lround(t)), 0, i32(n) - 1);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        ChartState* s = chartPool().get(ui, ec.node);
        if (!s) return false;
        auto hover = [&](Vec2 pos) {
            s->pointer = pos;
            s->inside = true;
            const i32 idx = hitIndex(ui, ec.node, *s, pos);
            if (f32(idx) != ui.value(ec.node)) ui.setValue(ec.node, f32(idx), true);
            ui.invalidate(ec.node);
        };
        switch (e.type) {
        case EventType::PointerMove:
            if (ui.pressed() == ec.node && !radial(*s) && !std::isnan(s->vx0)) { // drag pans a zoomed view
                const Rect p = plot(ui, ec.node, *s);
                const f32 d = (e.pos.x - s->dragX) / p.w * (s->dragV1 - s->dragV0);
                setView(ui, ec.node, *s, s->dragV0 - d, s->dragV1 - d);
            }
            hover(e.pos);
            return false;
        case EventType::PointerLeave:
            s->inside = false;
            if (ui.value(ec.node) != -1.f) ui.setValue(ec.node, -1.f, true);
            ui.invalidate(ec.node);
            return false;
        case EventType::PointerDown: {
            const auto items = legendItems(ui, ec.node, *s);
            for (u32 i = 0; i < items.size(); ++i)
                if (items[i].contains(e.pos)) { // legend click toggles the entry
                    if (radial(*s)) {
                        if (s->sliceHidden.size() <= i) s->sliceHidden.resize(i + 1, false);
                        s->sliceHidden[i] = !s->sliceHidden[i];
                    } else s->series[i].hidden = !s->series[i].hidden;
                    ui.emit(ec.node, SignalKind::Toggled, SignalArgs{f32(i), !entryHidden(*s, i), {}, &e});
                    ui.invalidate(ec.node);
                    return true;
                }
            f32 a, b;
            view(*s, a, b);
            s->dragX = e.pos.x; s->dragV0 = a; s->dragV1 = b;
            return false;
        }
        case EventType::Wheel: { // zoom the x axis around the pointer
            if (radial(*s) || e.wheel.y == 0.f) return false;
            const Rect p = plot(ui, ec.node, *s);
            if (!p.contains(e.pos)) return false;
            f32 a, b;
            view(*s, a, b);
            const f32 f = std::pow(0.8f, e.wheel.y), at = a + (e.pos.x - p.x) / p.w * (b - a);
            setView(ui, ec.node, *s, at - (at - a) * f, at + (b - at) * f);
            hover(e.pos);
            return true;
        }
        case EventType::KeyDown:
            if (e.key == Key::Home || e.key == Key::Escape) {
                if (std::isnan(s->vx0)) return false;
                setView(ui, ec.node, *s, -1e9f, 1e9f);
                return true;
            }
            if (!radial(*s) && (e.key == Key::Left || e.key == Key::Right) && !std::isnan(s->vx0)) {
                const f32 d = (s->vx1 - s->vx0) * 0.25f * (e.key == Key::Left ? -1.f : 1.f);
                setView(ui, ec.node, *s, s->vx0 + d, s->vx1 + d);
                return true;
            }
            return false;
        default: return false;
        }
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

} // namespace

void registerTierCD(UiContext& ui) {
    WidgetRegistry& r = ui.registry();
    if (r.find("gridview") != 0xFFFFFFFFu) return;
    r.registerWidget<GridView>();
    r.registerWidget<ColorPickerW>();
    r.registerWidget<CalendarW>();
    r.registerWidget<ChartW>();
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
SortFilterProxy::SortFilterProxy(const ItemModel* source) : m_src(source) {
    m_model.user = this;
    m_model.columns = source ? source->columns : 1;
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
}
u32 SortFilterProxy::rows() const {
    if (!m_src || !m_src->rows) return 0;
    return m_identity ? m_src->rows(m_src->user) : u32(m_map.size());
}
void SortFilterProxy::setFilter(u32 col, std::string_view needle) {
    m_filterCol = col;
    m_needle.assign(needle);
    for (char& c : m_needle) c = char(std::tolower(static_cast<unsigned char>(c)));
    invalidate();
}
void SortFilterProxy::setRowFilter(RowFilter fn, const void* user) {
    m_rowFilter = fn;
    m_rowFilterUser = user;
    invalidate();
}
void SortFilterProxy::setLessThan(LessThan fn, const void* user) {
    m_less = fn;
    m_lessUser = user;
    invalidate();
}
i64 SortFilterProxy::proxyRow(u32 sourceRow) const {
    if (m_identity) return sourceRow < rows() ? i64(sourceRow) : -1;
    if (!m_inverseValid) {
        m_inverse.assign(m_src && m_src->rows ? m_src->rows(m_src->user) : 0u, -1);
        for (std::size_t i = 0; i < m_map.size(); ++i) if (m_map[i] < m_inverse.size()) m_inverse[m_map[i]] = i64(i);
        m_inverseValid = true;
    }
    return sourceRow < m_inverse.size() ? m_inverse[sourceRow] : -1;
}
void SortFilterProxy::sortBy(i32 col, bool ascending) {
    m_sortCol = col;
    m_asc = ascending;
    invalidate();
}
void SortFilterProxy::invalidate() {
    m_identity = m_needle.empty() && m_sortCol < 0 && !m_rowFilter;
    m_map.clear();
    m_inverse.clear();
    m_inverseValid = false;
    if (m_identity || !m_src || !m_src->rows) { m_map.shrink_to_fit(); return; }
    const u32 n = m_src->rows(m_src->user);
    std::string scratch, low;
    m_map.reserve(m_needle.empty() ? n : n / 4);
    for (u32 r = 0; r < n; ++r) {
        if (!m_needle.empty()) {
            bool hit = false;
            const u32 c0 = m_filterCol == kAllColumns ? 0u : m_filterCol, c1 = m_filterCol == kAllColumns ? m_src->columns : m_filterCol + 1u;
            for (u32 col = c0; col < c1 && !hit; ++col) {
                low.assign(m_src->data(m_src->user, r, col, scratch));
                for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
                hit = low.find(m_needle) != std::string::npos;
            }
            if (!hit) continue;
        }
        if (m_rowFilter && !m_rowFilter(m_rowFilterUser, *m_src, r)) continue;
        m_map.push_back(r);
    }
    if (m_sortCol >= 0 && u32(m_sortCol) < m_src->columns) {
        // sort keys extracted once: numeric when every key parses, else strings
        const u32 col = u32(m_sortCol);
        std::vector<double> num(m_map.size());
        bool numeric = true;
        if (m_less) numeric = false;
        for (std::size_t i = 0; i < m_map.size() && numeric; ++i) numeric = parseNum(m_src->data(m_src->user, m_map[i], col, scratch), num[i]);
        std::vector<u32> idx(m_map.size());
        std::iota(idx.begin(), idx.end(), 0u);
        if (numeric) {
            std::stable_sort(idx.begin(), idx.end(), [&](u32 a, u32 b) { return m_asc ? num[a] < num[b] : num[b] < num[a]; });
        } else {
            std::vector<std::string> keys(m_map.size());
            for (std::size_t i = 0; i < m_map.size(); ++i) keys[i].assign(m_src->data(m_src->user, m_map[i], col, scratch));
            const auto less = [&](u32 a, u32 b) { return m_less ? m_less(m_lessUser, keys[a], keys[b]) : keys[a] < keys[b]; };
            std::stable_sort(idx.begin(), idx.end(), [&](u32 a, u32 b) { return m_asc ? less(a, b) : less(b, a); });
        }
        std::vector<u32> sorted(m_map.size());
        for (std::size_t i = 0; i < idx.size(); ++i) sorted[i] = m_map[idx[i]];
        m_map = std::move(sorted);
    }
}

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
void setCalendarRange(UiContext& ui, NodeHandle cal, int minYear, int minMonth, int minDay, int maxYear, int maxMonth, int maxDay) {
    if (!ui.alive(cal)) return;
    const i32 lo = daysFromCivil(minYear, minMonth, minDay), hi = std::max(lo, daysFromCivil(maxYear, maxMonth, maxDay));
    ui.setRange(cal, f32(lo), f32(hi), 1.f);
    CalendarW::select(ui, cal, i32(ui.value(cal)));
    ui.invalidate(cal);
}
void setCalendarOptions(UiContext& ui, NodeHandle cal, const CalendarOptions& o) {
    if (!ui.alive(cal)) return;
    CalState& st = calStores()[&ui][cal.index];
    st.gen = cal.generation;
    st.o = o;
    st.o.firstDayOfWeek = std::min<u8>(o.firstDayOfWeek, 6);
    ui.markAllDirty(); // week-number column changes the size hint
}
void calendarShownPage(const UiContext& ui, NodeHandle cal, int& year, int& month) {
    const i32 ym = CalendarW::shownMonth(ui, cal);
    year = ym / 12;
    month = ym % 12 + 1;
}
void setCalendarShownPage(UiContext& ui, NodeHandle cal, int year, int month) {
    if (ui.alive(cal)) CalendarW::setShown(ui, cal, year * 12 + std::clamp(month, 1, 12) - 1);
}
int calendarIsoWeek(int year, int month, int day) { return isoWeek(daysFromCivil(year, month, day)); }
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
void chartSetAxis(UiContext& ui, NodeHandle c, ChartAxisId axis, const ChartAxis& a) {
    if (ChartState* s = chartPool().get(ui, c)) { s->axis[axis == ChartAxisId::X ? 0 : 1] = a; ui.invalidate(c); }
}
ChartAxis chartAxis(const UiContext& ui, NodeHandle c, ChartAxisId axis) {
    const ChartState* s = chartPool().get(ui, c);
    return s ? s->axis[axis == ChartAxisId::X ? 0 : 1] : ChartAxis{};
}
void chartSetLegend(UiContext& ui, NodeHandle c, ChartLegend where) {
    if (ChartState* s = chartPool().get(ui, c)) { s->legend = where; ui.invalidate(c); }
}
void chartSetEntryVisible(UiContext& ui, NodeHandle c, u32 index, bool visible) {
    ChartState* s = chartPool().get(ui, c);
    if (!s) return;
    if (ChartW::radial(*s)) {
        if (s->sliceHidden.size() <= index) s->sliceHidden.resize(index + 1, false);
        s->sliceHidden[index] = !visible;
    } else if (index < s->series.size()) s->series[index].hidden = !visible;
    ui.invalidate(c);
}
bool chartEntryVisible(const UiContext& ui, NodeHandle c, u32 index) {
    const ChartState* s = chartPool().get(ui, c);
    if (!s) return false;
    if (!ChartW::radial(*s) && index >= s->series.size()) return false;
    return !ChartW::entryHidden(*s, index);
}
void chartSetSliceColors(UiContext& ui, NodeHandle c, std::span<const Color> colors) {
    if (ChartState* s = chartPool().get(ui, c)) { s->sliceColors.assign(colors.begin(), colors.end()); ui.invalidate(c); }
}
void chartSetXView(UiContext& ui, NodeHandle c, f32 from, f32 to) {
    if (ChartState* s = chartPool().get(ui, c)) ChartW::setView(ui, c, *s, std::min(from, to), std::max(from, to));
}
void chartXView(const UiContext& ui, NodeHandle c, f32& from, f32& to) {
    from = to = 0.f;
    if (const ChartState* s = chartPool().get(ui, c)) ChartW::view(*s, from, to);
}
void chartResetZoom(UiContext& ui, NodeHandle c) { chartSetXView(ui, c, -1e9f, 1e9f); }
std::vector<std::string> chartTooltip(const UiContext& ui, NodeHandle c) {
    const ChartState* s = chartPool().get(ui, c);
    return s && s->inside ? ChartW::tooltip(ui, c, *s) : std::vector<std::string>{};
}
Rect chartPlotRect(const UiContext& ui, NodeHandle c) {
    const ChartState* s = chartPool().get(ui, c);
    return s ? ChartW::plot(ui, c, *s) : Rect{};
}
std::vector<Rect> chartLegendRects(const UiContext& ui, NodeHandle c) {
    const ChartState* s = chartPool().get(ui, c);
    return s ? ChartW::legendItems(ui, c, *s) : std::vector<Rect>{};
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
    auto emit = [&](std::string text, std::string_view cls) {
        if (cls != "md-code") stripInline(text);
        const NodeHandle l = label(ui, view, text);
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
