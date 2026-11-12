// FUSE UI core interaction (plan §4.7): drag and drop, click counting, touch gestures and multi-touch. Part of
// UiContext (declared in ui.hpp); uses the node tree through the public API only.
#include "fuse/ui/ui.hpp"

#include <algorithm>
#include <cmath>

namespace fuse::ui {

namespace {
f32 dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }
constexpr f32 kPi = 3.14159265358979f;
} // namespace

bool UiContext::sendEvent(NodeHandle n, const Event& e) { return node(n) ? dispatch(n.index, e) : false; }

// ---- Drag and drop: registration ------------------------------------------------------------------------------------
UiContext::DndNode* UiContext::dndEntry(NodeHandle n, bool create) {
    DndNode* dead = nullptr;
    for (auto& d : m_dnd) {
        if (d.node == n) return &d;
        if (!dead && !node(d.node)) dead = &d;
    }
    if (!create) return nullptr;
    if (!dead) dead = &m_dnd.emplace_back();
    *dead = DndNode{};
    dead->node = n;
    return dead;
}

void UiContext::setDragSource(NodeHandle n, DragSourceFn fn) {
    if (!node(n)) return;
    if (DndNode* d = dndEntry(n, fn != nullptr)) d->source = fn;
}
void UiContext::setDropTypes(NodeHandle n, std::initializer_list<u32> types) {
    if (!node(n)) return;
    DndNode* d = dndEntry(n, types.size() != 0);
    if (!d) return;
    d->typeCount = 0;
    for (u32 t : types)
        if (d->typeCount < 4) d->types[d->typeCount++] = t;
}
void UiContext::setDropFilter(NodeHandle n, DropFilterFn fn) {
    if (!node(n)) return;
    if (DndNode* d = dndEntry(n, fn != nullptr)) d->filter = fn;
}

NodeHandle UiContext::dropTargetAt(Vec2 p, const DragData& data) {
    for (NodeHandle a = hitTest(p); node(a); a = parent(a)) {
        if (node(m_modal) && !inScope(a.index, m_modal.index)) break; // a modal scope blocks what is outside it
        if (states(a) & kStateDisabled) continue;
        const WidgetClass& wc = m_registry.get(classOf(a));
        if (wc.acceptsDrop && wc.acceptsDrop(*this, a, data, p)) return a;
        if (const DndNode* d = dndEntry(a, false)) {
            bool typeOk = false;
            for (u8 k = 0; k < d->typeCount; ++k) typeOk = typeOk || d->types[k] == data.type;
            // a filter alone decides everything; with a type list it only sees listed types
            if (d->filter ? ((d->typeCount == 0 || typeOk) && d->filter(*this, a, data, p)) : typeOk) return a;
        }
    }
    return {};
}

// ---- Drag and drop: the drag ----------------------------------------------------------------------------------------
bool UiContext::beginDrag(NodeHandle source, DragData data, const DragImage& image) {
    if (m_drag.active || !node(source)) return false;
    cancelPress(m_pointer); // the press ends without a Click
    if (m_touch.stage == TouchState::Stage::Pending) m_touch.stage = TouchState::Stage::Passive; // the finger never pans
    m_touch.pressed = false;
    m_gesture.longArmed = false;
    m_gesture.moved = true;
    m_drag = DragState{};
    m_drag.active = true;
    m_drag.data = std::move(data);
    m_drag.data.source = source;
    m_drag.image = image;
    setHover(0xFFFFFFFFu);
    if (!node(m_drag.data.source)) { // a hover slot destroyed the source
        m_drag.active = false;
        return false;
    }
    dragMoveTo(m_pointer);
    return true;
}

void UiContext::cancelDrag() {
    if (m_drag.active) dragFinish(m_drag.pos, false);
}

void UiContext::setDropIndicator(Rect r) {
    m_drag.indicator = r;
    m_drag.hasIndicator = !r.empty();
    m_overlayDirty = m_anyPaintDirty = true;
}

