#include <fuse/ai/agent_snapshot.hpp>

#include <cmath>

namespace fuse::ai {

float AgentSnapshot::distanceToTarget() const {
    const float dx = targetX - x;
    const float dy = targetY - y;
    return std::sqrt(dx * dx + dy * dy);
}

bool AgentSnapshot::directionTowardTarget(float& outDx, float& outDy) const {
    const float dx = targetX - x;
    const float dy = targetY - y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < 1e-6f) {
        return false;
    }
    outDx = dx / dist;
    outDy = dy / dist;
    return true;
}

} // namespace fuse::ai
