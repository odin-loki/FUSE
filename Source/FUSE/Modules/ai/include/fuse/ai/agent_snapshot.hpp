#pragma once

#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

namespace fuse::ai {

/// Read-only agent state for parallel BT evaluation — no scene graph pointers.
struct AgentSnapshot {
    Handle<Object> agent = Handle<Object>::invalid();
    float x = 0.f;
    float y = 0.f;
    float targetX = 0.f;
    float targetY = 0.f;
    float moveSpeed = 1.f;
    u32 teamId = 0;
    /// Height for 3D faction queries (hostile leaves); XY leaves ignore it.
    float z = 0.f;
    /// `FactionQuery` masks this agent applies when looking for hostiles (see `AgentBinding`).
    u32 friendlyMask = 0;
    u32 hostileMask = 0;

    float distanceToTarget() const;

    bool directionTowardTarget(float& outDx, float& outDy) const;
};

} // namespace fuse::ai