void UiContext::dragSetTarget(NodeHandle t, Vec2 p) {
    if (t == m_drag.target) return;
    const NodeHandle old = m_drag.target;
    m_drag.target = t;
    m_drag.hasIndicator = false;
    if (node(old)) {
        setState(old, kStateDragOver, false);
        Event e; e.type = EventType::DragLeave; e.pos = p; e.mods = m_mods; e.drag = &m_drag.data;
        dispatch(old.index, e);
    }
    if (node(t) && m_drag.target == t) {
        setState(t, kStateDragOver, true);
        Event e; e.type = EventType::DragEnter; e.pos = p; e.mods = m_mods; e.drag = &m_drag.data; e.dropAction = m_drag.action;
        dispatch(t.index, e);
    }
}

bool UiContext::dragMoveTo(Vec2 p) {
    m_drag.pos = p;
    m_overlayDirty = m_anyPaintDirty = true;
    const NodeHandle t = dropTargetAt(p, m_drag.data);
    m_drag.action = node(t) ? ((m_mods & kModCtrl) ? DropAction::Copy : DropAction::Move) : DropAction::None;
    dragSetTarget(t, p);
    if (m_drag.active && node(m_drag.target)) {
        Event e; e.type = EventType::DragMove; e.pos = p; e.mods = m_mods; e.drag = &m_drag.data; e.dropAction = m_drag.action;
        dispatch(m_drag.target.index, e);
    }
    return true;
}

bool UiContext::dragFinish(Vec2 p, bool drop) {
    if (!m_drag.active) return false;
    DropAction result = DropAction::None;
    if (drop) {
        dragMoveTo(p); // the target and the action are those of the release point
        if (node(m_drag.target) && m_drag.action != DropAction::None) {
            Event e; e.type = EventType::Drop; e.pos = p; e.mods = m_mods; e.drag = &m_drag.data; e.dropAction = m_drag.action;
            dispatch(m_drag.target.index, e);
            result = m_drag.action; // the handler may have refused (setDropAction(None)) or changed it
        }
    }
    const NodeHandle t = m_drag.target;
    m_drag.target = {};
    m_drag.hasIndicator = false;
    if (node(t)) {
        setState(t, kStateDragOver, false);
        if (result == DropAction::None) {
            Event e; e.type = EventType::DragLeave; e.pos = p; e.mods = m_mods; e.drag = &m_drag.data;
            dispatch(t.index, e);
        }
    }
    m_drag.active = false;
    m_drag.action = result;
    m_lastDrop = result;
    m_lastDropTarget = result != DropAction::None ? t : NodeHandle{};
    m_overlayDirty = m_anyPaintDirty = true;
    const NodeHandle src = m_drag.data.source;
    if (node(src)) {
        Event e; e.type = EventType::DragEnd; e.pos = p; e.mods = m_mods; e.drag = &m_drag.data; e.dropAction = result;
        dispatch(src.index, e);
    }
    return true;
}

bool UiContext::startDragFrom(NodeHandle from, Vec2 press) {
    for (NodeHandle a = from; node(a); a = parent(a)) {
        DragSourceFn fn = m_registry.get(classOf(a)).dragStart;
        if (const DndNode* d = dndEntry(a, false); d && d->source) fn = d->source;
        if (!fn) continue;
        if (states(a) & kStateDisabled) return false;
        DragData data;
        DragImage image;
        if (!fn(*this, a, press, data, image)) continue; // nothing to drag here: an ancestor may have
        return node(a) && beginDrag(a, std::move(data), image);
    }
    return false;
}

bool UiContext::maybeStartDrag(Vec2 p) {
    using Stage = TouchState::Stage;
    if (m_dragAsked || m_drag.active) return false;
    // touch: while the finger may still become a pan the drag waits (it starts on a long press instead)
    if (m_touch.stage == Stage::Pan || (m_touch.stage == Stage::Pending && !m_gesture.dragOk)) return false;
    if (dist(p, m_pressPos) < m_gestureCfg.dragThreshold) return false;
    m_dragAsked = true; // one question per press
    return startDragFrom(node(m_pressHit) ? m_pressHit : m_pressed, m_pressPos);
}

