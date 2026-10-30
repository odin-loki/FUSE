// In-app drag and drop (QDrag / QMimeData / QDropEvent): sources start a drag past the start distance, the payload
// carries MIME-typed data, drop targets accept per position with a drop action. OS-level DnD stays in fuse::app.
#include "fuse/ui/widgets/widgets.hpp"
#include "widgets_internal.hpp"

#include <unordered_map>

namespace fuse::ui::widgets {
namespace {

struct Target { u32 gen = 0; DropHandler fn; };
struct Source { u32 gen = 0; DragStarter start; DragFinished done; };
struct Dnd {
    NodeHandle src;
    Vec2 press;
    bool armed = false, active = false;
    DragPayload payload;
    DragStarter start;
    DragFinished done;
    NodeHandle target;
    u8 action = 0;
    Vec2 pos;
    std::unordered_map<u32, Target> targets;
    std::unordered_map<u32, Source> sources;
    NodeHandle indNode;
    Rect indRect;
    NodeHandle image;          ///< drag image card (overlay layer, pointer-events none)
    bool external = false;     ///< handed off to the OS drag (setExternalDragHandler)
    std::function<bool(UiContext&, const DragPayload&)> externalStart;
    // auto-scroll / hover delay
    TimerHandle scrollTimer, hoverTimer;
    NodeHandle scrollView, hoverView;
    Rect scrollArea;
    std::function<bool(UiContext&, NodeHandle, i32)> scrollFn;
    u64 hoverKey = ~0ull;
    std::function<void(UiContext&, NodeHandle)> hoverFn;
    u8 mods = 0;
};
std::unordered_map<const UiContext*, Dnd>& all() { static std::unordered_map<const UiContext*, Dnd> m; return m; }
Dnd& dndOf(const UiContext& ui) { return all()[&ui]; }

u8 proposedAction(u8 allowed, u8 mods) {
    // Qt: Ctrl copies, Shift moves, Ctrl+Shift links; otherwise the first of Move, Copy, Link the source allows
    u8 want = (mods & kModCtrl) && (mods & kModShift) ? DropLink : (mods & kModCtrl) ? DropCopy : (mods & kModShift) ? DropMove : 0;
    if (want & allowed) return want;
    for (u8 a : {DropMove, DropCopy, DropLink}) if (a & allowed) return a;
    return 0;
}
void deliver(UiContext& ui, Dnd& d, NodeHandle t, DropPhase ph, u8 mods) {
    auto it = d.targets.find(t.index);
    if (it == d.targets.end() || it->second.gen != t.generation || !it->second.fn) return;
    d.mods = mods;
    const DropEvent ev{ph, d.pos, &d.payload, proposedAction(d.payload.actions, mods), d.src, mods};
    DropHandler fn = it->second.fn; // the handler may re-register
    const u8 a = fn(ui, t, ev);
    if (ph == DropPhase::Enter || ph == DropPhase::Move || ph == DropPhase::Drop) d.action = (a & d.payload.actions) ? a : 0;
}
/// The innermost registered drop target under `p` (ancestors walked).
NodeHandle targetAt(const UiContext& ui, const Dnd& d, Vec2 p) {
    for (NodeHandle h = ui.hitTest(p); ui.alive(h); h = ui.parent(h)) {
        auto it = d.targets.find(h.index);
        if (it != d.targets.end() && it->second.gen == h.generation && it->second.fn) return h;
    }
    return {};
}
CursorShape cursorFor(u8 action) {
    return action & DropMove ? CursorShape::Move : action & DropCopy ? CursorShape::Copy : action & DropLink ? CursorShape::Link : CursorShape::Forbidden;
}
void stopTimers(UiContext& ui, Dnd& d) {
    if (d.scrollTimer.valid()) ui.cancelTimer(d.scrollTimer);
    if (d.hoverTimer.valid()) ui.cancelTimer(d.hoverTimer);
    d.scrollTimer = d.hoverTimer = {};
    d.scrollView = d.hoverView = {};
    d.scrollFn = {};
    d.hoverKey = ~0ull;
}
void placeImage(UiContext& ui, Dnd& d) {
    if (!ui.alive(d.image)) return;
    ui.setCss(d.image, "left", std::to_string(int(d.pos.x + 14.f)) + "px");
    ui.setCss(d.image, "top", std::to_string(int(d.pos.y + 10.f)) + "px");
    if (d.action == 0) ui.addClass(d.image, "drag-refused");
    else ui.removeClass(d.image, "drag-refused");
}
void makeImage(UiContext& ui, Dnd& d) {
    std::string t = d.payload.label;
    if (t.empty()) {
        t = std::string(d.payload.get("text/plain"));
        if (const std::size_t nl = t.find('\n'); nl != std::string::npos) t.resize(nl);
        if (t.empty() && !d.payload.formats.empty()) t = d.payload.formats.front().first;
    }
    if (t.size() > 64) t = t.substr(0, 61) + "...";
    d.image = label(ui, overlayLayer(ui), t);
    ui.addClass(d.image, "drag-image");
    ui.setCss(d.image, "position", "absolute");
    ui.setCss(d.image, "pointer-events", "none");
    ui.setCss(d.image, "opacity", "0.85");
    ui.setCss(d.image, "padding", "2px 8px");
    ui.setCss(d.image, "border-radius", "4px");
    ui.setCss(d.image, "background-color", "#2A2A33");
    placeImage(ui, d);
}
void endDrag(UiContext& ui, Dnd& d, u8 action) {
    const NodeHandle src = d.src;
    stopTimers(ui, d);
    if (ui.alive(d.image)) ui.destroy(d.image);
    d.image = {};
    d.external = false;
    ui.setCursorShape(CursorShape::Default);
    DragFinished done = std::move(d.done);
    if (ui.alive(d.indNode)) ui.invalidate(d.indNode);
    d.indNode = {};
    d.armed = d.active = false;
    d.target = {};
    d.start = {};
    d.done = {};
    d.payload = {};
    d.src = {};
    if (done && ui.alive(src)) done(ui, src, action);
}

} // namespace

std::string_view DragPayload::get(std::string_view mime) const {
    for (const auto& [m, v] : formats) if (m == mime) return v;
    return {};
}
bool DragPayload::has(std::string_view mime) const {
    for (const auto& f : formats) if (f.first == mime) return true;
    return false;
}
void DragPayload::set(std::string_view mime, std::string_view data) {
    for (auto& [m, v] : formats) if (m == mime) { v = data; return; }
    formats.emplace_back(std::string(mime), std::string(data));
}

void setDropTarget(UiContext& ui, NodeHandle n, DropHandler fn) {
    Dnd& d = dndOf(ui);
    if (fn) d.targets[n.index] = {n.generation, std::move(fn)};
    else d.targets.erase(n.index);
}
void setDragSource(UiContext& ui, NodeHandle n, DragStarter start, DragFinished done) {
    Dnd& d = dndOf(ui);
    const bool had = d.sources.contains(n.index) && d.sources[n.index].gen == n.generation;
    if (!start) { d.sources.erase(n.index); return; }
    d.sources[n.index] = {n.generation, std::move(start), std::move(done)};
    if (had) return;
    // widgets that leave pointer events unconsumed report them as SignalKind::Event
    ui.connect(n, SignalKind::Event, [](UiContext& c, NodeHandle self, const SignalArgs& a) {
        if (!a.event) return;
        Dnd& dd = dndOf(c);
        auto it = dd.sources.find(self.index);
        if (it == dd.sources.end() || it->second.gen != self.generation) return;
        const Event& e = *a.event;
        if (e.type == EventType::PointerDown && e.button == MouseButton::Left) dnd::press(c, self, e.pos, it->second.start, it->second.done);
        else if (e.type == EventType::PointerMove) dnd::move(c, self, e.pos, e.mods);
        else if (e.type == EventType::PointerUp) dnd::release(c, self, e.pos, e.mods);
        else if (e.type == EventType::KeyDown && e.key == Key::Escape) cancelDrag(c);
    });
}
bool dragActive(const UiContext& ui) { return dndOf(ui).active; }
NodeHandle dragTarget(const UiContext& ui) { return dndOf(ui).target; }
u8 dragAction(const UiContext& ui) { return dndOf(ui).active ? dndOf(ui).action : 0; }
const DragPayload* dragPayload(const UiContext& ui) { return dndOf(ui).active ? &dndOf(ui).payload : nullptr; }
void cancelDrag(UiContext& ui) {
    Dnd& d = dndOf(ui);
    if (!d.active) { d.armed = false; return; }
    if (ui.alive(d.target)) deliver(ui, d, d.target, DropPhase::Leave, 0);
    endDrag(ui, d, 0);
}
void setDropIndicator(UiContext& ui, NodeHandle n, Rect r) {
    Dnd& d = dndOf(ui);
    if (!n.valid()) { if (ui.alive(d.indNode)) ui.invalidate(d.indNode); d.indNode = {}; return; }
    if (ui.alive(d.indNode)) ui.invalidate(d.indNode);
    d.indNode = r.w > 0.f || r.h > 0.f ? n : NodeHandle{};
    d.indRect = r;
    if (ui.alive(n)) ui.invalidate(n);
}
bool dropIndicator(const UiContext& ui, NodeHandle n, Rect& r) {
    const Dnd& d = dndOf(ui);
    if (!d.active || d.indNode != n) return false;
    r = d.indRect;
    return true;
}

namespace dnd {
void press(UiContext& ui, NodeHandle src, Vec2 p, DragStarter start, DragFinished done) {
    Dnd& d = dndOf(ui);
    if (d.active) return;
    d.src = src;
    d.press = p;
    d.armed = true;
    d.start = std::move(start);
    d.done = std::move(done);
}
bool move(UiContext& ui, NodeHandle src, Vec2 p, u8 mods) {
    Dnd& d = dndOf(ui);
    if (d.src != src || !(d.armed || d.active)) return false;
    d.pos = p;
    if (!d.active) {
        const f32 dx = p.x - d.press.x, dy = p.y - d.press.y;
        if (dx * dx + dy * dy < kStartDragDistance * kStartDragDistance) return false;
        d.armed = false;
        d.payload = {};
        if (!d.start || !d.start(ui, src, d.press, d.payload) || d.payload.formats.empty() || !d.payload.actions) {
            d.start = {};
            d.done = {};
            return false;
        }
        d.active = true;
        makeImage(ui, d);
    }
    if (d.external) return true;
    const Size vp = ui.viewport();
    if (d.externalStart && (p.x < 0.f || p.y < 0.f || p.x >= vp.w || p.y >= vp.h)) {
        // left the window: hand the drag to the OS (QDrag continues outside the application)
        if (ui.alive(d.target)) deliver(ui, d, d.target, DropPhase::Leave, mods);
        d.target = {};
        auto start = d.externalStart;
        if (start(ui, d.payload)) {
            d.external = true;
            stopTimers(ui, d);
            if (ui.alive(d.image)) ui.destroy(d.image);
            d.image = {};
            setDropIndicator(ui, {}, {});
            return true;
        }
    }
    const NodeHandle t = targetAt(ui, d, p);
    if (t != d.target) {
        if (ui.alive(d.target)) deliver(ui, d, d.target, DropPhase::Leave, mods);
        if (ui.alive(d.indNode)) ui.invalidate(d.indNode);
        d.indNode = {};
        d.target = t;
        d.action = 0;
        if (ui.alive(t)) deliver(ui, d, t, DropPhase::Enter, mods);
    }
    if (ui.alive(t)) deliver(ui, d, t, DropPhase::Move, mods);
    if (d.scrollView.valid() && d.scrollView != t) stopTimers(ui, d);
    if (d.hoverView.valid() && d.hoverView != t) { if (d.hoverTimer.valid()) ui.cancelTimer(d.hoverTimer); d.hoverTimer = {}; d.hoverView = {}; d.hoverKey = ~0ull; }
    if (!ui.alive(t)) d.action = 0;
    ui.setCursorShape(cursorFor(d.action)); // QDrag cursor feedback: move / copy / link, forbidden when refused
    placeImage(ui, d);
    return true;
}
bool release(UiContext& ui, NodeHandle src, Vec2 p, u8 mods) {
    Dnd& d = dndOf(ui);
    if (d.src != src) return false;
    if (!d.active) { d.armed = false; d.start = {}; d.done = {}; return false; }
    if (d.external) return true; // the OS drag finishes it (finishExternalDrag)
    move(ui, src, p, mods);
    if (d.external) return true;
    u8 action = 0;
    if (ui.alive(d.target) && d.action) {
        deliver(ui, d, d.target, DropPhase::Drop, mods);
        action = d.action;
    }
    endDrag(ui, d, action);
    return true;
}
void autoScroll(UiContext& ui, NodeHandle view, Rect area, std::function<bool(UiContext&, NodeHandle, i32 dir)> scroll) {
    Dnd& d = dndOf(ui);
    if (!d.active) return;
    constexpr f32 kMargin = 16.f; // QAbstractItemView::autoScrollMargin
    const bool near = d.pos.y < area.y + kMargin || d.pos.y > area.bottom() - kMargin;
    if (!near) {
        if (d.scrollTimer.valid()) ui.cancelTimer(d.scrollTimer);
        d.scrollTimer = {};
        d.scrollView = {};
        return;
    }
    d.scrollView = view;
    d.scrollArea = area;
    d.scrollFn = std::move(scroll);
    if (d.scrollTimer.valid()) return;
    d.scrollTimer = ui.startTimer(0.05f, true, [](UiContext& c) {
        Dnd& dd = dndOf(c);
        const NodeHandle v = dd.scrollView;
        const bool top = dd.pos.y < dd.scrollArea.y + kMargin, bottom = dd.pos.y > dd.scrollArea.bottom() - kMargin;
        if (!dd.active || !c.alive(v) || dd.target != v || !(top || bottom) || !dd.scrollFn || !dd.scrollFn(c, v, top ? -1 : 1)) {
            if (dd.scrollTimer.valid()) c.cancelTimer(dd.scrollTimer);
            dd.scrollTimer = {};
            return;
        }
        deliver(c, dd, v, DropPhase::Move, dd.mods); // the row under the pointer changed
        c.setCursorShape(cursorFor(dd.action));
    });
}
void hoverDelay(UiContext& ui, NodeHandle view, u64 key, f32 delay, std::function<void(UiContext&, NodeHandle)> fn) {
    Dnd& d = dndOf(ui);
    if (!d.active || (d.hoverView == view && d.hoverKey == key && d.hoverTimer.valid())) return;
    if (d.hoverTimer.valid()) ui.cancelTimer(d.hoverTimer);
    d.hoverView = view;
    d.hoverKey = key;
    d.hoverFn = std::move(fn);
    d.hoverTimer = ui.startTimer(std::max(delay, 0.001f), false, [](UiContext& c) {
        Dnd& dd = dndOf(c);
        dd.hoverTimer = {};
        const NodeHandle view = dd.hoverView;
        if (!dd.active || dd.target != view || !c.alive(view) || !dd.hoverFn) return;
        dd.hoverKey = ~0ull;
        auto fn = std::move(dd.hoverFn);
        fn(c, view);
        deliver(c, dd, view, DropPhase::Move, dd.mods);
    });
}
} // namespace dnd

void setExternalDragHandler(UiContext& ui, std::function<bool(UiContext&, const DragPayload&)> start) { dndOf(ui).externalStart = std::move(start); }
void finishExternalDrag(UiContext& ui, u8 action) {
    Dnd& d = dndOf(ui);
    if (!d.active || !d.external) return;
    u8 a = action & d.payload.actions;
    if (!a && action) // the OS reports accepted / refused only: the source's preferred allowed action
        a = d.payload.actions & DropCopy ? u8(DropCopy) : d.payload.actions & DropMove ? u8(DropMove) : u8(d.payload.actions & DropLink);
    endDrag(ui, d, a);
}
bool externalDragActive(const UiContext& ui) { return dndOf(ui).active && dndOf(ui).external; }
Vec2 dragPosition(const UiContext& ui) { return dndOf(ui).pos; }
NodeHandle dragImage(const UiContext& ui) { return dndOf(ui).image; }

} // namespace fuse::ui::widgets
