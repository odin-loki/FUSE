#include <fuse/mechanics/physics_broadphase_bridge.hpp>

namespace fuse::mechanics {

void PhysicsBroadphaseBridge::bindTrigger(PolyhedronTriggerZone* trigger) {
    m_broadphase.bindTrigger(trigger);
    m_physics.bind(trigger);
}

void PhysicsBroadphaseBridge::setPositionProvider(PhysicsPositionProvider provider) {
    m_broadphase.setPositionProvider(provider);
    m_physics.setPositionProvider(provider);
}

void PhysicsBroadphaseBridge::trackBody(u32 objectId, float x, float y, float z) {
    m_broadphase.trackBody(objectId, x, y, z);
}

void PhysicsBroadphaseBridge::syncBody(u32 objectId) {
    m_broadphase.syncAll();
    m_physics.syncObject(objectId);
    ++m_syncCount;
}

void PhysicsBroadphaseBridge::syncAll() {
    m_broadphase.syncAll();
    m_physics.syncAll();
    ++m_syncCount;
}

} // namespace fuse::mechanics
