#pragma once

#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/frame.hpp>
#include <fuse/renderer/vk/swapchain.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::renderer {

struct GraphicsQueueSubmitDesc {
    VulkanDevice* device = nullptr;
    FrameManager* frameManager = nullptr;
    const VulkanSwapchain* swapchain = nullptr;
    /// UINT32_MAX when headless or acquire failed — submit omits WSI wait semaphores.
    u32 acquiredImageIndex = UINT32_MAX;
    /// When true, frame slot CB was already recorded (render graph execute path).
    bool commandsAlreadyRecorded = false;
};

struct GraphicsQueueSubmitResult {
    bool ok = false;
    bool submitted = false;
    bool headless = true;
    bool semaphoresUsed = false;
    std::string message;
};

/// True when acquire returned a real swapchain image and WSI semaphores should be wired.
bool shouldUseSwapchainSemaphores(const VulkanSwapchain* swapchain, u32 acquiredImageIndex);

/// Reset the current frame slot command pool before graph execute recording.
bool resetFrameSlotCommandPool(VulkanDevice& device, FrameManager& frameManager);

/// Record a minimal one-shot command buffer into the current frame slot (headless-safe).
bool recordFrameSlotCommands(VulkanDevice& device, FrameManager& frameManager);

/// Submit the current frame slot command buffer via `vkQueueSubmit`.
/// Headless CI: real submit + fence signal; no `vkQueuePresentKHR`.
/// Presentable WSI: waits on `imageAvailable`, signals `renderFinished`, signals `inFlightFence`.
/// When `FrameSyncData::timelineSemaphore` is non-null and timeline headers exist, also signals
/// the timeline to `timelineValue + 1` via `VkTimelineSemaphoreSubmitInfo`.
GraphicsQueueSubmitResult submitGraphicsQueue(const GraphicsQueueSubmitDesc& desc);

/// Submit the current slot transfer command buffer via `vkQueueSubmit`.
/// Records a one-time empty CB when not already recorded; skips when the transfer CB is null.
/// No WSI semaphores. Does not wait or signal the in-flight fence.
/// Uses `device.queues().transfer`, falling back to graphics if transfer is null.
GraphicsQueueSubmitResult submitTransferQueue(const GraphicsQueueSubmitDesc& desc);

} // namespace fuse::renderer
