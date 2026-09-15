#pragma once

#include <fuse/types.hpp>

#include <array>

namespace fuse::editor {

/// Headless profiler ring buffer (B6.10 stub — Qt plots deferred to U6 chrome).
class ProfilerPanel {
public:
    static constexpr u32 kHistoryFrames = 256;

    struct FrameProfileData {
        f32 cpuMs = 0.f;
        f32 gpuMs = 0.f;
        f32 depthPrepassMs = 0.f;
        f32 gbufferMs = 0.f;
        f32 sdfMarchMs = 0.f;
        f32 ddgiMs = 0.f;
        f32 deferredMs = 0.f;
        f32 shadowsMs = 0.f;
        f32 postprocessMs = 0.f;
        f32 physicsMs = 0.f;
        f32 ecsUpdateMs = 0.f;
        u32 drawCalls = 0;
        u32 triangles = 0;
        u32 sdfObjects = 0;
        u32 activePhysicsBodies = 0;
        usize cpuRamUsed = 0;
        usize gpuVramUsed = 0;
    };

    void pushFrameData(const FrameProfileData& data);
    void clearHistory();

    const FrameProfileData& latestFrame() const;
    const FrameProfileData& frameAt(u32 historyIndex) const;

    u32 frameCount() const { return m_frameCount; }
    u32 historyHead() const { return m_historyHead; }

    bool isPaused() const { return m_paused; }
    void setPaused(bool paused) { m_paused = paused; }

    f32 targetFps() const { return m_targetFps; }
    void setTargetFps(f32 fps) { m_targetFps = fps; }

private:
    std::array<FrameProfileData, kHistoryFrames> m_history{};
    u32 m_historyHead = 0;
    u32 m_frameCount = 0;
    bool m_paused = false;
    f32 m_targetFps = 60.f;
};

} // namespace fuse::editor
