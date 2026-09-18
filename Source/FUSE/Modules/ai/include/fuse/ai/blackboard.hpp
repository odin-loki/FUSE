#pragma once

#include <fuse/types.hpp>

#include <vector>

namespace fuse::ai {

/// Per-agent scratch flags written by BT actions; committed on the game thread.
class Blackboard {
public:
    static constexpr u32 kMaxFlags = 8;
    static constexpr u32 kMaxScalars = 4;

    void resize(u32 agentCount);
    u32 agentCount() const { return m_agentCount; }
    bool isEmpty() const { return m_agentCount == 0; }

    void setFlag(u32 agentIndex, u32 flagIndex, bool value);
    bool trySetFlag(u32 agentIndex, u32 flagIndex, bool value);
    bool getFlag(u32 agentIndex, u32 flagIndex) const { return flag(agentIndex, flagIndex); }
    bool tryGetFlag(u32 agentIndex, u32 flagIndex, bool& outValue) const;
    bool flag(u32 agentIndex, u32 flagIndex) const;

    void setScalar(u32 agentIndex, u32 slotIndex, float value);
    bool trySetScalar(u32 agentIndex, u32 slotIndex, float value);
    float scalar(u32 agentIndex, u32 slotIndex) const;
    bool tryGetScalar(u32 agentIndex, u32 slotIndex, float& outValue) const;

    void clearFlags(u32 agentIndex);
    void clearScalars(u32 agentIndex);

    [[nodiscard]] bool isFlagSlotValid(u32 flagIndex) const { return isValidFlag(flagIndex); }
    [[nodiscard]] bool isScalarSlotValid(u32 slotIndex) const { return isValidScalar(slotIndex); }

private:
    bool isValidAgent(u32 agentIndex) const { return agentIndex < m_agentCount; }
    bool isValidFlag(u32 flagIndex) const { return flagIndex < kMaxFlags; }
    bool isValidScalar(u32 slotIndex) const { return slotIndex < kMaxScalars; }

    u32 m_agentCount = 0;
    std::vector<u8> m_flags;
    std::vector<float> m_scalars;
};

/// Immutable view passed to worker BT eval jobs.
class BlackboardView {
public:
    BlackboardView() = default;
    explicit BlackboardView(const Blackboard& board);

    bool isBound() const { return m_board != nullptr; }
    u32 agentCount() const;
    bool isBoardEmpty() const;
    bool isAgentValid(u32 agentIndex) const;
    bool isScalarEmpty(u32 agentIndex, u32 slotIndex) const;
    bool isScalarSet(u32 agentIndex, u32 slotIndex) const;
    bool isFlagEmpty(u32 agentIndex, u32 flagIndex) const;
    bool isFlagSet(u32 agentIndex, u32 flagIndex) const;
    bool isFlagSlotValid(u32 flagIndex) const;
    bool isScalarSlotValid(u32 slotIndex) const;

    bool getFlag(u32 agentIndex, u32 flagIndex) const { return flag(agentIndex, flagIndex); }
    bool tryGetFlag(u32 agentIndex, u32 flagIndex, bool& outValue) const;
    bool flag(u32 agentIndex, u32 flagIndex) const;

    float scalar(u32 agentIndex, u32 slotIndex) const;
    bool tryGetScalar(u32 agentIndex, u32 slotIndex, float& outValue) const;

private:
    const Blackboard* m_board = nullptr;
};

} // namespace fuse::ai
