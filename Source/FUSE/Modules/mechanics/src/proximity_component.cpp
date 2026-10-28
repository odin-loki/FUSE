#include <fuse/mechanics/proximity_component.hpp>

namespace fuse::mechanics {

ProximityComponent::ProximityComponent() = default;

ProximityComponent::ProximityComponent(std::string name, f32 radius)
    : Component(std::move(name)) {
    setRadius(radius);
}

bool ProximityComponent::containsPoint(f32 x, f32 y, f32 z) const {
    const f32 dx = x - m_centerX;
    const f32 dy = y - m_centerY;
    const f32 dz = z - m_centerZ;
    const f32 radiusSq = m_radius * m_radius;
    return dx * dx + dy * dy + dz * dz <= radiusSq;
}

void ProximityComponent::testObject(u32 objectId, f32 x, f32 y, f32 z) {
    const bool inside = containsPoint(x, y, z);
    const bool wasInside = m_inside.find(objectId) != m_inside.end();

    if (inside && !wasInside) {
        m_inside.insert(objectId);
        ++m_enterCount;
        if (m_onEnter) {
            m_onEnter(objectId);
        }
    } else if (!inside && wasInside) {
        m_inside.erase(objectId);
        ++m_leaveCount;
        if (m_onLeave) {
            m_onLeave(objectId);
        }
    }
}

} // namespace fuse::mechanics
