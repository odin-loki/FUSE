#include "fuse/ui/extras/gesture.hpp"

#include <algorithm>
#include <cmath>

namespace fuse::ui {
namespace {
f32 dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }
Vec2 sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
} // namespace

GestureRecognizer::Pt* GestureRecognizer::find(std::uint32_t id) {
    for (Pt& p : m_pts) if (p.id == id) return &p;
    return nullptr;
}
Vec2 GestureRecognizer::centroid() const {
    Vec2 c;
    for (const Pt& p : m_pts) { c.x += p.pos.x; c.y += p.pos.y; }
    const f32 n = f32(std::max<std::size_t>(1, m_pts.size()));
    return {c.x / n, c.y / n};
}
void GestureRecognizer::reset() { m_pts.clear(); m_mode = Mode::Idle; m_maxFingers = 0; }

std::vector<Gesture> GestureRecognizer::tick(double time) {
    std::vector<Gesture> out;
    if (m_mode == Mode::Pending && m_pts.size() == 1 && time - m_downTime >= m_cfg.longPressSeconds) {
        m_mode = Mode::Held;
        out.push_back({.type = GestureType::LongPress, .state = GestureState::Started, .pos = m_pts[0].pos});
    }
    return out;
}

std::vector<Gesture> GestureRecognizer::feed(const TouchPoint& tp, double time) {
    std::vector<Gesture> out = tick(time);
    auto beginPinch = [&] {
        const Pt &a = m_pts[0], &b = m_pts[1];
        m_pinchDist0 = std::max(1.f, dist(a.pos, b.pos));
        m_pinchAngle0 = std::atan2(b.pos.y - a.pos.y, b.pos.x - a.pos.x);
        if (m_mode == Mode::Pan) out.push_back({.type = GestureType::Pan, .state = GestureState::Canceled, .pos = centroid()});
        m_mode = Mode::Pinch;
        out.push_back({.type = GestureType::Pinch, .state = GestureState::Started, .pos = centroid(), .fingers = 2});
    };
    switch (tp.phase) {
    case TouchPhase::Down:
        m_pts.push_back({tp.id, tp.pos, tp.pos, tp.pos, time});
        m_maxFingers = std::max(m_maxFingers, int(m_pts.size()));
        if (m_pts.size() == 1) { m_mode = Mode::Pending; m_downTime = time; m_lastMoveTime = time; m_velocity = {}; }
        else if (m_pts.size() == 2 && m_mode != Mode::Held) beginPinch();
        break;
    case TouchPhase::Move: {
        Pt* p = find(tp.id);
        if (!p) break;
        p->prev = p->pos;
        p->pos = tp.pos;
        const double dt = std::max(1e-4, time - m_lastMoveTime);
        if (m_mode == Mode::Pending && dist(p->pos, p->start) > m_cfg.tapSlop) {
            m_mode = Mode::Pan;
            m_panStart = p->start;
            m_panLast = p->start;
            out.push_back({.type = GestureType::Pan, .state = GestureState::Started, .pos = p->start, .fingers = int(m_pts.size())});
        }
        if (m_mode == Mode::Pan) {
            const Vec2 c = centroid(), d = sub(c, m_panLast);
            const Vec2 v{f32(d.x / dt), f32(d.y / dt)};
            m_velocity = {m_velocity.x * 0.3f + v.x * 0.7f, m_velocity.y * 0.3f + v.y * 0.7f};
            m_panLast = c;
            out.push_back({.type = GestureType::Pan, .state = GestureState::Updated, .pos = c, .delta = d, .offset = sub(c, m_panStart),
                           .velocity = m_velocity, .fingers = int(m_pts.size())});
        } else if (m_mode == Mode::Pinch && m_pts.size() >= 2) {
            const Pt &a = m_pts[0], &b = m_pts[1];
            f32 rot = std::atan2(b.pos.y - a.pos.y, b.pos.x - a.pos.x) - m_pinchAngle0;
            while (rot > 3.14159265f) rot -= 6.2831853f;
            while (rot < -3.14159265f) rot += 6.2831853f;
            out.push_back({.type = GestureType::Pinch, .state = GestureState::Updated, .pos = centroid(),
                           .scale = dist(a.pos, b.pos) / m_pinchDist0, .rotation = rot, .fingers = 2});
        }
        m_lastMoveTime = time;
        break;
    }
    case TouchPhase::Up:
    case TouchPhase::Cancel: {
        Pt* p = find(tp.id);
        if (!p) break;
        p->pos = tp.pos;
        const bool cancel = tp.phase == TouchPhase::Cancel;
        const Pt done = *p;
        if (m_mode == Mode::Pinch) {
            out.push_back({.type = GestureType::Pinch, .state = cancel ? GestureState::Canceled : GestureState::Finished, .pos = centroid(), .fingers = 2});
            m_mode = Mode::Idle; // remaining finger does not start a pan until lifted
        } else if (m_mode == Mode::Pan) {
            const Vec2 off = sub(centroid(), m_panStart);
            out.push_back({.type = GestureType::Pan, .state = cancel ? GestureState::Canceled : GestureState::Finished, .pos = centroid(),
                           .offset = off, .velocity = m_velocity, .fingers = m_maxFingers});
            const f32 speed = std::hypot(m_velocity.x, m_velocity.y), len = std::hypot(off.x, off.y);
            if (!cancel && speed >= m_cfg.swipeMinVelocity && len >= m_cfg.swipeMinDistance) {
                SwipeDirection d = std::abs(off.x) > std::abs(off.y) ? (off.x < 0 ? SwipeDirection::Left : SwipeDirection::Right)
                                                                    : (off.y < 0 ? SwipeDirection::Up : SwipeDirection::Down);
                out.push_back({.type = GestureType::Swipe, .state = GestureState::Finished, .pos = done.pos, .offset = off,
                               .velocity = m_velocity, .direction = d, .fingers = m_maxFingers});
            }
            m_mode = Mode::Idle;
        } else if (m_mode == Mode::Held) {
            out.push_back({.type = GestureType::LongPress, .state = cancel ? GestureState::Canceled : GestureState::Finished, .pos = done.pos});
            m_mode = Mode::Idle;
        } else if (m_mode == Mode::Pending && !cancel && m_pts.size() == 1 && time - m_downTime <= m_cfg.tapMaxSeconds) {
            out.push_back({.type = GestureType::Tap, .pos = done.pos, .fingers = m_maxFingers});
            if (time - m_lastTapTime <= m_cfg.doubleTapSeconds && dist(done.pos, m_lastTapPos) <= m_cfg.doubleTapSlop) {
                out.push_back({.type = GestureType::DoubleTap, .pos = done.pos});
                m_lastTapTime = -1e9;
            } else {
                m_lastTapTime = time;
                m_lastTapPos = done.pos;
            }
            m_mode = Mode::Idle;
        }
        std::erase_if(m_pts, [&](const Pt& q) { return q.id == tp.id; });
        if (m_pts.empty()) { m_mode = Mode::Idle; m_maxFingers = 0; }
        break;
    }
    }
    return out;
}

} // namespace fuse::ui
