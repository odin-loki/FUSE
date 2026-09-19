#pragma once

#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <vector>

namespace fuse::ai {

class BehaviorRuntime;

/// Resolves agent world positions from entity handles (picker / scene bind ore).
using AgentPositionProvider = std::function<bool(Handle<Object> entity, float& outX, float& outY)>;

struct AgentEntityBinding {
    u32 agentIndex = 0;
    Handle<Object> entity = Handle<Object>::invalid();
};

/// Apply editor/PIE entity bindings onto a `BehaviorRuntime` (game thread).
void wireAgentEntityBindings(BehaviorRuntime& runtime, const std::vector<AgentEntityBinding>& bindings);

} // namespace fuse::ai
