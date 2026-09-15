#pragma once

#include <fuse/ai/agent_snapshot.hpp>
#include <fuse/ai/behavior_tree.hpp>
#include <fuse/ai/blackboard.hpp>
#include <fuse/ai/spatial_query.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::ai {

struct AgentBinding {
    Handle<Object> agent = Handle<Object>::invalid();
    float x = 0.f;
    float y = 0.f;
    float targetX = 0.f;
    float targetY = 0.f;
    u32 teamId = 0;
};

/// Game-thread facade: build snapshots, jobify BT eval, commit blackboard writes.
/// Ore refs: BadBehaviour Runner, GuideBot actions, UAISK templates (Samples only).
class BehaviorRuntime {
public:
    void setTree(const BehaviorTree& tree);
    const BehaviorTree& tree() const { return m_tree; }

    void clearAgents();
    void addAgent(const AgentBinding& binding);
    u32 agentCount() const { return static_cast<u32>(m_bindings.size()); }

    Blackboard& blackboard() { return m_blackboard; }
    const Blackboard& blackboard() const { return m_blackboard; }

    /// Game thread: publish read-only snapshots for this frame.
    void buildSnapshots();

    /// Game thread: fork-join BT eval over agents (JobScheduler when available).
    void evaluate(const frame::FrameCtx& ctx);

    /// Game thread: apply BT action outputs to blackboard (already written during eval).
    void commit();

    const std::vector<AgentSnapshot>& snapshots() const { return m_snapshots; }
    const std::vector<BehaviorTickResult>& lastResults() const { return m_results; }

    u32 tickCount() const { return m_tickCount; }

private:
    void ensureWaitState();
    void buildAllyCandidates();

    BehaviorTree m_tree;
    std::vector<AgentBinding> m_bindings;
    std::vector<AgentSnapshot> m_snapshots;
    std::vector<BehaviorTickResult> m_results;
    std::vector<AllyCandidate> m_allies;
    Blackboard m_blackboard;
    std::vector<u32> m_waitStartTicks;
    u32 m_tickCount = 0;
};

} // namespace fuse::ai