void UiContext::paintDragOverlay(DrawList& dl) {
    const Color accent = Color::rgb(0x3D8BFD);
    if (m_drag.hasIndicator) {
        const Rect r = m_drag.indicator;
        if (std::min(r.w, r.h) <= 4.f) dl.rect(r, accent, std::min(r.w, r.h) * 0.5f); // insertion line
        else dl.roundedRect(r, accent.withAlpha(0.12f), {3.f, 3.f, 3.f, 3.f}, accent, 1.5f); // drop zone
    }
    if (m_drag.external) return; // the OS shows its own drag image
    const Vec2 at = m_drag.pos;
    const DragImage& im = m_drag.image;
    if (im.draw) {
        im.draw(*this, dl, at, m_drag.data, im.user);
        return;
    }
    if (node(im.node)) { // a translucent copy of the node under the pointer
        const Rect r = rect(im.node);
        const Rect savedDamage = m_damage;
        const u32 savedPainted = m_stats.painted;
        m_dragList.clear();
        paintRec(im.node.index, std::clamp(im.opacity, 0.05f, 1.f), m_dragList);
        m_damage = savedDamage;
        m_stats.painted = savedPainted;
        m_dragList.translate({at.x - im.hotspot.x - r.x, at.y - im.hotspot.y - r.y});
        for (const DrawCmd& c : m_dragList.cmds()) dl.push(c);
        return;
    }
    // default: a chip with the data's text (first line), "+" when the drop would copy
    std::string_view text = m_drag.data.text;
    text = text.substr(0, text.find('\n'));
    const f32 fs = 13.f;
    const bool copy = m_drag.action == DropAction::Copy;
    const f32 tw = text.empty() ? 12.f : std::ceil(measureString(text, fs));
    const Rect chip{std::round(at.x + 12.f), std::round(at.y + 14.f), tw + 16.f + (copy ? 16.f : 0.f), 24.f};
    dl.roundedRect(chip, Color::rgb(0x23262E, 0.94f), {6.f, 6.f, 6.f, 6.f}, accent.withAlpha(0.9f), 1.f);
    if (!text.empty()) paintString(dl, text, {chip.x + 8.f, chip.y, tw + 1.f, chip.h}, Color::rgb(0xF2F4F8), fs, TextAlignment::Start, 0xFFFFFFFFu);
    if (copy) {
        const f32 cx = chip.right() - 12.f, cy = chip.y + chip.h * 0.5f;
        dl.line({cx - 4.f, cy}, {cx + 4.f, cy}, accent, 1.5f);
        dl.line({cx, cy - 4.f}, {cx, cy + 4.f}, accent, 1.5f);
    }
}

// ---- Drag and drop: external (OS) drags -----------------------------------------------------------------------------
bool UiContext::dragExternalMove(Vec2 pos, const DragData& data) {
    if (m_drag.active && !m_drag.external) return false;
    if (!m_drag.active) {
        m_drag = DragState{};
        m_drag.active = m_drag.external = true;
        m_drag.data = data;
        m_drag.data.source = {};
    }
    dragMoveTo(pos);
    return node(m_drag.target) && m_drag.action != DropAction::None;
}
void UiContext::dragExternalLeave() {
    if (m_drag.active && m_drag.external) dragFinish(m_drag.pos, false);
}
DropAction UiContext::dropExternal(Vec2 pos, const DragData& data) {
    if (m_drag.active && !m_drag.external) return DropAction::None;
    const bool hovering = m_drag.active;
    if (!hovering) {
        m_drag = DragState{};
        m_drag.active = m_drag.external = true;
    }
    m_drag.data = data; // the drop carries the final data (some platforms only reveal the paths on drop)
    m_drag.data.source = {};
    dragFinish(pos, true);
    return m_lastDrop;
}

