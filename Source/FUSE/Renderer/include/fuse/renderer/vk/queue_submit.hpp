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

/// Record a minimal one-shot command buffer into the current frame slot (headless-safe).
bool recordFrameSlotCommands(VulkanDevice& device, FrameManager& frameManager);

/// Submit the current frame slot command buffer via `vkQueueSubmit`.
/// Headless CI: real submit + fence signal; no `vkQueuePresentKHR`.
/// Presentable WSI: waits on `imageAvailable`, signals `renderFinished`, signals `inFlightFence`.
GraphicsQueueSubmitResult submitGraphicsQueue(const GraphicsQueueSubmitDesc& desc);

} // namespace fuse::renderer
