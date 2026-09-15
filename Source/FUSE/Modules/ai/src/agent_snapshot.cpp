#include <fuse/ai/agent_snapshot.hpp>

#include <cmath>

namespace fuse::ai {

float AgentSnapshot::distanceToTarget() const {
    const float dx = targetX - x;
    const float dy = targetY - y;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace fuse::ai