// ---- Clicks ---------------------------------------------------------------------------------------------------------
void UiContext::noteClick(u32 target, NodeHandle hit, Vec2 p) {
    const NodeHandle t = handleOf(target);
    const bool again = m_clickCount == 1 && t == m_clickNode && m_clock - m_clickTime <= double(m_gestureCfg.doubleClickTime) &&
                       dist(p, m_clickPos) <= m_gestureCfg.doubleClickSlop;
    m_clickCount = again ? 2 : 1; // a third quick click starts a new sequence
    m_clickNode = t;
    m_clickTime = m_clock;
    m_clickPos = p;
    m_pressPos = p;
    m_pressHit = hit;
    m_dragAsked = false;
}

// ---- Touch gestures -------------------------------------------------------------------------------------------------
void UiContext::gestureArm(Vec2, double t) {
    GestureState& g = m_gesture;
    g.longArmed = m_touch.pressed && !m_scrollDrag.node.valid();
    g.moved = false;
    g.dragOk = false;
    g.held = 0.f;
    g.downTime = t;
}

void UiContext::gestureTick(f32 dt) {
    using Stage = TouchState::Stage;
    GestureState& g = m_gesture;
    if (m_touch.stage == Stage::Idle || m_touch.stage == Stage::Pan || g.moved || m_drag.active) {
        g.longArmed = false;
        return;
    }
    g.held += dt;
    if (g.held < m_gestureCfg.longPressTime) return;
    g.longArmed = false;
    g.moved = true; // a long press is not a tap
    const NodeHandle hit = node(m_touch.hit) ? m_touch.hit : hitTest(m_touch.down);
    if (!node(hit) || (node(m_modal) && !inScope(hit.index, m_modal.index))) return;
    Event e; e.type = EventType::GestureLongPress; e.pos = m_touch.down; e.mods = m_mods;
    if (dispatch(hit.index, e)) { // consumed: the press ends without a Click
        cancelPress(m_touch.down);
        m_touch.pressed = false;
        if (m_touch.stage == Stage::Pending) m_touch.stage = Stage::Passive;
        return;
    }
    if (node(hit)) startDragFrom(hit, m_touch.down); // a drag source under the finger picks its data up
}

void UiContext::gestureTap(Vec2 p, double t) {
    GestureState& g = m_gesture;
    const NodeHandle hit = m_touch.hit;
    if (!node(hit) || (node(m_modal) && !inScope(hit.index, m_modal.index))) return;
    Event e; e.type = EventType::GestureTap; e.pos = p; e.mods = m_mods;
    dispatch(hit.index, e);
    if (t - g.lastTapTime <= double(m_gestureCfg.doubleTapTime) && dist(p, g.lastTapPos) <= m_gestureCfg.doubleTapSlop) {
        g.lastTapTime = -1e9;
        if (node(hit)) {
            e.type = EventType::GestureDoubleTap;
            dispatch(hit.index, e);
        }
    } else {
        g.lastTapTime = t;
        g.lastTapPos = p;
    }
}

void UiContext::gestureEndTwo() {
    GestureState& g = m_gesture;
    if (!g.two) return;
    g.two = false;
    Event e; e.pos = g.centre; e.mods = m_mods; e.gesturePhase = GesturePhase::End; e.scale = g.scale; e.rotation = g.turned;
    if (g.pinching && node(g.target)) { e.type = EventType::GesturePinch; dispatch(g.target.index, e); }
    if (g.rotating && node(g.target)) { e.type = EventType::GestureRotate; dispatch(g.target.index, e); }
    g.pinching = g.rotating = false;
}

void UiContext::gestureReset() {
    gestureEndTwo();
    m_gesture.count = 0;
    m_gesture.spent = false;
    m_gesture.primaryDown = false;
    m_gesture.longArmed = false;
}

