#include <fuse/mechanics/physics_trigger_bridge.hpp>

namespace fuse::mechanics {

void PhysicsTriggerBridge::bind(PolyhedronTriggerZone* trigger) {
    m_trigger = trigger;
}

void PhysicsTriggerBridge::setPositionProvider(PhysicsPositionProvider provider) {
    m_provider = std::move(provider);
}

void PhysicsTriggerBridge::syncObject(u32 objectId) {
    if (m_trigger == nullptr || !m_provider) {
        return;
    }

    const PhysicsBodyPosition position = m_provider(objectId);
    m_lastPositions[objectId] = position;
    m_trigger->testObject(objectId, position.x, position.y, position.z);
    ++m_syncCount;
}

void PhysicsTriggerBridge::syncAll() {
    for (const auto& entry : m_lastPositions) {
        syncObject(entry.first);
    }
}

} // namespace fuse::mechanics
