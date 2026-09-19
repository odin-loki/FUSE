#include <fuse/ai/agent_entity_bind.hpp>

#include <fuse/ai/behavior_runtime.hpp>

namespace fuse::ai {

void wireAgentEntityBindings(BehaviorRuntime& runtime, const std::vector<AgentEntityBinding>& bindings) {
    for (const AgentEntityBinding& binding : bindings) {
        runtime.setAgentEntity(binding.agentIndex, binding.entity);
    }
}

} // namespace fuse::ai
