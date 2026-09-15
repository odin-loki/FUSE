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
    u32 teamId = 0;

    float distanceToTarget() const;
};

} // namespace fuse::ai
