#include <fuse/editor/profiler_panel.hpp>

namespace fuse::editor {

void ProfilerPanel::pushFrameData(const FrameProfileData& data) {
    if (m_paused) {
        return;
    }

    m_history[m_historyHead] = data;
    m_historyHead = (m_historyHead + 1u) % kHistoryFrames;
    if (m_frameCount < kHistoryFrames) {
        ++m_frameCount;
    }
}

void ProfilerPanel::clearHistory() {
    m_history.fill({});
    m_historyHead = 0;
    m_frameCount = 0;
}

const ProfilerPanel::FrameProfileData& ProfilerPanel::latestFrame() const {
    if (m_frameCount == 0) {
        return m_history[0];
    }

    const u32 latestIndex = (m_historyHead + kHistoryFrames - 1u) % kHistoryFrames;
    return m_history[latestIndex];
}

const ProfilerPanel::FrameProfileData& ProfilerPanel::frameAt(u32 historyIndex) const {
    if (m_frameCount == 0) {
        return m_history[0];
    }

    const u32 clampedIndex = historyIndex % m_frameCount;
    const u32 offset = (m_historyHead + kHistoryFrames - m_frameCount + clampedIndex) % kHistoryFrames;
    return m_history[offset];
}

ProfilerPanel::FrameStats ProfilerPanel::cpuStats() const {
    FrameStats stats;
    if (m_frameCount == 0) {
        return stats;
    }
    const f32 budget = m_targetFps > 0.f ? 1000.f / m_targetFps : 0.f;
    f32 sum = 0.f;
    stats.minMs = frameAt(0).cpuMs;
    stats.maxMs = stats.minMs;
    for (u32 i = 0; i < m_frameCount; ++i) {
        const f32 ms = frameAt(i).cpuMs;
        sum += ms;
        stats.minMs = ms < stats.minMs ? ms : stats.minMs;
        stats.maxMs = ms > stats.maxMs ? ms : stats.maxMs;
        if (budget > 0.f && ms > budget) {
            ++stats.overBudget;
        }
    }
    stats.frames = m_frameCount;
    stats.avgMs = sum / static_cast<f32>(m_frameCount);
    stats.avgFps = stats.avgMs > 0.f ? 1000.f / stats.avgMs : 0.f;
    return stats;
}

void ProfilerPanel::copyCpuHistory(std::vector<f32>& out) const {
    out.clear();
    out.reserve(m_frameCount);
    for (u32 i = 0; i < m_frameCount; ++i) {
        out.push_back(frameAt(i).cpuMs);
    }
}

} // namespace fuse::editor
