#include <fuse/ai/parallel_policy.hpp>
#include <fuse/ai/spatial_query.hpp>

namespace fuse::ai {

namespace {

u32 clamp_threshold_(u32 value, u32 minValue, u32 maxValue) {
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

} // namespace

u32 effective_success_threshold(const ParallelPolicy& policy, u32 activeChildCount) {
    if (policy.successThreshold > 0) {
        return policy.successThreshold;
    }
    return activeChildCount > 0 ? activeChildCount : 1u;
}

u32 effective_fail_threshold(const ParallelPolicy& policy) {
    return policy.failThreshold > 0 ? policy.failThreshold : 1u;
}

bool parallel_preconditions_satisfied(const ParallelPolicy& policy,
                                      u32 agentIndex,
                                      const BlackboardView& board,
                                      const BehaviorEvalContext& ctx) {
    if (policy.requireBoundBlackboard && !board.isBound()) {
        return false;
    }
    if (policy.requireAllyContext && !ally_context_available(ctx.allies)) {
        return false;
    }
    if (policy.requireValidAgent && !board.isAgentValid(agentIndex)) {
        return false;
    }
    return true;
}

bool is_parallel_policy_valid(const ParallelPolicy& policy, u32 activeChildCount) {
    if (activeChildCount == 0) {
        return false;
    }
    const u32 successNeeded = effective_success_threshold(policy, activeChildCount);
    const u32 failLimit = effective_fail_threshold(policy);
    return successNeeded <= activeChildCount && failLimit <= activeChildCount;
}

ParallelPolicy normalize_parallel_policy(const ParallelPolicy& policy, u32 activeChildCount) {
    if (activeChildCount == 0) {
        return policy;
    }

    ParallelPolicy normalized = policy;
    if (normalized.successThreshold > 0) {
        normalized.successThreshold =
            clamp_threshold_(normalized.successThreshold, 1u, activeChildCount);
    }
    if (normalized.failThreshold > 0) {
        normalized.failThreshold = clamp_threshold_(normalized.failThreshold, 1u, activeChildCount);
    }
    return normalized;
}

} // namespace fuse::ai
