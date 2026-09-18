#include <fuse/ai/blackboard.hpp>

namespace fuse::ai {

void Blackboard::resize(u32 agentCount) {
    m_agentCount = agentCount;
    m_flags.assign(static_cast<std::size_t>(agentCount) * kMaxFlags, 0);
    m_scalars.assign(static_cast<std::size_t>(agentCount) * kMaxScalars, 0.f);
}

bool Blackboard::trySetFlag(u32 agentIndex, u32 flagIndex, bool value) {
    if (!isValidAgent(agentIndex) || !isValidFlag(flagIndex)) {
        return false;
    }
    m_flags[static_cast<std::size_t>(agentIndex) * kMaxFlags + flagIndex] = value ? 1 : 0;
    return true;
}

void Blackboard::setFlag(u32 agentIndex, u32 flagIndex, bool value) {
    trySetFlag(agentIndex, flagIndex, value);
}

bool Blackboard::tryGetFlag(u32 agentIndex, u32 flagIndex, bool& outValue) const {
    if (!isValidAgent(agentIndex) || !isValidFlag(flagIndex)) {
        return false;
    }
    outValue = m_flags[static_cast<std::size_t>(agentIndex) * kMaxFlags + flagIndex] != 0;
    return true;
}

bool Blackboard::flag(u32 agentIndex, u32 flagIndex) const {
    bool value = false;
    tryGetFlag(agentIndex, flagIndex, value);
    return value;
}

bool Blackboard::trySetScalar(u32 agentIndex, u32 slotIndex, float value) {
    if (!isValidAgent(agentIndex) || !isValidScalar(slotIndex)) {
        return false;
    }
    m_scalars[static_cast<std::size_t>(agentIndex) * kMaxScalars + slotIndex] = value;
    return true;
}

void Blackboard::setScalar(u32 agentIndex, u32 slotIndex, float value) {
    trySetScalar(agentIndex, slotIndex, value);
}

bool Blackboard::tryGetScalar(u32 agentIndex, u32 slotIndex, float& outValue) const {
    if (!isValidAgent(agentIndex) || !isValidScalar(slotIndex)) {
        return false;
    }
    outValue = m_scalars[static_cast<std::size_t>(agentIndex) * kMaxScalars + slotIndex];
    return true;
}

float Blackboard::scalar(u32 agentIndex, u32 slotIndex) const {
    float value = 0.f;
    tryGetScalar(agentIndex, slotIndex, value);
    return value;
}

void Blackboard::clearFlags(u32 agentIndex) {
    if (!isValidAgent(agentIndex)) {
        return;
    }
    for (u32 flagIndex = 0; flagIndex < kMaxFlags; ++flagIndex) {
        trySetFlag(agentIndex, flagIndex, false);
    }
}

void Blackboard::clearScalars(u32 agentIndex) {
    if (!isValidAgent(agentIndex)) {
        return;
    }
    for (u32 slotIndex = 0; slotIndex < kMaxScalars; ++slotIndex) {
        trySetScalar(agentIndex, slotIndex, 0.f);
    }
}

BlackboardView::BlackboardView(const Blackboard& board)
    : m_board(&board) {}

bool BlackboardView::tryGetFlag(u32 agentIndex, u32 flagIndex, bool& outValue) const {
    return m_board ? m_board->tryGetFlag(agentIndex, flagIndex, outValue) : false;
}

bool BlackboardView::flag(u32 agentIndex, u32 flagIndex) const {
    return m_board ? m_board->flag(agentIndex, flagIndex) : false;
}

bool BlackboardView::tryGetScalar(u32 agentIndex, u32 slotIndex, float& outValue) const {
    return m_board ? m_board->tryGetScalar(agentIndex, slotIndex, outValue) : false;
}

float BlackboardView::scalar(u32 agentIndex, u32 slotIndex) const {
    return m_board ? m_board->scalar(agentIndex, slotIndex) : 0.f;
}

u32 BlackboardView::agentCount() const {
    return m_board ? m_board->agentCount() : 0u;
}

bool BlackboardView::isAgentValid(u32 agentIndex) const {
    return m_board && agentIndex < m_board->agentCount();
}

bool BlackboardView::isScalarEmpty(u32 agentIndex, u32 slotIndex) const {
    float value = 0.f;
    if (!tryGetScalar(agentIndex, slotIndex, value)) {
        return true;
    }
    return value == 0.f;
}

bool BlackboardView::isFlagEmpty(u32 agentIndex, u32 flagIndex) const {
    bool value = false;
    if (!tryGetFlag(agentIndex, flagIndex, value)) {
        return true;
    }
    return !value;
}

bool BlackboardView::isScalarSet(u32 agentIndex, u32 slotIndex) const {
    return !isScalarEmpty(agentIndex, slotIndex);
}

bool BlackboardView::isFlagSet(u32 agentIndex, u32 flagIndex) const {
    return !isFlagEmpty(agentIndex, flagIndex);
}

} // namespace fuse::ai
