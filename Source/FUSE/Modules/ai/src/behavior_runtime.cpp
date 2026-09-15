#include <fuse/ai/behavior_runtime.hpp>

#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/jobs/parallel_for.hpp>

namespace fuse::ai {

void BehaviorRuntime::setTree(const BehaviorTree& tree) {
    m_tree = tree;
    m_waitStartTicks.clear();
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

void BehaviorRuntime::ensureWaitState() {
    const std::size_t perAgent = m_tree.nodeCount();
    m_waitStartTicks.assign(m_bindings.size() * perAgent, 0);
}

void BehaviorRuntime::buildSnapshots() {
    m_snapshots.clear();
    m_snapshots.reserve(m_bindings.size());

    for (const AgentBinding& binding : m_bindings) {
        AgentSnapshot snap;
        snap.agent = binding.agent;
        snap.x = binding.x;
        snap.y = binding.y;
        snap.targetX = binding.targetX;
        snap.targetY = binding.targetY;
        m_snapshots.push_back(snap);
    }

    if (m_results.size() != m_snapshots.size()) {
        m_results.assign(m_snapshots.size(), BehaviorTickResult{});
    }

    ensureWaitState();
}

void BehaviorRuntime::evaluate(const frame::FrameCtx& ctx) {
    (void)ctx;
    if (m_snapshots.empty()) {
        return;
    }

    const BlackboardView boardView(m_blackboard);
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    const u32 nodeCount = m_tree.nodeCount();

    scheduler.parallel_for(0, static_cast<u32>(m_snapshots.size()), 1, [&](u32 agentIndex) {
        BehaviorEvalContext evalCtx;
        evalCtx.tickCount = m_tickCount;
        if (nodeCount > 0) {
            evalCtx.waitStartTicks = m_waitStartTicks.data() + static_cast<std::size_t>(agentIndex) * nodeCount;
        }
        m_results[agentIndex] = m_tree.tick(agentIndex, m_snapshots[agentIndex], boardView, evalCtx);
    });
}

void BehaviorRuntime::commit() {
    for (u32 agentIndex = 0; agentIndex < static_cast<u32>(m_results.size()); ++agentIndex) {
        const BehaviorTickResult& result = m_results[agentIndex];
        if (result.wroteFlag) {
            m_blackboard.setFlag(agentIndex, result.flagIndex, result.flagValue);
        }
    }
    ++m_tickCount;
}

} // namespace fuse::ai
