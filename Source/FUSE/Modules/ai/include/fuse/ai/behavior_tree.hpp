#pragma once

#include <fuse/ai/agent_snapshot.hpp>
#include <fuse/ai/blackboard.hpp>
#include <fuse/types.hpp>

#include <string>
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
    Inverter,
    Loop,
    SucceedAlways,
    Root,
    ConditionDistanceLess,
    ConditionDistanceGreater,
    ConditionBlackboardGet,
    ActionSetFlag,
    ActionBlackboardSet,
    ActionWait,
    ActionDistance,
    ActionMoveToward,
};

/// Optional per-tick eval state for leaves that span frames (wait) or read runtime tick count.
struct BehaviorEvalContext {
    u32 tickCount = 0;
    /// Per-node wait start tick for the current agent; length == tree nodeCount, 0 = not waiting.
    u32* waitStartTicks = nullptr;
};

/// Flat behavior-tree node — ore analogue: BadBehaviour composite/decorator/leaf nodes.
/// Ore: third_party/addons/BadBehaviour/Engine/source/BadBehavior/
struct BehaviorNode {
    NodeKind kind = NodeKind::Sequence;
    float threshold = 0.f;
    u32 flagIndex = 0;
    u32 loopCount = 1;
    u32 childA = 0;
    u32 childB = 0;
    std::string scriptHook;
};

/// Job-friendly BT evaluator — read-only snapshot + blackboard view, no scene mutation.
class BehaviorTree {
public:
    void addNode(BehaviorNode node);
    void setRoot(u32 nodeIndex);

    u32 nodeCount() const { return static_cast<u32>(m_nodes.size()); }
    u32 rootIndex() const { return m_root; }
    const BehaviorNode& node(u32 index) const { return m_nodes.at(index); }

    /// Evaluate one agent against immutable inputs (safe from worker threads).
    BehaviorTickResult tick(u32 agentIndex,
                            const AgentSnapshot& agent,
                            const BlackboardView& board,
                            const BehaviorEvalContext& ctx = {}) const;

    /// Demo tree: Sequence(ConditionDistanceLess(5), ActionSetFlag(0)).
    static BehaviorTree makePatrolWhenNearTarget();

    /// Same patrol demo built through NodeRegistry load path.
    static BehaviorTree makePatrolWhenNearTargetFromRegistry();

private:
    BehaviorTickResult tickNode(u32 nodeIndex,
                                u32 agentIndex,
                                const AgentSnapshot& agent,
                                const BlackboardView& board,
                                const BehaviorEvalContext& ctx) const;

    std::vector<BehaviorNode> m_nodes;
    u32 m_root = 0;
};

} // namespace fuse::ai
