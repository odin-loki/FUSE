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
void endDrag(UiContext& ui, Dnd& d, u8 action) {
    const NodeHandle src = d.src;
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
    return true;
}
bool release(UiContext& ui, NodeHandle src, Vec2 p, u8 mods) {
    Dnd& d = dndOf(ui);
    if (d.src != src) return false;
    if (!d.active) { d.armed = false; d.start = {}; d.done = {}; return false; }
    move(ui, src, p, mods);
    u8 action = 0;
    if (ui.alive(d.target) && d.action) {
        deliver(ui, d, d.target, DropPhase::Drop, mods);
        action = d.action;
    }
    endDrag(ui, d, action);
    return true;
}
} // namespace dnd

} // namespace fuse::ui::widgets
