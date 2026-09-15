#include <fuse/ai/blackboard.hpp>

namespace fuse::ai {

void Blackboard::resize(u32 agentCount) {
    m_agentCount = agentCount;
    m_flags.assign(static_cast<std::size_t>(agentCount) * kMaxFlags, 0);
}

void Blackboard::setFlag(u32 agentIndex, u32 flagIndex, bool value) {
    if (agentIndex >= m_agentCount || flagIndex >= kMaxFlags) {
        return;
    }
    m_flags[static_cast<std::size_t>(agentIndex) * kMaxFlags + flagIndex] = value ? 1 : 0;
}

bool Blackboard::flag(u32 agentIndex, u32 flagIndex) const {
    if (agentIndex >= m_agentCount || flagIndex >= kMaxFlags) {
        return false;
    }
    return m_flags[static_cast<std::size_t>(agentIndex) * kMaxFlags + flagIndex] != 0;
}

void Blackboard::clearFlags(u32 agentIndex) {
    if (agentIndex >= m_agentCount) {
        return;
    }
    for (u32 flagIndex = 0; flagIndex < kMaxFlags; ++flagIndex) {
        setFlag(agentIndex, flagIndex, false);
    }
}

BlackboardView::BlackboardView(const Blackboard& board)
    : m_board(&board) {}

bool BlackboardView::flag(u32 agentIndex, u32 flagIndex) const {
    return m_board ? m_board->flag(agentIndex, flagIndex) : false;
}

} // namespace fuse::ai