// ---- Multi-touch ----------------------------------------------------------------------------------------------------
bool UiContext::touchDown(u32 id, Vec2 p, double t) {
    GestureState& g = m_gesture;
    for (u32 k = 0; k < g.count; ++k)
        if (g.pts[k].id == id) { g.pts[k].pos = p; return true; } // a repeated down: just the new position
    if (g.count == 0) {
        const bool used = touchDown(p, t); // may reset the gesture state (a down without an up)
        g.pts[0] = {id, p};
        g.count = 1;
        g.primary = id;
        g.primaryDown = true;
        g.spent = false;
        return used;
    }
    if (g.count < GestureState::kMaxTouches) g.pts[g.count++] = {id, p};
    if (g.count == 2 && !g.spent) {
        // the second finger takes the first one's press / pan away (no Click, no fling) and starts a gesture
        GestureState::Point saved[2] = {g.pts[0], g.pts[1]};
        touchCancel();
        g.pts[0] = saved[0];
        g.pts[1] = saved[1];
        g.count = 2;
        g.spent = true;
        g.two = true;
        g.pinching = g.rotating = false;
        const Vec2 a = g.pts[0].pos, b = g.pts[1].pos;
        g.d0 = std::max(1.f, dist(a, b));
        g.lastAngle = std::atan2(b.y - a.y, b.x - a.x);
        g.turned = 0.f;
        g.scale = 1.f;
        g.centre = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        NodeHandle h = hitTest(g.centre);
        if (node(m_modal) && h.valid() && !inScope(h.index, m_modal.index)) h = {};
        g.target = h;
    }
    return true;
}

bool UiContext::touchMove(u32 id, Vec2 p, double t) {
    GestureState& g = m_gesture;
    u32 idx = g.count;
    for (u32 k = 0; k < g.count; ++k)
        if (g.pts[k].id == id) idx = k;
    if (idx == g.count) return false;
    g.pts[idx].pos = p;
    if (g.two) {
        if (idx > 1) return true;
        const Vec2 a = g.pts[0].pos, b = g.pts[1].pos;
        const f32 d = dist(a, b), ang = std::atan2(b.y - a.y, b.x - a.x);
        g.centre = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        g.scale = d / g.d0;
        f32 da = ang - g.lastAngle;
        if (da > kPi) da -= 2.f * kPi;
        if (da < -kPi) da += 2.f * kPi;
        g.turned += da;
        g.lastAngle = ang;
        Event e; e.pos = g.centre; e.mods = m_mods; e.scale = g.scale; e.rotation = g.turned;
        const bool pinchBegin = !g.pinching && std::abs(d - g.d0) >= m_gestureCfg.pinchSlop;
        if (pinchBegin) g.pinching = true;
        if (g.pinching && node(g.target)) {
            e.type = EventType::GesturePinch;
            e.gesturePhase = pinchBegin ? GesturePhase::Begin : GesturePhase::Update;
            dispatch(g.target.index, e);
        }
        const bool rotBegin = !g.rotating && std::abs(g.turned) >= m_gestureCfg.rotateSlop;
        if (rotBegin) g.rotating = true;
        if (g.rotating && node(g.target)) {
            e.type = EventType::GestureRotate;
            e.gesturePhase = rotBegin ? GesturePhase::Begin : GesturePhase::Update;
            dispatch(g.target.index, e);
        }
        return true;
    }
    if (g.spent) return true;
    if (id == g.primary && g.primaryDown) return touchMove(p, t);
    return true;
}

bool UiContext::touchUp(u32 id, Vec2 p, double t) {
    GestureState& g = m_gesture;
    u32 idx = g.count;
    for (u32 k = 0; k < g.count; ++k)
        if (g.pts[k].id == id) idx = k;
    if (idx == g.count) return false;
    if (g.two && idx < 2) {
        g.pts[idx].pos = p;
        gestureEndTwo();
    }
    for (u32 k = idx; k + 1 < g.count; ++k) g.pts[k] = g.pts[k + 1];
    --g.count;
    const bool primary = !g.spent && id == g.primary && g.primaryDown;
    if (g.count == 0) g.spent = false;
    if (primary) {
        g.primaryDown = false;
        return touchUp(p, t);
    }
    return true;
}

} // namespace fuse::ui
