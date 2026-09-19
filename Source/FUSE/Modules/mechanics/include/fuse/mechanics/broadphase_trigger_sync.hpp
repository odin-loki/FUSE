#pragma once

// Ore: Bullet broadphase → T3D Trigger polyhedron sync (without SimObject)

#include <fuse/mechanics/polyhedron_trigger.hpp>
#include <fuse/mechanics/physics_trigger_bridge.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <unordered_map>
#include <vector>

namespace fuse::mechanics {

/// Spatial-hash broadphase stub that batches body→trigger overlap tests.
class BroadphaseTriggerSync {
public:
    void setCellSize(float cellSize) { m_cellSize = cellSize > 0.f ? cellSize : 2.f; }
    float cellSize() const { return m_cellSize; }

    void trackBody(u32 objectId, float x, float y, float z);
    void bindTrigger(PolyhedronTriggerZone* trigger);
    void setPositionProvider(PhysicsPositionProvider provider);

    void syncAll();
    u32 syncCount() const { return m_syncCount; }
    u32 candidateCount() const { return m_candidateCount; }
    u32 neighborCandidateCount() const { return m_neighborCandidateCount; }

private:
    struct BodyState {
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
    };

    s32 cellKey(float x, float y) const;
    void insertBodyCell(u32 objectId, s32 key);
    void testBodyAgainstTrigger(u32 objectId, const BodyState& body);
    void testNeighborCells(u32 objectId, const BodyState& body);

    float m_cellSize = 2.f;
    PolyhedronTriggerZone* m_trigger = nullptr;
    PhysicsPositionProvider m_provider;
    std::unordered_map<u32, BodyState> m_bodies;
    std::unordered_map<s32, std::vector<u32>> m_cells;
    u32 m_syncCount = 0;
    u32 m_candidateCount = 0;
    u32 m_neighborCandidateCount = 0;
};

} // namespace fuse::mechanics
