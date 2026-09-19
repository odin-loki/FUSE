#include <fuse/mechanics/follow_component.hpp>

#include <cmath>

namespace fuse::mechanics {

FollowComponent::FollowComponent() : Component("FollowComponent") {}

FollowComponent::FollowComponent(std::string name, f32 speed)
    : Component(std::move(name)), m_speed(speed) {}

void FollowComponent::setPosition(f32 x, f32 y, f32 z) {
    m_x = x;
    m_y = y;
    m_z = z;
}

void FollowComponent::advanceToward(f32 targetX, f32 targetY, f32 targetZ, f32 dt) {
    const f32 dx = targetX - m_x;
    const f32 dy = targetY - m_y;
    const f32 dz = targetZ - m_z;
    const f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist <= 0.0001f) {
        m_arrived = true;
        ++m_tickCount;
        return;
    }

    const f32 step = m_speed * dt;
    if (step >= dist) {
        m_x = targetX;
        m_y = targetY;
        m_z = targetZ;
        m_arrived = true;
    } else {
        const f32 scale = step / dist;
        m_x += dx * scale;
        m_y += dy * scale;
        m_z += dz * scale;
        m_arrived = false;
    }
    ++m_tickCount;
}

} // namespace fuse::mechanics
