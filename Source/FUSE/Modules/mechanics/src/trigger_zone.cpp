#include <fuse/mechanics/trigger_zone.hpp>

namespace fuse::mechanics {

bool AxisAlignedBox::contains(float x, float y, float z) const {
    return x >= minX && x <= maxX && y >= minY && y <= maxY && z >= minZ && z <= maxZ;
}

TriggerZoneComponent::TriggerZoneComponent() = default;

TriggerZoneComponent::TriggerZoneComponent(std::string name, AxisAlignedBox bounds)
    : Component(std::move(name)), m_bounds(bounds) {}

void TriggerZoneComponent::testObject(u32 objectId, float x, float y, float z) {
    const bool inside = m_bounds.contains(x, y, z);
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

void TriggerZoneComponent::advance(u32 elapsedMs) {
    if (m_tickPeriodMs == 0) {
        return;
    }

    if (m_lastTickMs == 0) {
        m_lastTickMs = elapsedMs;
        return;
    }

    if (elapsedMs - m_lastTickMs >= m_tickPeriodMs) {
        m_lastTickMs = elapsedMs;
        ++m_tickCount;
        if (m_onTick) {
            m_onTick();
        }
    }
}

} // namespace fuse::mechanics
