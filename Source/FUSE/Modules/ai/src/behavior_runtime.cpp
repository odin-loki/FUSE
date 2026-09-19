#include <fuse/ai/behavior_runtime.hpp>

#include <algorithm>

#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/jobs/parallel_for.hpp>

namespace fuse::ai {

const BehaviorTree& BehaviorRuntime::tree() const {
    const auto it = m_treeProfiles.find(m_defaultProfileId);
    if (it != m_treeProfiles.end()) {
        return it->second;
    }
    static const BehaviorTree kEmpty;
    return kEmpty;
}

void BehaviorRuntime::setTree(const BehaviorTree& tree) {
    registerTreeProfile(m_defaultProfileId, tree);
    m_waitStartTicks.clear();
}

void BehaviorRuntime::registerTreeProfile(u32 profileId, const BehaviorTree& tree) {
    m_treeProfiles[profileId] = tree;
    m_waitStartTicks.clear();
}

void BehaviorRuntime::reloadTreeProfile(u32 profileId, const BehaviorTree& tree, TreeReloadPolicy policy) {
    registerTreeProfile(profileId, tree);
    if (policy == TreeReloadPolicy::ResetAgents) {
        m_blackboard.resize(static_cast<u32>(m_bindings.size()));
        m_waitStartTicks.clear();
        m_tickCount = 0;
    }
}

void BehaviorRuntime::setAgentPositionProvider(AgentPositionProvider provider) {
    m_positionProvider = std::move(provider);
}

void BehaviorRuntime::syncAgentBindingsFromEntities() {
    if (!m_positionProvider) {
        return;
    }

    for (AgentBinding& binding : m_bindings) {
        if (!binding.agent.isValid()) {
            continue;
        }
        float x = binding.x;
        float y = binding.y;
        if (m_positionProvider(binding.agent, x, y)) {
            binding.x = x;
            binding.y = y;
        }
    }
}

void BehaviorRuntime::clearAgents() {
    m_bindings.clear();
    m_snapshots.clear();
    m_results.clear();
    m_blackboard.resize(0);
    m_waitStartTicks.clear();
}

void BehaviorRuntime::addAgent(const AgentBinding& binding) {
    m_bindings.push_back(binding);
    m_blackboard.resize(static_cast<u32>(m_bindings.size()));
    ensureWaitState();
}

void BehaviorRuntime::setBindingPosition(u32 agentIndex, float x, float y) {
    if (agentIndex < m_bindings.size()) {
        m_bindings[agentIndex].x = x;
        m_bindings[agentIndex].y = y;
    }
}

void BehaviorRuntime::setAgentEntity(u32 agentIndex, Handle<Object> entity) {
    if (agentIndex < m_bindings.size()) {
        m_bindings[agentIndex].agent = entity;
    }
}

u32 BehaviorRuntime::maxTreeNodeCount() const {
    u32 maxNodes = 0;
    for (const auto& entry : m_treeProfiles) {
        maxNodes = std::max(maxNodes, entry.second.nodeCount());
    }
    return maxNodes;
}

const BehaviorTree& BehaviorRuntime::treeForAgent(u32 agentIndex) const {
    u32 profileId = m_defaultProfileId;
    if (agentIndex < m_bindings.size()) {
        profileId = m_bindings[agentIndex].treeProfileId;
    }

    const auto it = m_treeProfiles.find(profileId);
    if (it != m_treeProfiles.end()) {
        return it->second;
    }

    const auto fallback = m_treeProfiles.find(m_defaultProfileId);
    if (fallback != m_treeProfiles.end()) {
        return fallback->second;
    }

    static const BehaviorTree kEmpty;
    return kEmpty;
}

void BehaviorRuntime::ensureWaitState() {
    const u32 nodeCount = maxTreeNodeCount();
    const std::size_t needed = m_bindings.size() * nodeCount;
    if (m_waitStartTicks.size() != needed) {
        m_waitStartTicks.assign(needed, 0);
    }
}

void BehaviorRuntime::buildSnapshots() {
    syncAgentBindingsFromEntities();

    m_snapshots.clear();
    m_snapshots.reserve(m_bindings.size());

    for (const AgentBinding& binding : m_bindings) {
        AgentSnapshot snap;
        snap.agent = binding.agent;
        snap.x = binding.x;
        snap.y = binding.y;
        snap.targetX = binding.targetX;
        snap.targetY = binding.targetY;
        snap.moveSpeed = binding.moveSpeed;
        snap.teamId = binding.teamId;
        m_snapshots.push_back(snap);
    }

    if (m_results.size() != m_snapshots.size()) {
        m_results.assign(m_snapshots.size(), BehaviorTickResult{});
    }
}

void BehaviorRuntime::buildAllyCandidates() {
    m_allies.clear();
    m_allies.reserve(m_bindings.size());
    for (u32 agentIndex = 0; agentIndex < static_cast<u32>(m_bindings.size()); ++agentIndex) {
        const AgentBinding& binding = m_bindings[agentIndex];
        AllyCandidate candidate;
        candidate.agentIndex = agentIndex;
        candidate.x = binding.x;
        candidate.y = binding.y;
        candidate.teamId = binding.teamId;
        m_allies.push_back(candidate);
    }
}

void BehaviorRuntime::evaluate(const frame::FrameCtx& ctx) {
    (void)ctx;
    if (m_snapshots.empty()) {
        return;
    }

    buildAllyCandidates();

    const BlackboardView boardView(m_blackboard);
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    const u32 maxNodeCount = maxTreeNodeCount();

    scheduler.parallel_for(0, static_cast<u32>(m_snapshots.size()), 1, [&](u32 agentIndex) {
        const BehaviorTree& tree = treeForAgent(agentIndex);
        const u32 nodeCount = tree.nodeCount();

        BehaviorEvalContext evalCtx;
        evalCtx.tickCount = m_tickCount;
        evalCtx.allies = &m_allies;
        if (maxNodeCount > 0) {
            evalCtx.waitStartTicks = m_waitStartTicks.data() + static_cast<std::size_t>(agentIndex) * maxNodeCount;
        }
        m_results[agentIndex] = tree.tick(agentIndex, m_snapshots[agentIndex], boardView, evalCtx);
        (void)nodeCount;
    });
}

void BehaviorRuntime::commit() {
    for (u32 agentIndex = 0; agentIndex < static_cast<u32>(m_results.size()); ++agentIndex) {
        const BehaviorTickResult& result = m_results[agentIndex];
        if (result.wroteFlag) {
            m_blackboard.setFlag(agentIndex, result.flagIndex, result.flagValue);
        }
        if (result.wroteScalar) {
            m_blackboard.setScalar(agentIndex, result.scalarIndex, result.scalarValue);
        }
        if (result.movedPosition && agentIndex < m_bindings.size()) {
            m_bindings[agentIndex].x += result.deltaX;
            m_bindings[agentIndex].y += result.deltaY;
        }
    }
    ++m_tickCount;
}

} // namespace fuse::ai
