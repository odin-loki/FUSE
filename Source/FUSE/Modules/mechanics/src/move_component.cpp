#include <fuse/mechanics/move_component.hpp>

#include <cmath>

namespace fuse::mechanics {

MoveComponent::MoveComponent() : Component("MoveComponent") {}

MoveComponent::MoveComponent(std::string name, f32 speed)
    : Component(std::move(name)), m_speed(speed) {}

void MoveComponent::setTarget(f32 x, f32 y, f32 z) {
    m_targetX = x;
    m_targetY = y;
    m_targetZ = z;
    m_arrived = false;
}

void MoveComponent::advance(f32 dt) {
    ++m_tickCount;
    const f32 dx = m_targetX - m_x;
    const f32 dy = m_targetY - m_y;
    const f32 dz = m_targetZ - m_z;
    const f32 distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance <= 0.0001f) {
        m_arrived = true;
        return;
    }

    const f32 step = m_speed * dt;
    if (step >= distance) {
        m_x = m_targetX;
        m_y = m_targetY;
        m_z = m_targetZ;
        m_arrived = true;
        return;
    }

    const f32 scale = step / distance;
    m_x += dx * scale;
    m_y += dy * scale;
    m_z += dz * scale;
}

} // namespace fuse::mechanics
