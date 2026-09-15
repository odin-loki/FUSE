#pragma once

#include <fuse/ai/agent_snapshot.hpp>
#include <fuse/ai/blackboard.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::ai {

enum class BehaviorStatus {
    Running,
    Success,
    Failure,
};

struct BehaviorTickResult {
    BehaviorStatus status = BehaviorStatus::Failure;
    bool wroteFlag = false;
    u32 flagIndex = 0;
    bool flagValue = false;
};

enum class NodeKind {
    Sequence,
    Selector,
    ConditionDistanceLess,
    ActionSetFlag,
};

/// Flat behavior-tree node — ore analogue: BadBehaviour Branch/Behavior nodes.
/// TODO(U5 extract): map to third_party/addons/BadBehaviour/Engine/source/BadBehavior/core/
struct BehaviorNode {
    NodeKind kind = NodeKind::Sequence;
    float threshold = 0.f;
    u32 flagIndex = 0;
    u32 childA = 0;
    u32 childB = 0;
};

/// Job-friendly BT evaluator — read-only snapshot + blackboard view, no scene mutation.
class BehaviorTree {
public:
    void addNode(BehaviorNode node);
    void setRoot(u32 nodeIndex);

    u32 nodeCount() const { return static_cast<u32>(m_nodes.size()); }
    u32 rootIndex() const { return m_root; }

    /// Evaluate one agent against immutable inputs (safe from worker threads).
    BehaviorTickResult tick(u32 agentIndex,
                            const AgentSnapshot& agent,
                            const BlackboardView& board) const;

    /// Demo tree: Sequence(ConditionDistanceLess(5), ActionSetFlag(0)).
    static BehaviorTree makePatrolWhenNearTarget();

private:
    BehaviorTickResult tickNode(u32 nodeIndex,
                                u32 agentIndex,
                                const AgentSnapshot& agent,
                                const BlackboardView& board) const;

    std::vector<BehaviorNode> m_nodes;
    u32 m_root = 0;
};

} // namespace fuse::ai
