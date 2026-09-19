#include <fuse/mechanics/broadphase_trigger_sync.hpp>

#include <cmath>

namespace fuse::mechanics {

s32 BroadphaseTriggerSync::cellKey(float x, float y) const {
    const s32 cellX = static_cast<s32>(std::floor(x / m_cellSize));
    const s32 cellY = static_cast<s32>(std::floor(y / m_cellSize));
    return (cellX << 16) ^ (cellY & 0xFFFF);
}

void BroadphaseTriggerSync::insertBodyCell(u32 objectId, s32 key) {
    m_cells[key].push_back(objectId);
}

void BroadphaseTriggerSync::trackBody(u32 objectId, float x, float y, float z) {
    m_bodies[objectId] = {x, y, z};
}

void BroadphaseTriggerSync::bindTrigger(PolyhedronTriggerZone* trigger) {
    m_trigger = trigger;
}

void BroadphaseTriggerSync::setPositionProvider(PhysicsPositionProvider provider) {
    m_provider = std::move(provider);
}

void BroadphaseTriggerSync::testBodyAgainstTrigger(u32 objectId, const BodyState& body) {
    if (m_trigger == nullptr) {
        return;
    }

    ++m_candidateCount;
    m_trigger->testObject(objectId, body.x, body.y, body.z);
}

void BroadphaseTriggerSync::syncAll() {
    m_cells.clear();
    m_candidateCount = 0;

    if (m_provider) {
        for (auto& entry : m_bodies) {
            const PhysicsBodyPosition position = m_provider(entry.first);
            entry.second = {position.x, position.y, position.z};
        }
    }

    for (const auto& entry : m_bodies) {
        insertBodyCell(entry.first, cellKey(entry.second.x, entry.second.y));
    }

    if (m_trigger == nullptr) {
        return;
    }

    for (const auto& cellEntry : m_cells) {
        for (u32 objectId : cellEntry.second) {
            const auto bodyIt = m_bodies.find(objectId);
            if (bodyIt == m_bodies.end()) {
                continue;
            }
            testBodyAgainstTrigger(objectId, bodyIt->second);
        }
    }

    ++m_syncCount;
}

} // namespace fuse::mechanics
