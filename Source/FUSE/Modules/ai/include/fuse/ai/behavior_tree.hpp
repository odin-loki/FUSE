#pragma once

#include <fuse/ai/agent_snapshot.hpp>
#include <fuse/ai/blackboard.hpp>
#include <fuse/ai/spatial_query.hpp>
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
    bool wroteScalar = false;
    u32 scalarIndex = 0;
    float scalarValue = 0.f;
};

enum class NodeKind {
    Sequence,
    Selector,
    Parallel,
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
    ConditionAlliesInRadius,
    ConditionAnyAllyInRadius,
    ActionNearestAlly,
    ActionAlliesCount,
    GuardBlackboardBound,
    GuardBlackboardScalarEmpty,
    GuardBlackboardFlagEmpty,
    GuardBlackboardEmpty,
    GuardBlackboardAgentValid,
    GuardBlackboardScalarSet,
    GuardBlackboardFlagSet,
    GuardAllyContext,
    GuardAllyRadiusValid,
};

/// Optional per-tick eval state for leaves that span frames (wait) or read runtime tick count.
struct BehaviorEvalContext {
    u32 tickCount = 0;
    /// Per-node wait start tick for the current agent; length == tree nodeCount, 0 = not waiting.
    u32* waitStartTicks = nullptr;
    /// Read-only ally list for spatial query leaves (built by BehaviorRuntime each frame).
    const std::vector<AllyCandidate>* allies = nullptr;
};

/// Per-child aggregation policy for `bb.parallel` (BadBehaviour Parallel composite stub).
struct ParallelPolicy {
    /// Minimum child successes required; 0 means all active children must succeed.
    u32 successThreshold = 0;
    /// Failures tolerated before the composite fails; 0 defaults to 1.
    u32 failThreshold = 1;
    /// When true, stop ticking remaining children once fail threshold is reached.
    bool abortOnFail = false;
    /// When true, stop ticking remaining children once success threshold is met.
    bool abortOnSuccess = false;
    /// When true, fail immediately when the blackboard view is not bound.
    bool requireBoundBlackboard = false;
    /// When true, fail immediately when ally context is null or empty.
    bool requireAllyContext = false;
    /// When true, fail immediately when the agent index is out of range for the blackboard.
    bool requireValidAgent = false;
    /// When true, fail immediately when the parallel node's `threshold` is not a valid ally radius.
    bool requireValidAllyRadius = false;
};

/// Flat behavior-tree node — ore analogue: BadBehaviour composite/decorator/leaf nodes.
/// Ore: third_party/addons/BadBehaviour/Engine/source/BadBehavior/
struct BehaviorNode {
    static constexpr u32 kNoScalarSlot = UINT32_MAX;

    NodeKind kind = NodeKind::Sequence;
    float threshold = 0.f;
    u32 flagIndex = 0;
    u32 loopCount = 1;
    u32 childA = 0;
    u32 childB = 0;
    ParallelPolicy parallelPolicy;
    u32 scalarSlot = kNoScalarSlot;
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
