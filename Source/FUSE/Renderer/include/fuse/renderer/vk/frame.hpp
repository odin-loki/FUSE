#pragma once

#include <fuse/alloc/frame_allocator.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

/// Triple-buffered frame ring — aligned with architecture-parallel FrameBarrier sync point.
static constexpr u32 kFramesInFlight = 3;

struct FrameCommandData {
    void* commandPool = nullptr;     // VkCommandPool
    void* primaryCommandBuffer = nullptr; // VkCommandBuffer
    void* transferCommandBuffer = nullptr; // VkCommandBuffer
    void* descriptorPool = nullptr;  // VkDescriptorPool — per-slot, reset each beginFrame
};

struct FrameSyncData {
    void* imageAvailable = nullptr;  // VkSemaphore — swapchain acquire signal
    void* renderFinished = nullptr;  // VkSemaphore — present wait
    void* inFlightFence = nullptr;   // VkFence — CPU wait before reusing slot
    bool fenceSignaled = false;
    FrameCommandData commands{};
};

struct FrameManagerInfo {
    bool ready = false;
    u32 framesInFlight = kFramesInFlight;
    u32 currentIndex = 0;
    u64 totalFrames = 0;
    std::string message;
};

/// Triple-buffered FrameData: per-slot CPU scratch (8MB) + optional VkDescriptorPool.
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

    FrameSyncData& current();
    const FrameSyncData& current() const;
    FrameSyncData& slot(u32 index);
    const FrameSyncData& slot(u32 index) const;
    void* currentCommandBuffer() const;

    fuse::alloc::FrameAllocator& scratch();
    const fuse::alloc::FrameAllocator& scratch() const;
    void* allocateScratch(u32 bytes, u32 align = 16);
    u32 scratchUsedBytes() const;

    /// Waits on the current slot fence, resets scratch + descriptor pool. Call after tick barrier, before render record.
    void beginFrame(u32 frameIndex);

    /// Wait on the in-flight fence for a slot before acquire (B2.2 present path).
    bool waitInFlightFence(u32 slotIndex);

    /// Marks the current slot as submitted — fence will be waited on next beginFrame.
    void endFrame();

    /// Mirrors FrameBarrier::signalTickJobsComplete — tick jobs for frame N are done.
    void signalTickComplete();

    bool tickComplete() const { return m_tickComplete; }

private:
    FrameManager() = default;
    bool initialize(VulkanDevice& device);
    void shutdown();
    void constructScratchAllocators();
    u32 currentSlotIndex() const { return m_info.currentIndex % kFramesInFlight; }

    FrameManagerInfo m_info;
    FrameSyncData m_slots[kFramesInFlight]{};
    std::unique_ptr<fuse::alloc::FrameAllocator> m_scratch[kFramesInFlight];
    bool m_tickComplete = false;
    u32 m_lastBarrierFrame = 0;
    void* m_device = nullptr;
};

} // namespace fuse::renderer
