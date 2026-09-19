#pragma once

#include <fuse/ai/agent_snapshot.hpp>
#include <fuse/ai/agent_entity_bind.hpp>
#include <fuse/ai/behavior_tree.hpp>
#include <fuse/ai/blackboard.hpp>
#include <fuse/ai/spatial_query.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <unordered_map>
#include <vector>

namespace fuse::ai {

enum class TreeReloadPolicy {
    PreserveBlackboard,
    ResetAgents,
};

struct AgentBinding {
    Handle<Object> agent = Handle<Object>::invalid();
    float x = 0.f;
    float y = 0.f;
    float targetX = 0.f;
    float targetY = 0.f;
    float moveSpeed = 1.f;
    u32 teamId = 0;
    /// Selects registered tree profile (default 0).
    u32 treeProfileId = 0;
};

/// Game-thread facade: build snapshots, jobify BT eval, commit blackboard writes.
/// Ore refs: BadBehaviour Runner, GuideBot actions, UAISK templates (Samples only).
class BehaviorRuntime {
public:
    void setTree(const BehaviorTree& tree);
    const BehaviorTree& tree() const;

    /// Register a named tree profile for per-agent selection.
    void registerTreeProfile(u32 profileId, const BehaviorTree& tree);
    /// Hot-reload a profile tree with optional blackboard reset (runtime tree reload ore).
    void reloadTreeProfile(u32 profileId, const BehaviorTree& tree, TreeReloadPolicy policy);
    u32 treeProfileCount() const { return static_cast<u32>(m_treeProfiles.size()); }

    void setAgentPositionProvider(AgentPositionProvider provider);
    void syncAgentBindingsFromEntities();

    void clearAgents();
    void addAgent(const AgentBinding& binding);
    void setBindingPosition(u32 agentIndex, float x, float y);
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

    const std::vector<AgentBinding>& bindings() const { return m_bindings; }

private:
    void ensureWaitState();
    void buildAllyCandidates();
    [[nodiscard]] const BehaviorTree& treeForAgent(u32 agentIndex) const;
    [[nodiscard]] u32 maxTreeNodeCount() const;

    std::unordered_map<u32, BehaviorTree> m_treeProfiles;
    u32 m_defaultProfileId = 0;
    AgentPositionProvider m_positionProvider;
    std::vector<AgentBinding> m_bindings;
    std::vector<AgentSnapshot> m_snapshots;
    std::vector<BehaviorTickResult> m_results;
    std::vector<AllyCandidate> m_allies;
    Blackboard m_blackboard;
    std::vector<u32> m_waitStartTicks;
    u32 m_tickCount = 0;
};

} // namespace fuse::ai
