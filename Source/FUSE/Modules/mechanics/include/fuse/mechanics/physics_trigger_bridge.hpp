#pragma once

// Ore: Engine/source/T3D/trigger.h physics polyhedron bridge (without Bullet SimObject)

#include <fuse/mechanics/polyhedron_trigger.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <unordered_map>

namespace fuse::mechanics {

struct PhysicsBodyPosition {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

using PhysicsPositionProvider = std::function<PhysicsBodyPosition(u32 objectId)>;

/// Bridges physics body positions into PolyhedronTriggerZone enter/leave tests.
class PhysicsTriggerBridge {
public:
    void bind(PolyhedronTriggerZone* trigger);
    void setPositionProvider(PhysicsPositionProvider provider);

    void syncObject(u32 objectId);
    void syncAll();

    u32 syncCount() const { return m_syncCount; }

private:
    PolyhedronTriggerZone* m_trigger = nullptr;
    PhysicsPositionProvider m_provider;
    std::unordered_map<u32, PhysicsBodyPosition> m_lastPositions;
    u32 m_syncCount = 0;
};

} // namespace fuse::mechanics
