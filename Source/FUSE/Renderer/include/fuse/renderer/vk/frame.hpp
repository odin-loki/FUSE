#pragma once

#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

/// Triple-buffered frame ring — aligned with architecture-parallel FrameBarrier sync point.
static constexpr u32 kFramesInFlight = 3;

struct FrameSyncData {
    void* imageAvailable = nullptr;  // VkSemaphore — swapchain acquire signal
    void* renderFinished = nullptr;  // VkSemaphore — present wait
    void* inFlightFence = nullptr;   // VkFence — CPU wait before reusing slot
    bool fenceSignaled = false;
};

struct FrameManagerInfo {
    bool ready = false;
    u32 framesInFlight = kFramesInFlight;
    u32 currentIndex = 0;
    u64 totalFrames = 0;
    std::string message;
};

/// Per-frame fencing sketch — command pools / descriptor pools deferred to B2.3+.
class FrameManager {
public:
    static std::unique_ptr<FrameManager> create(VulkanDevice& device);
    ~FrameManager();

    FrameManager(const FrameManager&) = delete;
    FrameManager& operator=(const FrameManager&) = delete;

    const FrameManagerInfo& info() const { return m_info; }
    bool isReady() const { return m_info.ready; }

    u32 currentIndex() const { return m_info.currentIndex; }
    u64 totalFrames() const { return m_info.totalFrames; }

    const FrameSyncData& current() const;
    const FrameSyncData& slot(u32 index) const;

    /// Waits on the current slot fence, advances ring index. Call after tick barrier, before render record.
    void beginFrame(u32 frameIndex);

    /// Marks the current slot as submitted — fence will be waited on next beginFrame.
    void endFrame();

    /// Mirrors FrameBarrier::signalTickJobsComplete — tick jobs for frame N are done.
    void signalTickComplete();

    bool tickComplete() const { return m_tickComplete; }

private:
    FrameManager() = default;
    bool initialize(VulkanDevice& device);
    void shutdown();

    FrameManagerInfo m_info;
    FrameSyncData m_slots[kFramesInFlight]{};
    bool m_tickComplete = false;
    u32 m_lastBarrierFrame = 0;
    void* m_device = nullptr;
};

} // namespace fuse::renderer
