#include <fuse/ai/behavior_tree.hpp>
#include <fuse/ai/node_registry.hpp>
#include <fuse/ai/spatial_query.hpp>

namespace fuse::ai {

namespace {

constexpr u32 kParallelChildCount = 2;

BehaviorTickResult mergeParallelSideEffects(const BehaviorTickResult& current,
                                            const BehaviorTickResult& child) {
    BehaviorTickResult merged = current;
    if (child.wroteFlag) {
        merged.wroteFlag = true;
        merged.flagIndex = child.flagIndex;
        merged.flagValue = child.flagValue;
    }
    if (child.wroteScalar) {
        merged.wroteScalar = true;
        merged.scalarIndex = child.scalarIndex;
        merged.scalarValue = child.scalarValue;
    }
    return merged;
}

BehaviorTickResult aggregateParallelChildren(const BehaviorTickResult& first,
                                             const BehaviorTickResult& second,
                                             const ParallelPolicy& policy,
                                             bool secondTicked) {
    const u32 activeChildCount = secondTicked ? kParallelChildCount : 1u;
    const u32 successNeeded =
        policy.successThreshold > 0 ? policy.successThreshold : activeChildCount;
    const u32 failLimit = policy.failThreshold > 0 ? policy.failThreshold : 1u;

    u32 successCount = 0;
    u32 failCount = 0;
    u32 runningCount = 0;
    BehaviorTickResult merged;

    const BehaviorTickResult* children[2] = {&first, &second};
    for (u32 childIndex = 0; childIndex < activeChildCount; ++childIndex) {
        const BehaviorTickResult& child = *children[childIndex];
        if (child.status == BehaviorStatus::Success) {
            ++successCount;
        } else if (child.status == BehaviorStatus::Failure) {
            ++failCount;
        } else {
            ++runningCount;
        }
        merged = mergeParallelSideEffects(merged, child);
    }

    BehaviorTickResult result = merged;
    if (runningCount > 0) {
        result.status = BehaviorStatus::Running;
        return result;
    }
    if (failCount >= failLimit) {
        result.status = BehaviorStatus::Failure;
        return result;
    }
    if (successCount >= successNeeded) {
        result.status = BehaviorStatus::Success;
        return result;
    }
    result.status = BehaviorStatus::Failure;
    return result;
}

u32 waitTicksForNode(const BehaviorNode& node) {
    if (node.loopCount > 0) {
        return node.loopCount;
    }
    return node.threshold > 0.f ? static_cast<u32>(node.threshold) : 1u;
}

bool parallelRadiusGuardPasses(const BehaviorNode& node, const ParallelPolicy& policy) {
    if (!policy.requireValidRadius) {
        return true;
    }
    return is_finite_ally_radius(node.threshold);
}

} // namespace

bool parallel_policy_has_guards(const ParallelPolicy& policy) {
    return policy.requireBoundBlackboard || policy.requireAllyContext || policy.requireValidAgent ||
           policy.requireNonEmptyBoard || policy.requireValidRadius;
}

bool parallel_policy_is_valid(const ParallelPolicy& policy, u32 childCount) {
    if (childCount == 0) {
        return false;
    }
    if (policy.successThreshold > childCount) {
        return false;
    }
    return true;
}

void BehaviorTree::addNode(BehaviorNode node) {
    m_nodes.push_back(std::move(node));
}

void BehaviorTree::setRoot(u32 nodeIndex) {
    m_root = nodeIndex;
}

BehaviorTickResult BehaviorTree::tickNode(u32 nodeIndex,
                                          u32 agentIndex,
                                          const AgentSnapshot& agent,
                                          const BlackboardView& board,
                                          const BehaviorEvalContext& ctx) const {
    if (nodeIndex >= m_nodes.size()) {
        return {};
    }

    const BehaviorNode& node = m_nodes[nodeIndex];
    switch (node.kind) {
    case NodeKind::Sequence: {
        BehaviorTickResult first = tickNode(node.childA, agentIndex, agent, board, ctx);
        if (first.status != BehaviorStatus::Success) {
            return first;
        }
        return tickNode(node.childB, agentIndex, agent, board, ctx);
    }
    case NodeKind::Selector: {
        BehaviorTickResult first = tickNode(node.childA, agentIndex, agent, board, ctx);
        if (first.status == BehaviorStatus::Success) {
            return first;
        }
        return tickNode(node.childB, agentIndex, agent, board, ctx);
    }
    case NodeKind::Parallel: {
        const ParallelPolicy& policy = node.parallelPolicy;
        if (policy.requireBoundBlackboard && !board.isBound()) {
            return {};
        }
        if (policy.requireNonEmptyBoard && board.isBoardEmpty()) {
            return {};
        }
        if (policy.requireValidAgent && !board.isAgentValid(agentIndex)) {
            return {};
        }
        if (policy.requireAllyContext && !ally_context_available(ctx.allies)) {
            return {};
        }
        if (!parallelRadiusGuardPasses(node, policy)) {
            return {};
        }

        const u32 failLimit = policy.failThreshold > 0 ? policy.failThreshold : 1u;
        const u32 successNeeded =
            policy.successThreshold > 0 ? policy.successThreshold : kParallelChildCount;

        const BehaviorTickResult first = tickNode(node.childA, agentIndex, agent, board, ctx);
        const u32 failCountAfterFirst =
            first.status == BehaviorStatus::Failure ? 1u : 0u;
        const u32 successCountAfterFirst =
            first.status == BehaviorStatus::Success ? 1u : 0u;
        const bool tickSecond =
            (!policy.abortOnFail || failCountAfterFirst < failLimit) &&
            (!policy.abortOnSuccess || successCountAfterFirst < successNeeded);

        BehaviorTickResult second;
        if (tickSecond) {
            second = tickNode(node.childB, agentIndex, agent, board, ctx);
        }
        return aggregateParallelChildren(first, second, policy, tickSecond);
    }
    case NodeKind::Inverter: {
        BehaviorTickResult child = tickNode(node.childA, agentIndex, agent, board, ctx);
        if (child.status == BehaviorStatus::Success) {
            child.status = BehaviorStatus::Failure;
        } else if (child.status == BehaviorStatus::Failure) {
            child.status = BehaviorStatus::Success;
        }
        return child;
    }
    case NodeKind::Loop: {
        const u32 iterations = node.loopCount > 0 ? node.loopCount : 1;
        BehaviorTickResult lastChild;
        for (u32 i = 0; i < iterations; ++i) {
            lastChild = tickNode(node.childA, agentIndex, agent, board, ctx);
            if (lastChild.status != BehaviorStatus::Success) {
                return lastChild;
            }
        }
        return lastChild;
    }
    case NodeKind::SucceedAlways: {
        BehaviorTickResult child = tickNode(node.childA, agentIndex, agent, board, ctx);
        if (child.status == BehaviorStatus::Running) {
            return child;
        }
        child.status = BehaviorStatus::Success;
        return child;
    }
    case NodeKind::Root:
        return tickNode(node.childA, agentIndex, agent, board, ctx);
    case NodeKind::ConditionDistanceLess: {
        BehaviorTickResult result;
        result.status = agent.distanceToTarget() < node.threshold ? BehaviorStatus::Success
                                                                  : BehaviorStatus::Failure;
        return result;
    }
    case NodeKind::ConditionDistanceGreater: {
        BehaviorTickResult result;
        result.status = agent.distanceToTarget() > node.threshold ? BehaviorStatus::Success
                                                                  : BehaviorStatus::Failure;
        return result;
    }
    case NodeKind::ConditionBlackboardGet: {
        BehaviorTickResult result;
        result.status = board.flag(agentIndex, node.flagIndex) ? BehaviorStatus::Success
                                                               : BehaviorStatus::Failure;
        return result;
    }
    case NodeKind::ActionSetFlag: {
        BehaviorTickResult result;
        result.status = BehaviorStatus::Success;
        result.wroteFlag = true;
        result.flagIndex = node.flagIndex;
        result.flagValue = true;
        return result;
    }
    case NodeKind::ActionBlackboardSet: {
        BehaviorTickResult result;
        result.status = BehaviorStatus::Success;
        result.wroteFlag = true;
        result.flagIndex = node.flagIndex;
        result.flagValue = node.threshold > 0.5f;
        return result;
    }
    case NodeKind::ActionWait: {
        BehaviorTickResult result;
        const u32 duration = waitTicksForNode(node);
        if (!ctx.waitStartTicks) {
            result.status = BehaviorStatus::Success;
            return result;
        }

        u32& startTick = ctx.waitStartTicks[nodeIndex];
        if (startTick == 0) {
            startTick = ctx.tickCount + 1;
            result.status = BehaviorStatus::Running;
            return result;
        }

        if ((ctx.tickCount + 1) - startTick < duration) {
            result.status = BehaviorStatus::Running;
            return result;
        }

        startTick = 0;
        result.status = BehaviorStatus::Success;
        return result;
    }
    case NodeKind::ActionDistance: {
        BehaviorTickResult result;
        const float distance = agent.distanceToTarget();
        result.status = BehaviorStatus::Success;
        result.wroteFlag = true;
        result.flagIndex = node.flagIndex;
        result.flagValue = distance <= node.threshold;
        return result;
    }
    case NodeKind::ActionMoveToward: {
        BehaviorTickResult result;
        const float distance = agent.distanceToTarget();
        result.status = distance > node.threshold ? BehaviorStatus::Success : BehaviorStatus::Failure;
        if (result.status == BehaviorStatus::Success) {
            result.wroteFlag = true;
            result.flagIndex = node.flagIndex;
            result.flagValue = true;
        }
        return result;
    }
    case NodeKind::ConditionAlliesInRadius: {
        BehaviorTickResult result;
        if (!ally_context_available(ctx.allies)) {
            result.status = BehaviorStatus::Failure;
            return result;
        }
        if (!is_finite_ally_radius(node.threshold)) {
            result.status = BehaviorStatus::Failure;
            return result;
        }

        RadiusFilterPolicy policy;
        policy.radius = node.threshold;
        policy.minCount = node.loopCount > 0 ? node.loopCount : 1u;

        const bool satisfied = allies_in_radius_satisfied(agentIndex,
                                                        agent.teamId,
                                                        agent.x,
                                                        agent.y,
                                                        policy,
                                                        *ctx.allies);
        result.status = satisfied ? BehaviorStatus::Success : BehaviorStatus::Failure;
        return result;
    }
    case NodeKind::ConditionAnyAllyInRadius: {
        BehaviorTickResult result;
        if (!ally_context_available(ctx.allies)) {
            result.status = BehaviorStatus::Failure;
            return result;
        }
        if (!is_finite_ally_radius(node.threshold)) {
            result.status = BehaviorStatus::Failure;
            return result;
        }

        const bool found = has_any_ally_in_radius(agentIndex,
                                                  agent.teamId,
                                                  agent.x,
                                                  agent.y,
                                                  node.threshold,
                                                  *ctx.allies);
        result.status = found ? BehaviorStatus::Success : BehaviorStatus::Failure;
        return result;
    }
    case NodeKind::ConditionNoAlliesInRadius: {
        BehaviorTickResult result;
        if (!ally_context_available(ctx.allies)) {
            result.status = BehaviorStatus::Failure;
            return result;
        }
        if (!is_finite_ally_radius(node.threshold)) {
            result.status = BehaviorStatus::Failure;
            return result;
        }

        const bool clear = has_no_allies_in_radius(agentIndex,
                                                   agent.teamId,
                                                   agent.x,
                                                   agent.y,
                                                   node.threshold,
                                                   *ctx.allies);
        result.status = clear ? BehaviorStatus::Success : BehaviorStatus::Failure;
        return result;
    }
    case NodeKind::ActionAlliesCount: {
        BehaviorTickResult result;
        if (!ally_context_available(ctx.allies)) {
            result.status = BehaviorStatus::Failure;
            return result;
        }
        if (!is_finite_ally_radius(node.threshold)) {
            result.status = BehaviorStatus::Failure;
            return result;
        }

        const u32 count = count_allies_in_radius(agentIndex,
                                                 agent.teamId,
                                                 agent.x,
                                                 agent.y,
                                                 node.threshold,
                                                 *ctx.allies);
        result.status = BehaviorStatus::Success;
        if (node.scalarSlot < Blackboard::kMaxScalars) {
            result.wroteScalar = true;
            result.scalarIndex = node.scalarSlot;
            result.scalarValue = static_cast<float>(count);
        }
        if (node.flagIndex < Blackboard::kMaxFlags) {
            result.wroteFlag = true;
            result.flagIndex = node.flagIndex;
            result.flagValue = count > 0;
        }
        return result;
    }
    case NodeKind::ActionNearestAlly: {
        BehaviorTickResult result;
        if (!ally_context_available(ctx.allies)) {
            result.status = BehaviorStatus::Failure;
            return result;
        }

        const NearestAllyResult nearest = find_nearest_ally_within_radius(agentIndex,
                                                                        agent.teamId,
                                                                        agent.x,
                                                                        agent.y,
                                                                        node.threshold,
                                                                        *ctx.allies);
        if (!nearest.found) {
            result.status = BehaviorStatus::Failure;
            return result;
        }

        result.status = BehaviorStatus::Success;
        result.wroteFlag = true;
        result.flagIndex = node.flagIndex;
        result.flagValue = true;
        if (node.scalarSlot < Blackboard::kMaxScalars) {
            result.wroteScalar = true;
            result.scalarIndex = node.scalarSlot;
            result.scalarValue = static_cast<float>(nearest.allyIndex);
        }
        return result;
    }
    case NodeKind::ActionNearestAllyDistance: {
        BehaviorTickResult result;
        if (!ally_context_available(ctx.allies)) {
            result.status = BehaviorStatus::Failure;
            return result;
        }

        const float distanceSq = nearest_ally_distance_sq(agentIndex,
                                                           agent.teamId,
                                                           agent.x,
                                                           agent.y,
                                                           *ctx.allies);
        if (distanceSq <= 0.f) {
            result.status = BehaviorStatus::Failure;
            return result;
        }

        result.status = BehaviorStatus::Success;
        if (node.scalarSlot < Blackboard::kMaxScalars) {
            result.wroteScalar = true;
            result.scalarIndex = node.scalarSlot;
            result.scalarValue = distanceSq;
        }
        if (node.flagIndex < Blackboard::kMaxFlags) {
            result.wroteFlag = true;
            result.flagIndex = node.flagIndex;
            result.flagValue = true;
        }
        return result;
    }
    case NodeKind::GuardBlackboardBound: {
        BehaviorTickResult result;
        result.status = board.isBound() ? BehaviorStatus::Success : BehaviorStatus::Failure;
        return result;
    }
    case NodeKind::GuardBlackboardScalarEmpty: {
        BehaviorTickResult result;
        if (!board.isBound()) {
            result.status = BehaviorStatus::Failure;
            return result;
        }
        const u32 slot = node.scalarSlot < Blackboard::kMaxScalars ? node.scalarSlot : 0u;
        result.status = board.isScalarEmpty(agentIndex, slot) ? BehaviorStatus::Success
                                                              : BehaviorStatus::Failure;
        return result;
    }
    case NodeKind::GuardBlackboardScalarSet: {
        BehaviorTickResult result;
        if (!board.isBound() || !board.isAgentValid(agentIndex)) {
            result.status = BehaviorStatus::Failure;
            return result;
        }
        const u32 slot = node.scalarSlot < Blackboard::kMaxScalars ? node.scalarSlot : 0u;
        result.status = board.isScalarSet(agentIndex, slot) ? BehaviorStatus::Success
                                                            : BehaviorStatus::Failure;
        return result;
    }
    case NodeKind::GuardBlackboardFlagEmpty: {
        BehaviorTickResult result;
        if (!board.isBound()) {
            result.status = BehaviorStatus::Failure;
            return result;
        }
        const u32 flag = node.flagIndex < Blackboard::kMaxFlags ? node.flagIndex : 0u;
        result.status = board.isFlagEmpty(agentIndex, flag) ? BehaviorStatus::Success
                                                            : BehaviorStatus::Failure;
        return result;
    }
    case NodeKind::GuardBlackboardFlagSet: {
        BehaviorTickResult result;
        if (!board.isBound() || !board.isAgentValid(agentIndex)) {
            result.status = BehaviorStatus::Failure;
            return result;
        }
        const u32 flag = node.flagIndex < Blackboard::kMaxFlags ? node.flagIndex : 0u;
        result.status = board.isFlagSet(agentIndex, flag) ? BehaviorStatus::Success
                                                          : BehaviorStatus::Failure;
        return result;
    }
    case NodeKind::GuardBlackboardEmpty: {
        BehaviorTickResult result;
        result.status = board.isBoardEmpty() ? BehaviorStatus::Success : BehaviorStatus::Failure;
        return result;
    }
    case NodeKind::GuardBlackboardNonempty: {
        BehaviorTickResult result;
        result.status = board.isBoardEmpty() ? BehaviorStatus::Failure : BehaviorStatus::Success;
        return result;
    }
    case NodeKind::GuardBlackboardAgentValid: {
        BehaviorTickResult result;
        result.status = board.isAgentValid(agentIndex) ? BehaviorStatus::Success
                                                       : BehaviorStatus::Failure;
        return result;
    }
    case NodeKind::GuardAllyContext: {
        BehaviorTickResult result;
        result.status = ally_context_available(ctx.allies) ? BehaviorStatus::Success
                                                           : BehaviorStatus::Failure;
        return result;
    }
    case NodeKind::GuardValidAllyRadius: {
        BehaviorTickResult result;
        result.status = is_valid_ally_radius(node.threshold) ? BehaviorStatus::Success
                                                           : BehaviorStatus::Failure;
        return result;
    }
    }
    return {};
}

BehaviorTickResult BehaviorTree::tick(u32 agentIndex,
                                      const AgentSnapshot& agent,
                                      const BlackboardView& board,
                                      const BehaviorEvalContext& ctx) const {
    if (m_nodes.empty()) {
        return {};
    }
    return tickNode(m_root, agentIndex, agent, board, ctx);
}

BehaviorTree BehaviorTree::makePatrolWhenNearTarget() {
    BehaviorTree tree;

    const u32 condition = 0;
    const u32 action = 1;
    const u32 sequence = 2;

    tree.addNode({NodeKind::ConditionDistanceLess, 5.f, 0, 1, 0, 0});
    tree.addNode({NodeKind::ActionSetFlag, 0.f, 0, 1, 0, 0});
    tree.addNode({NodeKind::Sequence, 0.f, 0, 1, condition, action});
    tree.setRoot(sequence);
    return tree;
}

BehaviorTree BehaviorTree::makePatrolWhenNearTargetFromRegistry() {
    const std::vector<NodeLoadSpec> specs = {
        {"bb.condition.distance_less", 5.f, 0, 1, {}, {}},
        {"bb.action.set_flag", 0.f, 0, 1, {}, {}},
        {"bb.sequence", 0.f, 0, 1, {}, {0, 1}},
    };

    BehaviorTree tree;
    loadTreeFromSpecs(specs, 2, tree);
    return tree;
}

} // namespace fuse::ai
