#include <fuse/mechanics/look_at_component.hpp>

#include <cmath>

namespace fuse::mechanics {

namespace {

f32 normalizeAngleDeg(f32 angle) {
    while (angle > 180.f) {
        angle -= 360.f;
    }
    while (angle < -180.f) {
        angle += 360.f;
    }
    return angle;
}

} // namespace

LookAtComponent::LookAtComponent() : Component("look_at") {}

LookAtComponent::LookAtComponent(std::string name, f32 turnSpeedDeg)
    : Component(std::move(name)), m_turnSpeedDeg(turnSpeedDeg) {}

void LookAtComponent::setTarget(f32 targetX, f32 targetY) {
    m_targetX = targetX;
    m_targetY = targetY;
    m_aligned = false;
}

void LookAtComponent::advanceTowardTarget(f32 x, f32 y, f32 dt) {
    const f32 dx = m_targetX - x;
    const f32 dy = m_targetY - y;
    const f32 desiredYaw = std::atan2(dy, dx) * 180.f / 3.14159265f;
    const f32 delta = normalizeAngleDeg(desiredYaw - m_yawDeg);
    const f32 step = m_turnSpeedDeg * dt;
    if (std::fabs(delta) <= step) {
        m_yawDeg = desiredYaw;
        m_aligned = true;
    } else {
        m_yawDeg += (delta > 0.f ? step : -step);
        m_aligned = false;
    }
    ++m_tickCount;
}

} // namespace fuse::mechanics
