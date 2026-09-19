#pragma once

// Ore: Bullet broadphase → T3D Trigger polyhedron pipeline (without SimObject)

#include <fuse/mechanics/broadphase_trigger_sync.hpp>
#include <fuse/mechanics/physics_trigger_bridge.hpp>
#include <fuse/types.hpp>

namespace fuse::mechanics {

/// Combines spatial-hash broadphase with direct physics body sync into one trigger pipeline.
class PhysicsBroadphaseBridge {
public:
    void bindTrigger(PolyhedronTriggerZone* trigger);
    void setPositionProvider(PhysicsPositionProvider provider);
    void setCellSize(float cellSize) { m_broadphase.setCellSize(cellSize); }

    void trackBody(u32 objectId, float x, float y, float z);
    void syncBody(u32 objectId);
    void syncAll();

    u32 syncCount() const { return m_syncCount; }
    u32 broadphaseSyncCount() const { return m_broadphase.syncCount(); }
    u32 physicsSyncCount() const { return m_physics.syncCount(); }
    u32 neighborCandidateCount() const { return m_broadphase.neighborCandidateCount(); }

private:
    BroadphaseTriggerSync m_broadphase;
    PhysicsTriggerBridge m_physics;
    u32 m_syncCount = 0;
};

} // namespace fuse::mechanics
