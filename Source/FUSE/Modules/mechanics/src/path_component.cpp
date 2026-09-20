#include <fuse/mechanics/path_component.hpp>

#include <cmath>

namespace fuse::mechanics {

PathComponent::PathComponent() : Component("PathComponent") {}

PathComponent::PathComponent(std::string name) : Component(std::move(name)) {}

void PathComponent::addWaypoint(f32 x, f32 y, f32 z) {
    m_waypoints.push_back({x, y, z});
}

void PathComponent::setPosition(f32 x, f32 y, f32 z) {
    m_x = x;
    m_y = y;
    m_z = z;
}

void PathComponent::advanceAlongPath(f32 speed, f32 dt) {
    if (m_waypoints.empty() || m_pathIndex >= m_waypoints.size()) {
        m_finished = true;
        ++m_tickCount;
        return;
    }

    const PathWaypoint& target = m_waypoints[m_pathIndex];
    const f32 dx = target.x - m_x;
    const f32 dy = target.y - m_y;
    const f32 dz = target.z - m_z;
    const f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist <= 0.0001f) {
        ++m_pathIndex;
        if (m_pathIndex >= m_waypoints.size()) {
            if (m_loop && !m_waypoints.empty()) {
                m_pathIndex = 0;
                ++m_loopCount;
                m_finished = false;
            } else {
                m_finished = true;
            }
        }
        ++m_tickCount;
        return;
    }

    const f32 step = speed * dt;
    if (step >= dist) {
        m_x = target.x;
        m_y = target.y;
        m_z = target.z;
        ++m_pathIndex;
        if (m_pathIndex >= m_waypoints.size()) {
            if (m_loop && !m_waypoints.empty()) {
                m_pathIndex = 0;
                ++m_loopCount;
                m_finished = false;
            } else {
                m_finished = true;
            }
        }
    } else {
        const f32 scale = step / dist;
        m_x += dx * scale;
        m_y += dy * scale;
        m_z += dz * scale;
        m_finished = false;
    }
    ++m_tickCount;
}

} // namespace fuse::mechanics
