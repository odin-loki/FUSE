#include <fuse/ai/behavior_tree.hpp>

namespace fuse::ai {

void BehaviorTree::addNode(BehaviorNode node) {
    m_nodes.push_back(node);
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

    tree.addNode({NodeKind::ConditionDistanceLess, 5.f, 0, 0, 0});
    tree.addNode({NodeKind::ActionSetFlag, 0.f, 0, 0, 0});
    tree.addNode({NodeKind::Sequence, 0.f, 0, condition, action});
    tree.setRoot(sequence);
    return tree;
}

} // namespace fuse::ai
