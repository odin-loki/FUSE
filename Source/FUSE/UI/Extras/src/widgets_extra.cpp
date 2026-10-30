// Qt-parity extra widgets (see widgets_extra.hpp).
#include <map>
#include "fuse/ui/extras/widgets_extra.hpp"

#include "fuse/text/text.hpp"
#include "fuse/ui/os.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>

namespace fuse::ui::widgets {
namespace {

NodeHandle make(UiContext& ui, std::string_view cls, NodeHandle parent, std::string_view id = {}) {
    auto h = ui.create(cls, parent, id);
    return h ? *h : NodeHandle{};
}
void px(UiContext& ui, NodeHandle n, Prop p, f32 v) { ui.set(n, p, PropValue::ofLength(Length::px(v))); }
NodeHandle nth(const UiContext& ui, NodeHandle n, u32 i) {
    NodeHandle c = ui.firstChild(n);
    while (i-- && ui.alive(c)) c = ui.nextSibling(c);
    return ui.alive(c) ? c : NodeHandle{};
}
u32 childCount(const UiContext& ui, NodeHandle n) {
    u32 k = 0;
    for (NodeHandle c = ui.firstChild(n); ui.alive(c); c = ui.nextSibling(c)) ++k;
    return k;
}
std::vector<std::string_view> split(std::string_view s) {
    std::vector<std::string_view> v;
    if (s.empty()) return v;
    std::size_t a = 0;
    for (;;) {
        auto b = s.find('\n', a);
        v.push_back(s.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a));
        if (b == std::string_view::npos) break;
        a = b + 1;
    }
    return v;
}
f32 lineH(const UiContext& ui, NodeHandle n) {
    const f32 p = ui.computed(n).fontSize;
    return std::ceil(ui.font() ? ui.font()->metrics(p).lineHeight() : p * 1.25f);
}

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

void showOnly(UiContext& ui, NodeHandle stack, u32 index) {
    u32 i = 0;
    for (NodeHandle c = ui.firstChild(stack); ui.alive(c); c = ui.nextSibling(c), ++i) ui.setCss(c, "display", i == index ? "flex" : "none");
}

// ---- LCD ----------------------------------------------------------------------------------------------------------------
struct LcdState { u32 digits = 6, base = 10; std::string text; bool explicitText = false; };
Pool<LcdState>& lcdPool() { static Pool<LcdState> p; return p; }
std::string lcdString(const UiContext& ui, NodeHandle n, const LcdState& s) {
    if (s.explicitText) return s.text;
    const double v = ui.value(n);
    char b[64];
    if (s.base == 10) {
        std::snprintf(b, sizeof b, "%.*g", int(std::max(1u, s.digits)), v);
        std::string r = b;
        if (r.find('e') != std::string::npos) r = "E";
        return r;
    }
    long long iv = std::llround(v);
    std::string r;
    const bool neg = iv < 0;
    unsigned long long u = neg ? 0ull - (unsigned long long)iv : (unsigned long long)iv;
    do { const int d = int(u % s.base); r.insert(r.begin(), char(d < 10 ? '0' + d : 'a' + d - 10)); u /= s.base; } while (u);
    return neg ? "-" + r : r;
}
struct LcdW {
    static constexpr std::string_view kName = "lcdnumber";
    static constexpr A11yRole kRole = A11yRole::Text;
    static Size measure(UiContext&, NodeHandle n, Size) { (void)n; return {150.f, 44.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        LcdState* s = lcdPool().get(pc.ui, pc.node);
        if (!s) return;
        std::string t = lcdString(pc.ui, pc.node, *s);
        // count cells: '.' and ':' attach to the previous / own narrow cell
        const u32 cells = std::max<u32>(s->digits, u32(std::count_if(t.begin(), t.end(), [](char c) { return c != '.'; })));
        const Rect c = pc.content;
        const f32 cw = std::min(c.w / f32(std::max(1u, cells)), c.h * 0.75f), h = c.h, w = cw * 0.72f, th = std::max(1.5f, h * 0.09f);
        const Color on = pc.tint(pc.style.color), off = pc.tint(pc.style.color.withAlpha(pc.style.color.a * 0.08f));
        // right-aligned
        u32 used = u32(std::count_if(t.begin(), t.end(), [](char ch) { return ch != '.'; }));
        f32 x = c.right() - f32(std::min(cells, used)) * cw;
        for (std::size_t i = 0; i < t.size(); ++i) {
            const char ch = t[i];
            if (ch == '.') continue;
            const u8 m = sevenSegment(ch);
            const f32 ox = x + (cw - w) * 0.5f, oy = c.y, hh = h * 0.5f;
            const Rect seg[7] = {
                {ox + th, oy, w - 2 * th, th},                // a
                {ox + w - th, oy + th, th, hh - 1.5f * th},   // b
                {ox + w - th, oy + hh + th * 0.5f, th, hh - 1.5f * th}, // c
                {ox + th, oy + h - th, w - 2 * th, th},       // d
                {ox, oy + hh + th * 0.5f, th, hh - 1.5f * th},// e
                {ox, oy + th, th, hh - 1.5f * th},            // f
                {ox + th, oy + hh - th * 0.5f, w - 2 * th, th}, // g
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

// ---- key sequence edit ------------------------------------------------------------------------------------------------
struct KeySeqW {
    static constexpr std::string_view kName = "keysequenceedit";
    static constexpr A11yRole kRole = A11yRole::TextField;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) { return {140.f, lineH(ui, n)}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const std::string_view t = pc.ui.text(pc.node);
        const bool focus = pc.ui.focused() == pc.node;
        const Color col = pc.tint(pc.style.color.withAlpha(pc.style.color.a * (t.empty() ? 0.5f : 1.f)));
        pc.ui.paintString(dl, t.empty() ? (focus ? "Press shortcut…" : "None") : t, pc.content, col, pc.style.fontSize, TextAlignment::Start, pc.node.index);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        if (e.type == EventType::PointerDown) { ec.ui.setFocus(ec.node, FocusReason::Pointer); return true; }
        if (e.type != EventType::KeyDown) return false;
        if (e.key == Key::Tab && !e.mods) return false;
        if (e.key == Key::Escape || (e.key == Key::Backspace && !e.mods)) {
            ec.ui.setText(ec.node, "");
            ec.ui.emit(ec.node, SignalKind::TextChanged, {.text = ""});
        } else {
            const std::string s = keySequenceText(e.key, e.mods);
            ec.ui.setText(ec.node, s);
            ec.ui.emit(ec.node, SignalKind::TextChanged, {.text = ec.ui.text(ec.node)});
            ec.ui.emit(ec.node, SignalKind::Submitted, {.text = ec.ui.text(ec.node)});
        }
        ec.ui.invalidate(ec.node);
        return true;
    }
};

// ---- MDI ----------------------------------------------------------------------------------------------------------------
struct MdiState { Rect geo; Vec2 grab; Rect grabGeo; u8 drag = 0; /*1 move 2 resize*/ };
Pool<MdiState>& mdiPool() { static Pool<MdiState> p; return p; }
void applyGeo(UiContext& ui, NodeHandle sub, const Rect& r) {
    px(ui, sub, Prop::Left, r.x);
    px(ui, sub, Prop::Top, r.y);
    px(ui, sub, Prop::Width, r.w);
    px(ui, sub, Prop::Height, r.h);
}
void raise(UiContext& ui, NodeHandle sub) {
    const NodeHandle area = ui.parent(sub);
    NodeHandle last;
    for (NodeHandle c = ui.firstChild(area); ui.alive(c); c = ui.nextSibling(c)) last = c;
    if (ui.alive(area) && last != sub) ui.moveTo(sub, area);
    for (NodeHandle c = ui.firstChild(area); ui.alive(c); c = ui.nextSibling(c)) ui.removeClass(c, "active");
    ui.addClass(sub, "active");
}
/// Title bar / resize grip of a sub-window: the class carries the drag behaviour.
struct MdiTitleW {
    static constexpr std::string_view kName = "mdititle";
    static constexpr A11yRole kRole = A11yRole::Group;
    static constexpr bool kInteractive = true;
    static bool onEvent(EventContext& ec, const Event& e) {
        UiContext& ui = ec.ui;
        if (ec.phase == Phase::Capture) return false;
        const NodeHandle sub = ui.parent(ec.node);
        MdiState* s = mdiPool().get(ui, sub);
        if (!s) return false;
        if (e.type == EventType::PointerDown && ec.target == ec.node) {
            raise(ui, sub);
            s->drag = ui.hasClass(ec.node, "mdigrip") ? 2 : 1;
            s->grab = e.pos;
            s->grabGeo = s->geo;
            return true;
        }
        if (e.type == EventType::PointerMove && s->drag) {
            Rect r = s->grabGeo;
            const f32 dx = e.pos.x - s->grab.x, dy = e.pos.y - s->grab.y;
            if (s->drag == 1) { r.x += dx; r.y += dy; }
            else { r.w = std::max(80.f, r.w + dx); r.h = std::max(48.f, r.h + dy); }
            s->geo = r;
            applyGeo(ui, sub, r);
            return true;
        }
        if (e.type == EventType::PointerUp) { s->drag = 0; return ec.target == ec.node; }
        return false;
    }
};
struct MdiSubW {
    static constexpr std::string_view kName = "mdisubwindow";
    static constexpr A11yRole kRole = A11yRole::Window;
    static constexpr bool kInteractive = true;
    static bool onEvent(EventContext& ec, const Event& e) {
        if (e.type == EventType::PointerDown) raise(ec.ui, ec.node);
        return false;
    }
};

// ---- graphics view -----------------------------------------------------------------------------------------------------
struct ViewState {
    GraphicsScene* scene = nullptr;
    f32 zoom = 1.f;
    Vec2 pan;
    u8 mode = 0; // 1 move items, 2 pan, 3 rubber band
    Vec2 last, bandA, bandB;
    u32 pressItem = ~0u;
    double lastClick = -1;
    u32 lastClickItem = ~0u;
};
Pool<ViewState>& viewPool() { static Pool<ViewState> p; return p; }
struct GraphicsViewW {
    static constexpr std::string_view kName = "graphicsview";
    static constexpr A11yRole kRole = A11yRole::Image;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {400.f, 300.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        ViewState* v = viewPool().get(pc.ui, pc.node);
        if (!v || !v->scene) return;
        const Rect c = pc.content;
        const f32 z = v->zoom;
        auto T = [&](Vec2 s) { return Vec2{c.x + (s.x - v->pan.x) * z, c.y + (s.y - v->pan.y) * z}; };
        auto TR = [&](const Rect& r) { const Vec2 a = T({r.x, r.y}); return Rect{a.x, a.y, r.w * z, r.h * z}; };
        dl.pushClip(c);
        const Rect visible{v->pan.x, v->pan.y, c.w / z, c.h / z};
        for (u32 id : v->scene->paintOrder()) {
            const GraphicsItem& it = *v->scene->item(id);
            if (!it.visible) continue;
            const Rect bb = it.shape == GraphicsShape::Line
                                ? Rect{std::min(it.rect.x, it.rect.right()), std::min(it.rect.y, it.rect.bottom()), std::abs(it.rect.w) + 1, std::abs(it.rect.h) + 1}
                                : it.rect;
            if (bb.intersect(visible).empty() && it.shape != GraphicsShape::Polygon) continue;
            const Rect r = TR(it.rect);
            const Color fill = pc.tint(it.fill), stroke = pc.tint(it.stroke);
            const f32 sw = std::max(1.f, it.strokeWidth * z);
            switch (it.shape) {
            case GraphicsShape::Rect:
                dl.roundedRect(r, fill, {0, 0, 0, 0}, stroke, it.strokeWidth > 0 ? sw : 0.f);
                break;
            case GraphicsShape::Ellipse: {
                const f32 rad = std::min(r.w, r.h) * 0.5f;
                if (std::abs(r.w - r.h) < 0.5f) dl.roundedRect(r, fill, {rad, rad, rad, rad}, stroke, it.strokeWidth > 0 ? sw : 0.f);
                else { // horizontal spans approximate a true ellipse
                    const int rows = std::max(2, int(r.h / 2.f));
                    for (int i = 0; i < rows; ++i) {
                        const f32 yy = (f32(i) + 0.5f) / f32(rows) * 2.f - 1.f, hw = r.w * 0.5f * std::sqrt(std::max(0.f, 1.f - yy * yy));
                        dl.rect({r.x + r.w * 0.5f - hw, r.y + f32(i) * r.h / f32(rows), hw * 2.f, r.h / f32(rows) + 0.5f}, fill);
                    }
                }
                break;
            }
            case GraphicsShape::Line: dl.line(T({it.rect.x, it.rect.y}), T({it.rect.right(), it.rect.bottom()}), stroke, sw); break;
            case GraphicsShape::Text:
                pc.ui.paintString(dl, it.text, r, fill, std::max(4.f, pc.style.fontSize * z), TextAlignment::Start, pc.node.index);
                break;
            case GraphicsShape::Polygon:
                for (std::size_t i = 0; i < it.points.size(); ++i) dl.line(T(it.points[i]), T(it.points[(i + 1) % it.points.size()]), stroke, sw);
                break;
            }
            if (it.selected) {
                const Rect b = TR(bb);
                dl.roundedRect({b.x - 3, b.y - 3, b.w + 6, b.h + 6}, {}, {0, 0, 0, 0}, pc.tint(Color::rgb(0xFFD54F)), 1.5f);
            }
        }
        if (v->mode == 3) {
            const Vec2 a = v->bandA, b = v->bandB;
            const Rect band{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(a.x - b.x), std::abs(a.y - b.y)};
            dl.roundedRect(band, pc.tint(Color::rgb(0x3D8BFD, 0.2f)), {0, 0, 0, 0}, pc.tint(Color::rgb(0x3D8BFD)), 1.f);
        }
        dl.popClip();
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
            // keep the scene point under the pointer fixed
            const Rect c = ui.contentRect(ec.node);
            v->pan = {sp.x - (e.pos.x - c.x) / nz, sp.y - (e.pos.y - c.y) / nz};
            v->zoom = nz;
            ui.invalidate(ec.node);
            return true;
        }
        case EventType::PointerDown: {
            ui.setFocus(ec.node, FocusReason::Pointer);
            v->last = e.pos;
            if (e.button == MouseButton::Middle || (e.mods & kModAlt)) { v->mode = 2; return true; }
            const u32 hit = sc.itemAt(sp);
            v->pressItem = hit;
            if (hit != ~0u) {
                const bool toggle = e.mods & kModCtrl;
                const GraphicsItem* it = sc.item(hit);
                if (it->selectable) {
                    if (toggle) sc.select(hit, !it->selected);
                    else if (!it->selected) sc.select(hit, true, true);
                    ui.emit(ec.node, SignalKind::ValueChanged, {.value = f32(hit)});
                }
                const double now = os::monotonicSeconds();
                if (now - v->lastClick < 0.4 && v->lastClickItem == hit) ui.emit(ec.node, SignalKind::Submitted, {.value = f32(hit)});
                v->lastClick = now;
                v->lastClickItem = hit;
                v->mode = 1;
            } else {
                if (!(e.mods & kModCtrl)) { sc.clearSelection(); ui.emit(ec.node, SignalKind::ValueChanged, {.value = -1.f}); }
                v->mode = 3;
                v->bandA = v->bandB = e.pos;
            }
            ui.invalidate(ec.node);
            return true;
        }
        case EventType::PointerMove: {
            if (!v->mode) return false;
            const Vec2 d{(e.pos.x - v->last.x) / v->zoom, (e.pos.y - v->last.y) / v->zoom};
            v->last = e.pos;
            if (v->mode == 2) v->pan = {v->pan.x - d.x, v->pan.y - d.y};
            else if (v->mode == 1) { for (u32 id : sc.selection()) if (sc.item(id)->movable) sc.move(id, d); }
            else if (v->mode == 3) v->bandB = e.pos;
            ui.invalidate(ec.node);
            return true;
        }
        case EventType::PointerUp: {
            if (v->mode == 3) {
                const Vec2 a = viewToScene(ui, ec.node, v->bandA), b = viewToScene(ui, ec.node, v->bandB);
                const Rect r{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(a.x - b.x), std::abs(a.y - b.y)};
                if (r.w > 1 && r.h > 1)
                    for (u32 id : sc.itemsIn(r)) if (sc.item(id)->selectable) sc.select(id, true);
                ui.emit(ec.node, SignalKind::ValueChanged, {.value = f32(sc.selection().empty() ? -1 : i32(sc.selection().back()))});
            }
            v->mode = 0;
            ui.invalidate(ec.node);
            return true;
        }
        case EventType::KeyDown: {
            const f32 step = 10.f / v->zoom;
            Vec2 d{};
            switch (e.key) {
            case Key::Left: d.x = -step; break;
            case Key::Right: d.x = step; break;
            case Key::Up: d.y = -step; break;
            case Key::Down: d.y = step; break;
            case Key::Delete: for (u32 id : sc.selection()) sc.remove(id); ui.invalidate(ec.node); return true;
            default: return false;
            }
            for (u32 id : sc.selection()) if (sc.item(id)->movable) sc.move(id, d);
            ui.invalidate(ec.node);
            return true;
        }
        default: return false;
        }
    }
};

// ---- message box icon -------------------------------------------------------------------------------------------------
struct MsgIconW {
    static constexpr std::string_view kName = "msgicon";
    static constexpr A11yRole kRole = A11yRole::Image;
    static Size measure(UiContext&, NodeHandle, Size) { return {32.f, 32.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const auto icon = MessageIcon(u32(pc.ui.value(pc.node)));
        const Color col = icon == MessageIcon::Critical ? Color::rgb(0xE5484D) : icon == MessageIcon::Warning ? Color::rgb(0xF5A524)
                        : icon == MessageIcon::Question ? Color::rgb(0x8E4EC6) : Color::rgb(0x3D8BFD);
        const Rect r = pc.content;
        dl.rect(r, pc.tint(col), r.w * 0.5f);
        const char* g = icon == MessageIcon::Critical ? "×" : icon == MessageIcon::Warning ? "!" : icon == MessageIcon::Question ? "?" : "i";
        pc.ui.paintString(dl, g, r, pc.tint(Color::rgb(0xFFFFFF)), r.h * 0.7f, TextAlignment::Center, pc.node.index);
    }
};

struct WizardState { u32 cur = 0, count = 0; NodeHandle stack, back, next, finish, card, stepTitle; std::vector<std::string> titles; std::function<bool(u32)> canLeave; };
Pool<WizardState>& wizPool() { static Pool<WizardState> p; return p; }
// The wizard keeps its state on a hidden "wizardstate" node: the first child of the dialog body.
NodeHandle wizStateNode(const UiContext& ui, NodeHandle wiz) { return ui.firstChild(dialogBody(ui, wiz)); }
void wizUpdate(UiContext& ui, WizardState& s) {
    showOnly(ui, s.stack, s.cur);
    if (s.cur < s.titles.size()) ui.setText(s.stepTitle, s.titles[s.cur]);
    ui.setDisabled(s.back, s.cur == 0);
    ui.setCss(s.next, "display", s.cur + 1 < s.count ? "flex" : "none");
    ui.setCss(s.finish, "display", s.cur + 1 >= s.count ? "flex" : "none");
    setDialogDefaultButton(ui, s.card, s.cur + 1 < s.count ? s.next : s.finish); // QWizard: Next, then Finish, is default
    if (ui.focused() == s.next && s.cur + 1 >= s.count) ui.setFocus(s.finish, FocusReason::Keyboard);
    if (ui.focused() == s.finish && s.cur + 1 < s.count) ui.setFocus(s.next, FocusReason::Keyboard);
}

struct CompleterState { std::vector<std::string> entries; u32 max = 8; };
Pool<CompleterState>& compPool() { static Pool<CompleterState> p; return p; }

} // namespace

void registerExtraWidgets(UiContext& ui) {
    WidgetRegistry& r = ui.registry();
    if (r.find("lcdnumber") != 0xFFFFFFFFu) return;
    r.registerWidget<LcdW>();
    r.registerWidget<KeySeqW>();
    r.registerWidget<MdiTitleW>();
    r.registerWidget<MdiSubW>();
    r.registerWidget<GraphicsViewW>();
    r.registerWidget<MsgIconW>();
    ui.addStyleSheet(R"(
.groupbox { border-width: 1px; border-color: rgba(255,255,255,0.18); border-radius: 6px; padding: 8px; gap: 6px; }
.groupbox-title { font-size: 13px; opacity: 0.9; }
.toolbox-header { border-radius: 0px; }
.statusbar { flex-direction: row; padding: 2px 8px; gap: 12px; background: rgba(0,0,0,0.25); }
.statusbar-perm { flex-direction: row; gap: 8px; }
lcdnumber { color: #7CFC9A; background: #0B1A10; padding: 6px; border-radius: 4px; }
keysequenceedit { border-width: 1px; border-color: rgba(255,255,255,0.25); border-radius: 4px; padding: 4px 8px; }
.mdiarea { layout: absolute; background: rgba(0,0,0,0.3); overflow: hidden; }
mdisubwindow { background: #22262E; border-width: 1px; border-color: rgba(255,255,255,0.2); border-radius: 6px; shadow-color: rgba(0,0,0,0.5); shadow-blur: 12px; }
mdisubwindow.active { border-color: #3D8BFD; }
mdititle { flex-direction: row; padding: 4px 8px; background: rgba(255,255,255,0.06); align-items: center; }
.mdigrip { width: 12px; height: 12px; align-self: end; }
graphicsview { background: #15181D; border-width: 1px; border-color: rgba(255,255,255,0.15); }
.msgbox-row { flex-direction: row; gap: 12px; align-items: center; }
.dialog-buttons { flex-direction: row; gap: 8px; justify-content: end; }
)");
}

// ---- group box ----------------------------------------------------------------------------------------------------------
NodeHandle groupBox(UiContext& ui, NodeHandle parent, std::string_view title, bool checkable, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle box = panel(ui, parent, id);
    ui.addClass(box, "groupbox");
    ui.setA11y(box, A11yRole::Group, title);
    NodeHandle head;
    if (checkable) head = checkBox(ui, box, title, true);
    else head = label(ui, box, title);
    ui.addClass(head, "groupbox-title");
    const NodeHandle body = panel(ui, box);
    if (checkable)
        ui.connect(head, SignalKind::Toggled, [box, body](UiContext& c, NodeHandle, const SignalArgs& a) {
            c.setDisabled(body, !a.flag);
            c.setCss(body, "opacity", a.flag ? "1" : "0.45");
            c.emit(box, SignalKind::Toggled, {.flag = a.flag});
        });
    return box;
}
NodeHandle groupBoxBody(const UiContext& ui, NodeHandle box) { return nth(ui, box, 1); }

// ---- stacked widget -----------------------------------------------------------------------------------------------------
NodeHandle stackedWidget(UiContext& ui, NodeHandle parent, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle s = panel(ui, parent, id);
    ui.addClass(s, "stacked");
    ui.setRange(s, 0.f, 1e6f, 1.f);
    return s;
}
NodeHandle stackedAddPage(UiContext& ui, NodeHandle stack) {
    const NodeHandle p = panel(ui, stack);
    ui.setCss(p, "flex-grow", "1");
    showOnly(ui, stack, stackedIndex(ui, stack));
    return p;
}
void setStackedIndex(UiContext& ui, NodeHandle stack, u32 index) {
    const u32 n = childCount(ui, stack);
    if (!n) return;
    index = std::min(index, n - 1);
    const bool changed = u32(ui.value(stack)) != index;
    ui.setValue(stack, f32(index), changed);
    showOnly(ui, stack, index);
}
u32 stackedIndex(const UiContext& ui, NodeHandle stack) { return u32(std::max(0.f, ui.value(stack))); }
u32 stackedCount(const UiContext& ui, NodeHandle stack) { return childCount(ui, stack); }
NodeHandle stackedPage(const UiContext& ui, NodeHandle stack, u32 index) { return nth(ui, stack, index); }

// ---- tool box -----------------------------------------------------------------------------------------------------------
NodeHandle toolBox(UiContext& ui, NodeHandle parent, std::string_view titles, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle box = panel(ui, parent, id);
    ui.addClass(box, "toolbox");
    ui.setRange(box, 0.f, 1e6f, 1.f);
    u32 i = 0;
    for (std::string_view t : split(titles)) {
        const NodeHandle h = button(ui, box, t);
        ui.addClass(h, "toolbox-header");
        const NodeHandle p = panel(ui, box);
        ui.setA11y(p, A11yRole::Group, t);
        ui.setCss(p, "display", i == 0 ? "flex" : "none");
        ui.connect(h, SignalKind::Clicked, [box, i](UiContext& c, NodeHandle, const SignalArgs&) { setToolBoxIndex(c, box, i); });
        ++i;
    }
    return box;
}
NodeHandle toolBoxPage(const UiContext& ui, NodeHandle box, u32 index) { return nth(ui, box, index * 2 + 1); }
void setToolBoxIndex(UiContext& ui, NodeHandle box, u32 index) {
    const u32 pages = childCount(ui, box) / 2;
    if (index >= pages) return;
    for (u32 k = 0; k < pages; ++k) ui.setCss(toolBoxPage(ui, box, k), "display", k == index ? "flex" : "none");
    if (u32(ui.value(box)) != index) ui.setValue(box, f32(index), true);
}

// ---- status bar ---------------------------------------------------------------------------------------------------------
NodeHandle statusBar(UiContext& ui, NodeHandle parent, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle bar = panel(ui, parent, id);
    ui.addClass(bar, "statusbar");
    ui.setA11y(bar, A11yRole::Group, "Status");
    const NodeHandle msg = label(ui, bar, "");
    ui.setCss(msg, "flex-grow", "1");
    const NodeHandle perm = panel(ui, bar);
    ui.addClass(perm, "statusbar-perm");
    return bar;
}
void showStatusMessage(UiContext& ui, NodeHandle bar, std::string_view text, f32 seconds) {
    const NodeHandle msg = ui.firstChild(bar);
    if (!ui.alive(msg)) return;
    ui.setText(msg, text);
    const f32 gen = ui.value(bar) + 1.f; // generation: a newer message cancels the older timeout
    ui.setRange(bar, 0.f, 1e9f, 0.f);
    ui.setValue(bar, gen);
    if (seconds > 0.f)
        ui.startTimer(seconds, false, [bar, msg, gen](UiContext& c) { if (c.alive(msg) && c.value(bar) == gen) c.setText(msg, ""); });
}
std::string_view statusMessage(const UiContext& ui, NodeHandle bar) { return ui.text(ui.firstChild(bar)); }
NodeHandle statusBarPermanent(const UiContext& ui, NodeHandle bar) { return nth(ui, bar, 1); }

namespace {
ButtonRole messageButtonRole(std::string_view b) {
    std::string l(b);
    for (char& c : l) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (l == "ok" || l == "save" || l == "save all" || l == "open" || l == "retry" || l == "ignore") return ButtonRole::Accept;
    if (l == "yes" || l == "yes to all") return ButtonRole::Yes;
    if (l == "no" || l == "no to all") return ButtonRole::No;
    if (l == "cancel" || l == "close" || l == "abort") return ButtonRole::Reject;
    if (l == "discard" || l == "don't save" || l == "close without saving") return ButtonRole::Destructive;
    if (l == "help") return ButtonRole::Help;
    if (l == "apply") return ButtonRole::Apply;
    if (l == "reset" || l == "restore defaults") return ButtonRole::Reset;
    return ButtonRole::Action;
}
} // namespace
// ---- message box / input dialog -----------------------------------------------------------------------------------------
NodeHandle messageBox(UiContext& ui, std::string_view title, std::string_view text, std::string_view buttons, MessageIcon icon, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle dlg = dialog(ui, title, id);
    const NodeHandle body = dialogBody(ui, dlg);
    const NodeHandle row = panel(ui, body);
    ui.addClass(row, "msgbox-row");
    if (icon != MessageIcon::None) {
        const NodeHandle ic = make(ui, "msgicon", row);
        ui.setRange(ic, 0.f, 8.f, 1.f);
        ui.setValue(ic, f32(u32(icon)));
    }
    const NodeHandle l = label(ui, row, text);
    ui.addClass(l, "msgbox-text");
    // QMessageBox: a dialog button box; roles from the usual button names so the platform order and Enter / Escape apply
    const NodeHandle btns = dialogButtonBox(ui, body, 0);
    ui.addClass(btns, "dialog-buttons");
    u32 i = 0;
    NodeHandle last;
    for (std::string_view b : split(buttons)) {
        const NodeHandle bt = dialogButtonBoxAdd(ui, btns, b, messageButtonRole(b));
        last = bt;
        ui.connect(bt, SignalKind::Clicked, [dlg, i](UiContext& c, NodeHandle self, const SignalArgs&) {
            const std::string t(c.text(self));
            closeDialog(c, dlg);
            c.emit(dlg, SignalKind::Submitted, {.value = f32(i), .text = t});
        });
        ++i;
    }
    if (!dialogButtonBoxRejectButton(ui, btns).valid()) setDialogRejectButton(ui, dlg, last); // Escape = last button
    if (!dialogButtonBoxDefault(ui, btns).valid()) setDialogDefaultButton(ui, dlg, last);
    openDialog(ui, dlg);
    if (const NodeHandle d = dialogButtonBoxDefault(ui, btns); d.valid()) ui.setFocus(d, FocusReason::Keyboard);
    return dlg;
}
NodeHandle inputDialog(UiContext& ui, std::string_view title, std::string_view prompt, std::string_view initial, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle dlg = dialog(ui, title, id);
    const NodeHandle body = dialogBody(ui, dlg);
    label(ui, body, prompt);
    const NodeHandle le = lineEdit(ui, body, initial);
    const NodeHandle btns = dialogButtonBox(ui, body, StdButton::Ok | StdButton::Cancel);
    ui.addClass(btns, "dialog-buttons");
    const NodeHandle ok = dialogButtonBoxButton(ui, btns, StdButton::Ok), cancel = dialogButtonBoxButton(ui, btns, StdButton::Cancel);
    auto accept = [dlg, le](UiContext& c, NodeHandle, const SignalArgs&) {
        const std::string t = c.editBuffer(le).text();
        closeDialog(c, dlg);
        c.emit(dlg, SignalKind::Submitted, {.value = 1.f, .text = t});
    };
    ui.connect(ok, SignalKind::Clicked, accept);
    ui.connect(le, SignalKind::Submitted, accept);
    ui.connect(cancel, SignalKind::Clicked, [dlg](UiContext& c, NodeHandle, const SignalArgs&) {
        closeDialog(c, dlg);
        c.emit(dlg, SignalKind::Submitted, {.value = 0.f});
    });
    openDialog(ui, dlg);
    ui.setFocus(le, FocusReason::Keyboard);
    return dlg;
}

// ---- wizard -------------------------------------------------------------------------------------------------------------
NodeHandle wizard(UiContext& ui, std::string_view title, std::string_view pageTitles, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle dlg = dialog(ui, title, id);
    const NodeHandle body = dialogBody(ui, dlg);
    const NodeHandle stNode = panel(ui, body);
    ui.setCss(stNode, "display", "none");
    WizardState& s = wizPool().create(ui, stNode);
    s.card = dlg;
    const NodeHandle stepTitle = label(ui, body, "");
    ui.addClass(stepTitle, "wizard-step");
    s.stepTitle = stepTitle;
    s.stack = panel(ui, body);
    ui.setCss(s.stack, "flex-grow", "1");
    for (std::string_view t : split(pageTitles)) {
        const NodeHandle p = panel(ui, s.stack);
        ui.setA11y(p, A11yRole::Group, t);
        s.titles.emplace_back(t);
        ++s.count;
    }
    const NodeHandle btns = panel(ui, body);
    ui.addClass(btns, "dialog-buttons");
    const NodeHandle cancel = button(ui, btns, "Cancel");
    s.back = button(ui, btns, "< Back");
    s.next = button(ui, btns, "Next >");
    s.finish = button(ui, btns, "Finish");
    // QWizard keeps its own Back / Next / Finish / Cancel row (it is not a QDialogButtonBox); Escape = Cancel
    setDialogRejectButton(ui, dlg, cancel);
    ui.connect(s.back, SignalKind::Clicked, [dlg](UiContext& c, NodeHandle, const SignalArgs&) { wizardBack(c, dlg); });
    ui.connect(s.next, SignalKind::Clicked, [dlg](UiContext& c, NodeHandle, const SignalArgs&) { wizardNext(c, dlg); });
    ui.connect(s.finish, SignalKind::Clicked, [dlg](UiContext& c, NodeHandle, const SignalArgs&) {
        WizardState* w = wizPool().get(c, wizStateNode(c, dlg));
        if (w && w->canLeave && !w->canLeave(w->cur)) return;
        closeDialog(c, dlg);
        c.emit(dlg, SignalKind::Submitted, {.value = 1.f});
    });
    ui.connect(cancel, SignalKind::Clicked, [dlg](UiContext& c, NodeHandle, const SignalArgs&) {
        closeDialog(c, dlg);
        c.emit(dlg, SignalKind::Submitted, {.value = 0.f});
    });
    wizUpdate(ui, s);
    openDialog(ui, dlg);
    ui.setFocus(s.count > 1 ? s.next : s.finish, FocusReason::Keyboard); // QWizard focuses the default button
    return dlg;
}
NodeHandle wizardPage(const UiContext& ui, NodeHandle wiz, u32 index) {
    const NodeHandle st = wizStateNode(ui, wiz);
    const NodeHandle stack = ui.alive(st) ? ui.nextSibling(ui.nextSibling(st)) : NodeHandle{};
    return ui.alive(stack) ? nth(ui, stack, index) : NodeHandle{};
}
u32 wizardCurrent(const UiContext& ui, NodeHandle wiz) {
    auto* s = wizPool().get(ui, wizStateNode(ui, wiz));
    return s ? s->cur : 0u;
}
void wizardNext(UiContext& ui, NodeHandle wiz) {
    WizardState* s = wizPool().get(ui, wizStateNode(ui, wiz));
    if (!s || s->cur + 1 >= s->count) return;
    if (s->canLeave && !s->canLeave(s->cur)) return;
    ++s->cur;
    wizUpdate(ui, *s);
    ui.emit(wiz, SignalKind::ValueChanged, {.value = f32(s->cur)});
}
void wizardBack(UiContext& ui, NodeHandle wiz) {
    WizardState* s = wizPool().get(ui, wizStateNode(ui, wiz));
    if (!s || s->cur == 0) return;
    --s->cur;
    wizUpdate(ui, *s);
    ui.emit(wiz, SignalKind::ValueChanged, {.value = f32(s->cur)});
}
void setWizardValidator(UiContext& ui, NodeHandle wiz, std::function<bool(u32)> canLeave) {
    if (WizardState* s = wizPool().get(ui, wizStateNode(ui, wiz))) s->canLeave = std::move(canLeave);
}

// ---- LCD API --------------------------------------------------------------------------------------------------------------
u8 sevenSegment(char c) {
    switch (std::tolower((unsigned char)c)) {
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
    lcdPool().create(ui, n).digits = std::max(1u, digits);
    ui.setRange(n, -1e15f, 1e15f, 0.f);
    return n;
}
void setLcdText(UiContext& ui, NodeHandle lcd, std::string_view t) {
    if (LcdState* s = lcdPool().get(ui, lcd)) { s->text = std::string(t); s->explicitText = true; ui.invalidate(lcd); }
}
void setLcdBase(UiContext& ui, NodeHandle lcd, u32 base) {
    if (LcdState* s = lcdPool().get(ui, lcd)) { s->base = (base == 2 || base == 8 || base == 16) ? base : 10; s->explicitText = false; ui.invalidate(lcd); }
}
std::string lcdText(const UiContext& ui, NodeHandle lcd) {
    auto* s = lcdPool().get(ui, lcd);
    return s ? lcdString(ui, lcd, *s) : std::string();
}

// ---- key sequences ------------------------------------------------------------------------------------------------------
std::string_view keyName(Key k) {
    switch (k) {
    case Key::Tab: return "Tab"; case Key::Enter: return "Return"; case Key::Escape: return "Esc"; case Key::Space: return "Space";
    case Key::Backspace: return "Backspace"; case Key::Delete: return "Del"; case Key::Left: return "Left"; case Key::Right: return "Right";
    case Key::Up: return "Up"; case Key::Down: return "Down"; case Key::Home: return "Home"; case Key::End: return "End";
    case Key::PageUp: return "PgUp"; case Key::PageDown: return "PgDown";
    case Key::A: return "A"; case Key::B: return "B"; case Key::C: return "C"; case Key::D: return "D"; case Key::E: return "E";
    case Key::F: return "F"; case Key::G: return "G"; case Key::H: return "H"; case Key::I: return "I"; case Key::J: return "J";
    case Key::K: return "K"; case Key::L: return "L"; case Key::M: return "M"; case Key::N: return "N"; case Key::O: return "O";
    case Key::P: return "P"; case Key::Q: return "Q"; case Key::R: return "R"; case Key::S: return "S"; case Key::T: return "T";
    case Key::U: return "U"; case Key::V: return "V"; case Key::W: return "W"; case Key::X: return "X"; case Key::Y: return "Y";
    case Key::Z: return "Z";
    case Key::Num0: return "0"; case Key::Num1: return "1"; case Key::Num2: return "2"; case Key::Num3: return "3"; case Key::Num4: return "4";
    case Key::Num5: return "5"; case Key::Num6: return "6"; case Key::Num7: return "7"; case Key::Num8: return "8"; case Key::Num9: return "9";
    case Key::F1: return "F1"; case Key::F2: return "F2"; case Key::F3: return "F3"; case Key::F4: return "F4"; case Key::F5: return "F5";
    case Key::F6: return "F6"; case Key::F7: return "F7"; case Key::F8: return "F8"; case Key::F9: return "F9"; case Key::F10: return "F10";
    case Key::F11: return "F11"; case Key::F12: return "F12";
    case Key::BracketLeft: return "["; case Key::BracketRight: return "]";
    default: return "";
    }
}
std::string keySequenceText(Key k, u8 mods) {
    std::string s;
    if (mods & kModCtrl) s += "Ctrl+";
    if (mods & kModAlt) s += "Alt+";
    if (mods & kModShift) s += "Shift+";
    if (mods & kModSuper) s += "Meta+";
    const std::string_view n = keyName(k);
    if (n.empty()) return {};
    return s + std::string(n);
}
NodeHandle keySequenceEdit(UiContext& ui, NodeHandle parent, std::string_view initial, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle n = make(ui, "keysequenceedit", parent, id);
    ui.setText(n, initial);
    return n;
}

// ---- MDI API --------------------------------------------------------------------------------------------------------------
NodeHandle mdiArea(UiContext& ui, NodeHandle parent, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle a = panel(ui, parent, id);
    ui.addClass(a, "mdiarea");
    ui.setCss(a, "layout", "absolute");
    ui.setCss(a, "flex-grow", "1");
    return a;
}
NodeHandle mdiSubWindow(UiContext& ui, NodeHandle area, std::string_view title, Rect g, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle sub = make(ui, "mdisubwindow", area, id);
    if (!ui.alive(sub)) return sub;
    ui.setA11y(sub, A11yRole::Window, title);
    mdiPool().create(ui, sub).geo = g;
    applyGeo(ui, sub, g);
    const NodeHandle bar = make(ui, "mdititle", sub);
    const NodeHandle t = label(ui, bar, title);
    ui.setCss(t, "flex-grow", "1");
    ui.setCss(t, "pointer-events", "none");
    const NodeHandle x = toolButton(ui, bar, "×");
    ui.connect(x, SignalKind::Clicked, [sub](UiContext& c, NodeHandle, const SignalArgs&) {
        c.emit(sub, SignalKind::Submitted, {.value = 0.f});
        c.destroy(sub);
    });
    const NodeHandle body = panel(ui, sub);
    ui.setCss(body, "flex-grow", "1");
    ui.setCss(body, "padding", "6px");
    const NodeHandle grip = make(ui, "mdititle", sub);
    ui.addClass(grip, "mdigrip");
    raise(ui, sub);
    return sub;
}
NodeHandle mdiBody(const UiContext& ui, NodeHandle sub) { return nth(ui, sub, 1); }
Rect mdiGeometry(const UiContext& ui, NodeHandle sub) {
    auto* s = mdiPool().get(ui, sub);
    return s ? s->geo : Rect{};
}
void setMdiGeometry(UiContext& ui, NodeHandle sub, Rect r) {
    if (MdiState* s = mdiPool().get(ui, sub)) { s->geo = r; applyGeo(ui, sub, r); }
}
std::vector<NodeHandle> mdiSubWindows(const UiContext& ui, NodeHandle area) {
    std::vector<NodeHandle> v;
    for (NodeHandle c = ui.firstChild(area); ui.alive(c); c = ui.nextSibling(c))
        if (mdiPool().get(ui, c)) v.push_back(c);
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
    for (u32 i = 0; i < n; ++i) {
        const u32 cx = i % cols, cy = i / cols;
        const f32 w = c.w / f32(cols), h = c.h / f32(rows);
        setMdiGeometry(ui, v[i], {f32(cx) * w, f32(cy) * h, w, h});
    }
}
void mdiCascade(UiContext& ui, NodeHandle area) {
    const auto v = mdiSubWindows(ui, area);
    const Rect c = ui.contentRect(area);
    for (u32 i = 0; i < v.size(); ++i)
        setMdiGeometry(ui, v[i], {f32(i) * 24.f, f32(i) * 24.f, std::max(160.f, c.w * 0.6f), std::max(120.f, c.h * 0.6f)});
}

// ---- completer ------------------------------------------------------------------------------------------------------------
std::vector<std::string> completions(const std::vector<std::string>& entries, std::string_view prefix, u32 max) {
    auto lower = [](std::string_view s) { std::string o(s); for (char& c : o) c = char(std::tolower((unsigned char)c)); return o; };
    const std::string p = lower(prefix);
    std::vector<std::string> a, b;
    for (const std::string& e : entries) {
        const std::string l = lower(e);
        if (l.starts_with(p)) a.push_back(e);
        else if (!p.empty() && l.find(p) != std::string::npos) b.push_back(e);
    }
    a.insert(a.end(), b.begin(), b.end());
    if (a.size() > max) a.resize(max);
    return a;
}
void attachCompleter(UiContext& ui, NodeHandle le, std::vector<std::string> entries, u32 maxShown) {
    // state lives on a hidden child-less sibling-free helper node under the line edit's parent
    const NodeHandle holder = panel(ui, ui.parent(le));
    ui.setCss(holder, "display", "none");
    CompleterState& s = compPool().create(ui, holder);
    s.entries = std::move(entries);
    s.max = maxShown;
    auto open = [holder, le](UiContext& c) {
        CompleterState* st = compPool().get(c, holder);
        if (!st) return;
        const auto m = completions(st->entries, c.editBuffer(le).text(), st->max);
        if (m.empty()) return;
        std::string items;
        for (const auto& e : m) { if (!items.empty()) items += '\n'; items += e; }
        const Rect r = c.rect(le);
        popupMenu(c, le, items, {r.x, r.bottom()});
    };
    ui.connect(le, SignalKind::Event, [open](UiContext& c, NodeHandle, const SignalArgs& a) {
        if (a.event && a.event->type == EventType::KeyDown && a.event->key == Key::Down) open(c);
    });
    ui.connect(le, SignalKind::Submitted, [le](UiContext& c, NodeHandle, const SignalArgs& a) {
        // popupMenu re-emits the pick on its owner with value = index; the line edit's own Enter has no event text
        if (a.text.empty() || a.text == c.editBuffer(le).text()) return;
        const std::string t(a.text);
        c.editBuffer(le).setText(t);
        c.editBuffer(le).end(false);
        c.setText(le, t);
        c.invalidate(le);
    });
}

// ---- graphics scene -------------------------------------------------------------------------------------------------------
std::vector<u32>* GraphicsScene::cell(i32 cx, i32 cy, bool create) {
    const u64 key = (u64(u32(cx)) << 32) | u32(cy);
    auto it = std::lower_bound(m_cells.begin(), m_cells.end(), key, [](const auto& a, u64 k) { return a.first < k; });
    if (it != m_cells.end() && it->first == key) return &it->second;
    if (!create) return nullptr;
    return &m_cells.insert(it, {key, {}})->second;
}
namespace {
Rect itemBounds(const GraphicsItem& it) {
    if (it.shape == GraphicsShape::Line)
        return {std::min(it.rect.x, it.rect.right()), std::min(it.rect.y, it.rect.bottom()), std::abs(it.rect.w), std::abs(it.rect.h)};
    if (it.shape == GraphicsShape::Polygon && !it.points.empty()) {
        f32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (Vec2 p : it.points) { x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y); }
        return {x0, y0, x1 - x0, y1 - y0};
    }
    return it.rect;
}
} // namespace
void GraphicsScene::index(u32 id, bool insert) {
    const Rect b = itemBounds(m_items[id]);
    const i32 x0 = i32(std::floor(b.x / kCell)), y0 = i32(std::floor(b.y / kCell));
    const i32 x1 = i32(std::floor((b.x + b.w) / kCell)), y1 = i32(std::floor((b.y + b.h) / kCell));
    for (i32 y = y0; y <= y1; ++y)
        for (i32 x = x0; x <= x1; ++x) {
            if (insert) cell(x, y, true)->push_back(id);
            else if (auto* c = cell(x, y, false)) std::erase(*c, id);
        }
}
u32 GraphicsScene::add(GraphicsItem it) {
    const u32 id = u32(m_items.size());
    m_items.push_back(std::move(it));
    m_alive.push_back(1);
    m_order.push_back(id);
    index(id, true);
    ++m_rev;
    if (onChanged) onChanged(id);
    return id;
}
void GraphicsScene::remove(u32 id) {
    if (id >= m_items.size() || !m_alive[id]) return;
    index(id, false);
    m_alive[id] = 0;
    std::erase(m_order, id);
    ++m_rev;
    if (onChanged) onChanged(id);
}
GraphicsItem* GraphicsScene::item(u32 id) { return id < m_items.size() && m_alive[id] ? &m_items[id] : nullptr; }
const GraphicsItem* GraphicsScene::item(u32 id) const { return id < m_items.size() && m_alive[id] ? &m_items[id] : nullptr; }
void GraphicsScene::move(u32 id, Vec2 d) {
    GraphicsItem* it = item(id);
    if (!it) return;
    index(id, false);
    it->rect.x += d.x;
    it->rect.y += d.y;
    for (Vec2& p : it->points) { p.x += d.x; p.y += d.y; }
    index(id, true);
    ++m_rev;
    if (onChanged) onChanged(id);
}
bool GraphicsScene::hit(const GraphicsItem& it, Vec2 p) {
    const Rect b = itemBounds(it);
    switch (it.shape) {
    case GraphicsShape::Ellipse: {
        if (b.w <= 0 || b.h <= 0) return false;
        const f32 nx = (p.x - b.x - b.w * 0.5f) / (b.w * 0.5f), ny = (p.y - b.y - b.h * 0.5f) / (b.h * 0.5f);
        return nx * nx + ny * ny <= 1.f;
    }
    case GraphicsShape::Line: {
        const Vec2 a{it.rect.x, it.rect.y}, c{it.rect.right(), it.rect.bottom()};
        const f32 dx = c.x - a.x, dy = c.y - a.y, l2 = dx * dx + dy * dy;
        const f32 t = l2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.f, 1.f) : 0.f;
        const f32 ex = a.x + t * dx - p.x, ey = a.y + t * dy - p.y;
        return ex * ex + ey * ey <= std::pow(std::max(3.f, it.strokeWidth), 2.f);
    }
    case GraphicsShape::Polygon: { // even-odd
        bool in = false;
        const auto& v = it.points;
        for (std::size_t i = 0, j = v.size() - 1; i < v.size(); j = i++)
            if (((v[i].y > p.y) != (v[j].y > p.y)) && p.x < (v[j].x - v[i].x) * (p.y - v[i].y) / (v[j].y - v[i].y) + v[i].x) in = !in;
        return in;
    }
    default: return b.contains(p);
    }
}
std::vector<u32> GraphicsScene::paintOrder() const {
    std::vector<u32> o = m_order;
    std::stable_sort(o.begin(), o.end(), [&](u32 a, u32 b) { return m_items[a].z < m_items[b].z; });
    return o;
}
u32 GraphicsScene::itemAt(Vec2 p) const {
    auto* self = const_cast<GraphicsScene*>(this);
    const auto* c = self->cell(i32(std::floor(p.x / kCell)), i32(std::floor(p.y / kCell)), false);
    if (!c) return ~0u;
    u32 best = ~0u;
    for (u32 id : *c) {
        const GraphicsItem& it = m_items[id];
        if (!it.visible || !hit(it, p)) continue;
        if (best == ~0u || it.z > m_items[best].z || (it.z == m_items[best].z &&
            std::find(m_order.begin(), m_order.end(), id) > std::find(m_order.begin(), m_order.end(), best))) best = id;
    }
    return best;
}
std::vector<u32> GraphicsScene::itemsIn(const Rect& r) const {
    std::vector<u32> out;
    for (u32 id : paintOrder()) {
        const GraphicsItem& it = m_items[id];
        if (it.visible && !itemBounds(it).intersect(r).empty()) out.push_back(id);
    }
    return out;
}
void GraphicsScene::select(u32 id, bool on, bool exclusive) {
    if (exclusive) clearSelection();
    if (GraphicsItem* it = item(id)) { it->selected = on && it->selectable; ++m_rev; }
}
void GraphicsScene::clearSelection() { for (u32 id : m_order) m_items[id].selected = false; ++m_rev; }
std::vector<u32> GraphicsScene::selection() const {
    std::vector<u32> s;
    for (u32 id : m_order) if (m_items[id].selected) s.push_back(id);
    return s;
}
Rect GraphicsScene::bounds() const {
    f32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (u32 id : m_order) {
        const Rect b = itemBounds(m_items[id]);
        x0 = std::min(x0, b.x); y0 = std::min(y0, b.y); x1 = std::max(x1, b.right()); y1 = std::max(y1, b.bottom());
    }
    return m_order.empty() ? Rect{} : Rect{x0, y0, x1 - x0, y1 - y0};
}
u32 GraphicsScene::size() const { return u32(m_order.size()); }

NodeHandle graphicsView(UiContext& ui, NodeHandle parent, GraphicsScene* scene, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle v = make(ui, "graphicsview", parent, id);
    if (!ui.alive(v)) return v;
    viewPool().create(ui, v).scene = scene;
    return v;
}
void setViewTransform(UiContext& ui, NodeHandle view, f32 zoom, Vec2 pan) {
    if (ViewState* v = viewPool().get(ui, view)) { v->zoom = std::clamp(zoom, 0.05f, 40.f); v->pan = pan; ui.invalidate(view); }
}
f32 viewZoom(const UiContext& ui, NodeHandle view) { auto* v = viewPool().get(ui, view); return v ? v->zoom : 1.f; }
Vec2 viewPan(const UiContext& ui, NodeHandle view) { auto* v = viewPool().get(ui, view); return v ? v->pan : Vec2{}; }
Vec2 viewToScene(const UiContext& ui, NodeHandle view, Vec2 p) {
    auto* v = viewPool().get(ui, view);
    if (!v) return p;
    const Rect c = ui.contentRect(view);
    return {v->pan.x + (p.x - c.x) / v->zoom, v->pan.y + (p.y - c.y) / v->zoom};
}
Vec2 sceneToView(const UiContext& ui, NodeHandle view, Vec2 s) {
    auto* v = viewPool().get(ui, view);
    if (!v) return s;
    const Rect c = ui.contentRect(view);
    return {c.x + (s.x - v->pan.x) * v->zoom, c.y + (s.y - v->pan.y) * v->zoom};
}
void fitInView(UiContext& ui, NodeHandle view) {
    ViewState* v = viewPool().get(ui, view);
    if (!v || !v->scene) return;
    const Rect b = v->scene->bounds(), c = ui.contentRect(view);
    if (b.empty() || c.empty()) return;
    const f32 z = std::min(c.w / b.w, c.h / b.h) * 0.95f;
    setViewTransform(ui, view, z, {b.x + b.w * 0.5f - c.w * 0.5f / z, b.y + b.h * 0.5f - c.h * 0.5f / z});
}


// ---- virtual keyboard ---------------------------------------------------------------------------------------------------
namespace {
struct VkState {
    NodeHandle kb, target;
    bool shift = false, caps = false, symbols = false;
    std::vector<std::pair<NodeHandle, std::string>> keys; // character keys + their base label
    std::map<std::string, NodeHandle, std::less<>> named;
};
bool inSubtree(const UiContext& ui, NodeHandle root, NodeHandle n) {
    for (; ui.alive(n); n = ui.parent(n)) if (n == root) return true;
    return false;
}
constexpr std::string_view kVkLetters[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
constexpr std::string_view kVkSymbols[3] = {"1234567890", "@#$%&-+()/", "*\"':;!?,."};
std::string vkLabel(const VkState& st, std::size_t row, std::size_t col) {
    const std::string_view r = st.symbols ? kVkSymbols[row] : kVkLetters[row];
    if (col >= r.size()) return {};
    char c = r[col];
    if (!st.symbols && (st.shift || st.caps) && c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return std::string(1, c);
}
std::map<u32, std::shared_ptr<VkState>>& vkStates() { static std::map<u32, std::shared_ptr<VkState>> m; return m; }
void vkRelabel(UiContext& ui, VkState& st) {
    std::size_t i = 0;
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t col = 0; col < kVkLetters[row].size() || col < kVkSymbols[row].size(); ++col, ++i) {
            if (i >= st.keys.size()) return;
            const std::string l = vkLabel(st, row, col);
            ui.setText(st.keys[i].first, l);
            ui.setCss(st.keys[i].first, "display", l.empty() ? "none" : "flex");
        }
    ui.setText(st.named["?123"], st.symbols ? "ABC" : "?123");
    ui.setCss(st.named["Shift"], "opacity", st.caps ? "1" : st.shift ? "0.85" : "0.6");
}
} // namespace

NodeHandle virtualKeyboard(UiContext& ui, NodeHandle parent, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle kb = panel(ui, parent, id);
    ui.addClass(kb, "vkeyboard");
    ui.setCss(kb, "gap", "4px");
    ui.setCss(kb, "padding", "6px");
    ui.setA11y(kb, A11yRole::Group, "Keyboard");
    auto st = std::make_shared<VkState>();
    st->kb = kb;
    vkStates()[kb.index] = st;
    VkState* sp = st.get();
    // remember the last text node focused outside the keyboard
    auto resolveTarget = [sp](UiContext& u) {
        const NodeHandle f = u.focused();
        if (u.alive(f) && !inSubtree(u, sp->kb, f)) sp->target = f;
        if (u.alive(sp->target) && u.focused() != sp->target) u.setFocus(sp->target);
        return u.alive(sp->target);
    };
    auto makeRow = [&] {
        const NodeHandle r = panel(ui, kb);
        ui.setCss(r, "flex-direction", "row");
        ui.setCss(r, "gap", "4px");
        ui.setCss(r, "justify-content", "center");
        return r;
    };
    auto makeKey = [&](NodeHandle row, std::string_view text) {
        const NodeHandle k = button(ui, row, text);
        ui.setFocusable(k, false);
        ui.addClass(k, "vkey");
        ui.setCss(k, "min-width", "28px");
        return k;
    };
    for (std::size_t row = 0; row < 3; ++row) {
        const NodeHandle r = makeRow();
        if (row == 2) {
            const NodeHandle sh = makeKey(r, "Shift");
            st->named["Shift"] = sh;
            ui.connect(sh, SignalKind::Clicked, [sp](UiContext& u, NodeHandle, const SignalArgs&) {
                if (sp->caps) { sp->caps = sp->shift = false; }
                else if (sp->shift) { sp->caps = true; }
                else sp->shift = true;
                vkRelabel(u, *sp);
                u.emit(sp->kb, SignalKind::Toggled, {.flag = sp->shift || sp->caps});
            });
        }
        for (std::size_t col = 0; col < kVkLetters[row].size() || col < kVkSymbols[row].size(); ++col) {
            const NodeHandle k = makeKey(r, "");
            st->keys.emplace_back(k, std::string{});
            ui.connect(k, SignalKind::Clicked, [sp, resolveTarget](UiContext& u, NodeHandle n, const SignalArgs&) {
                if (!resolveTarget(u)) return;
                const std::string t(u.text(n));
                u.textInput(t);
                if (sp->shift && !sp->caps) { sp->shift = false; vkRelabel(u, *sp); u.emit(sp->kb, SignalKind::Toggled, {.flag = false}); }
            });
        }
        if (row == 2) {
            const NodeHandle bs = makeKey(r, "Backspace");
            st->named["Backspace"] = bs;
            ui.setText(bs, "\xE2\x8C\xAB"); // ⌫
            ui.connect(bs, SignalKind::Clicked, [resolveTarget](UiContext& u, NodeHandle, const SignalArgs&) {
                if (resolveTarget(u)) { u.keyDown(Key::Backspace); u.keyUp(Key::Backspace); }
            });
        }
    }
    const NodeHandle last = makeRow();
    const NodeHandle sym = makeKey(last, "?123");
    st->named["?123"] = sym;
    ui.connect(sym, SignalKind::Clicked, [sp](UiContext& u, NodeHandle, const SignalArgs&) {
        sp->symbols = !sp->symbols;
        u.setValue(sp->kb, sp->symbols ? 1.f : 0.f, true);
        vkRelabel(u, *sp);
    });
    const NodeHandle space = makeKey(last, "Space");
    st->named["Space"] = space;
    ui.setText(space, " ");
    ui.setCss(space, "min-width", "160px");
    ui.connect(space, SignalKind::Clicked, [resolveTarget](UiContext& u, NodeHandle, const SignalArgs&) {
        if (resolveTarget(u)) u.textInput(" ");
    });
    const NodeHandle enter = makeKey(last, "Enter");
    st->named["Enter"] = enter;
    ui.setText(enter, "\xE2\x8F\x8E"); // ⏎
    ui.connect(enter, SignalKind::Clicked, [resolveTarget](UiContext& u, NodeHandle, const SignalArgs&) {
        if (resolveTarget(u)) { u.keyDown(Key::Enter); u.keyUp(Key::Enter); }
    });
    ui.setRange(kb, 0.f, 1.f, 1.f);
    vkRelabel(ui, *st);
    return kb;
}

NodeHandle virtualKeyboardKey(const UiContext& ui, NodeHandle keyboard, std::string_view label) {
    auto it = vkStates().find(keyboard.index);
    if (it == vkStates().end() || it->second->kb != keyboard) return {};
    VkState& st = *it->second;
    if (auto n = st.named.find(label); n != st.named.end()) return n->second;
    for (auto& [k, base] : st.keys) if (ui.text(k) == label) return k;
    return {};
}

void virtualKeyboardAutoShow(UiContext& ui, NodeHandle keyboard) {
    auto it = vkStates().find(keyboard.index);
    if (it == vkStates().end() || !ui.alive(keyboard)) return;
    const NodeHandle f = ui.focused();
    const bool editing = ui.wantsTextInput() || (ui.alive(f) && inSubtree(ui, keyboard, f));
    ui.setCss(keyboard, "display", editing ? "flex" : "none");
}

} // namespace fuse::ui::widgets
