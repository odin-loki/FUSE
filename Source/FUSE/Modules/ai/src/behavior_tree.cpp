#include <fuse/ai/behavior_tree.hpp>
#include <fuse/ai/node_registry.hpp>

namespace fuse::ai {

void BehaviorTree::addNode(BehaviorNode node) {
    m_nodes.push_back(std::move(node));
}

void BehaviorTree::setRoot(u32 nodeIndex) {
    m_root = nodeIndex;
}

BehaviorTickResult BehaviorTree::tickNode(u32 nodeIndex,
                                          u32 /*agentIndex*/,
                                          const AgentSnapshot& agent,
                                          const BlackboardView& board) const {
    if (nodeIndex >= m_nodes.size()) {
        return {};
    }

    const BehaviorNode& node = m_nodes[nodeIndex];
    switch (node.kind) {
    case NodeKind::Sequence: {
        BehaviorTickResult first = tickNode(node.childA, 0, agent, board);
        if (first.status != BehaviorStatus::Success) {
            return first;
        }
        return tickNode(node.childB, 0, agent, board);
    }
    case NodeKind::Selector: {
        BehaviorTickResult first = tickNode(node.childA, 0, agent, board);
        if (first.status == BehaviorStatus::Success) {
            return first;
        }
        return tickNode(node.childB, 0, agent, board);
    }
    case NodeKind::Inverter: {
        BehaviorTickResult child = tickNode(node.childA, 0, agent, board);
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
            lastChild = tickNode(node.childA, 0, agent, board);
            if (lastChild.status != BehaviorStatus::Success) {
                return lastChild;
            }
        }
        return lastChild;
    }
    case NodeKind::SucceedAlways: {
        BehaviorTickResult child = tickNode(node.childA, 0, agent, board);
        if (child.status == BehaviorStatus::Running) {
            return child;
        }
        child.status = BehaviorStatus::Success;
        return child;
    }
    case NodeKind::Root:
        return tickNode(node.childA, 0, agent, board);
    case NodeKind::ConditionDistanceLess: {
        BehaviorTickResult result;
        result.status = agent.distanceToTarget() < node.threshold ? BehaviorStatus::Success
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
    }
    return {};
}

BehaviorTickResult BehaviorTree::tick(u32 agentIndex,
                                      const AgentSnapshot& agent,
                                      const BlackboardView& board) const {
    if (m_nodes.empty()) {
        return {};
    }
    return tickNode(m_root, agentIndex, agent, board);
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
