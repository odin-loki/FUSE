#include <fuse/mechanics/area_component.hpp>

#include <cmath>

namespace fuse::mechanics {

AreaComponent::AreaComponent() : Component("area") {}

AreaComponent::AreaComponent(std::string name, float radius) : Component(std::move(name)), m_radius(radius) {}

bool AreaComponent::contains(float x, float y, float z) const {
    const float distanceSq = x * x + y * y + z * z;
    return distanceSq <= m_radius * m_radius;
}

bool AreaComponent::testObject(u32 objectId, float x, float y, float z) {
    const bool inside = contains(x, y, z);
    if (inside && (!m_objectInside || m_trackedObjectId != objectId)) {
        m_trackedObjectId = objectId;
        m_objectInside = true;
        ++m_enterCount;
        return true;
    }
    if (!inside && m_objectInside && m_trackedObjectId == objectId) {
        m_objectInside = false;
        ++m_leaveCount;
    }
    return inside;
}

} // namespace fuse::mechanics
