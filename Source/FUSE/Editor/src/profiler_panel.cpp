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

} // namespace fuse::editor
