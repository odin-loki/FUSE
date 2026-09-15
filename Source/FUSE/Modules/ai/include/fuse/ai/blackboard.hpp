#pragma once

#include <fuse/types.hpp>

#include <vector>

namespace fuse::ai {

/// Per-agent scratch flags written by BT actions; committed on the game thread.
class Blackboard {
public:
    static constexpr u32 kMaxFlags = 8;

    void resize(u32 agentCount);
    u32 agentCount() const { return m_agentCount; }

    void setFlag(u32 agentIndex, u32 flagIndex, bool value);
    bool getFlag(u32 agentIndex, u32 flagIndex) const { return flag(agentIndex, flagIndex); }
    bool flag(u32 agentIndex, u32 flagIndex) const;

    void clearFlags(u32 agentIndex);

private:
    u32 m_agentCount = 0;
    std::vector<u8> m_flags;
};

/// Immutable view passed to worker BT eval jobs.
class BlackboardView {
public:
    BlackboardView() = default;
    explicit BlackboardView(const Blackboard& board);

    bool getFlag(u32 agentIndex, u32 flagIndex) const { return flag(agentIndex, flagIndex); }
    bool flag(u32 agentIndex, u32 flagIndex) const;

private:
    const Blackboard* m_board = nullptr;
};

} // namespace fuse::ai
