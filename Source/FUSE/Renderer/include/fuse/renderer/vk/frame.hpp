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
    void* computeCommandBuffer = nullptr; // VkCommandBuffer
    void* descriptorPool = nullptr;  // VkDescriptorPool — per-slot, reset each beginFrame
};

struct FrameSyncData {
    void* imageAvailable = nullptr;  // VkSemaphore — swapchain acquire signal
    void* renderFinished = nullptr;  // VkSemaphore — present wait
    void* inFlightFence = nullptr;   // VkFence — CPU wait before reusing slot
    void* timelineSemaphore = nullptr; // VkSemaphore — VK_SEMAPHORE_TYPE_TIMELINE
    u64 timelineValue = 0;
    bool fenceSignaled = false;
    void* timestampQueryPool = nullptr; // VkQueryPool — TIMESTAMP, 2 queries (begin/end)
    FrameCommandData commands{};
};

struct FrameManagerInfo {
    bool ready = false;
    u32 framesInFlight = kFramesInFlight;
    u32 currentIndex = 0;
    u64 totalFrames = 0;
    bool timestampsReady = false;
    u64 lastGpuTimeNs = 0;
    u32 timestampWriteCount = 0;
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
    void* currentTransferCommandBuffer() const;
    void* currentComputeCommandBuffer() const;
    void* currentTimelineSemaphore() const;
    void* currentDescriptorPool() const;
    u64 currentTimelineValue() const;
    u32 descriptorPoolResetCount() const { return m_descriptorPoolResetCount; }

    bool timestampsReady() const { return m_info.timestampsReady; }
    u64 lastGpuTimeNs() const { return m_info.lastGpuTimeNs; }
    u32 timestampWriteCount() const { return m_info.timestampWriteCount; }
    u32 debugNamesSet() const { return m_debugNamesSet; }

    /// vkCmdResetQueryPool + vkCmdWriteTimestamp TOP_OF_PIPE (query 0) on the current slot pool.
    /// `commandBuffer` is a native VkCommandBuffer that must be in recording state. No-op if not ready.
    void writeTimestampBegin(void* commandBuffer);
    /// vkCmdWriteTimestamp BOTTOM_OF_PIPE (query 1) on the current slot pool. No-op if not ready.
    void writeTimestampEnd(void* commandBuffer);
    /// vkGetQueryPoolResults (64-bit) after the slot fence is signaled. Multiplies ticks by timestampPeriod.
    /// Returns false if timestamps are not ready or results are unavailable. lastGpuTimeNs may be 0.
    bool readLastGpuTimeNs(u32 slot, u64* outNs);

    /// Allocate `count` descriptor sets from the current slot pool using `setLayout` (VkDescriptorSetLayout).
    /// Returns number allocated (0 on stub / null layout / null pool / vkAllocate failure).
    /// outSets may be null if count==0. When non-null, writes up to `count` void* native sets.
    u32 allocateDescriptorSets(void* setLayout, u32 count, void** outSets);

    u32 descriptorSetsAllocatedThisFrame() const { return m_descriptorSetsAllocatedThisFrame; }

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
    u32 m_descriptorPoolResetCount = 0;
    u32 m_descriptorSetsAllocatedThisFrame = 0;
    float m_timestampPeriod = 0.f;
    u32 m_debugNamesSet = 0;
};

} // namespace fuse::renderer
