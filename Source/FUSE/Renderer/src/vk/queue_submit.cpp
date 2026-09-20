#include <fuse/renderer/vk/queue_submit.hpp>

#include <fuse/renderer/vk/swapchain_util.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

bool shouldUseSwapchainSemaphores(const VulkanSwapchain* swapchain, u32 acquiredImageIndex) {
    return swapchain != nullptr && isSwapchainPresentable(*swapchain) && !isEmptyAcquireResult(acquiredImageIndex);
}

#if defined(FUSE_VULKAN_BACKEND)

bool recordMinimalSubmitCommands(VkCommandBuffer commandBuffer) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        return false;
    }
    return vkEndCommandBuffer(commandBuffer) == VK_SUCCESS;
}

bool resetSlotCommandPool(VkDevice device, const FrameSyncData& slot) {
    if (slot.commands.commandPool == nullptr) {
        return false;
    }
    return vkResetCommandPool(device, static_cast<VkCommandPool>(slot.commands.commandPool), 0) == VK_SUCCESS;
}

#endif

bool resetFrameSlotCommandPool(VulkanDevice& device, FrameManager& frameManager) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid() || !frameManager.isReady()) {
        return false;
    }
    auto vkDevice = static_cast<VkDevice>(device.nativeHandle());
    return resetSlotCommandPool(vkDevice, frameManager.current());
#else
    (void)device;
    (void)frameManager;
    return false;
#endif
}

bool recordFrameSlotCommands(VulkanDevice& device, FrameManager& frameManager) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid() || !frameManager.isReady()) {
        return false;
    }

    const FrameSyncData& slot = frameManager.current();
    auto commandBuffer = static_cast<VkCommandBuffer>(slot.commands.primaryCommandBuffer);
    if (commandBuffer == VK_NULL_HANDLE) {
        return false;
    }

    auto vkDevice = static_cast<VkDevice>(device.nativeHandle());
    if (!resetSlotCommandPool(vkDevice, slot)) {
        return false;
    }
    return recordMinimalSubmitCommands(commandBuffer);
#else
    (void)device;
    (void)frameManager;
    return false;
#endif
}

GraphicsQueueSubmitResult submitGraphicsQueue(const GraphicsQueueSubmitDesc& desc) {
    GraphicsQueueSubmitResult result;

    if (desc.device == nullptr || !desc.device->isValid() || desc.frameManager == nullptr ||
        !desc.frameManager->isReady()) {
        result.message = "submitGraphicsQueue requires valid device and frame manager";
        return result;
    }

#if defined(FUSE_VULKAN_BACKEND)
    FrameSyncData& slot = desc.frameManager->current();
    auto commandBuffer = static_cast<VkCommandBuffer>(slot.commands.primaryCommandBuffer);
    if (commandBuffer == VK_NULL_HANDLE || slot.commands.commandPool == nullptr) {
        result.message = "frame slot missing command buffer";
        return result;
    }

    auto vkDevice = static_cast<VkDevice>(desc.device->nativeHandle());
    if (!desc.commandsAlreadyRecorded) {
        if (!resetSlotCommandPool(vkDevice, slot)) {
            result.message = "command pool reset failed";
            return result;
        }
        if (!recordMinimalSubmitCommands(commandBuffer)) {
            result.message = "command buffer record failed";
            return result;
        }
    }

    const bool useSemaphores = shouldUseSwapchainSemaphores(desc.swapchain, desc.acquiredImageIndex);
    result.headless = !useSemaphores;
    result.semaphoresUsed = useSemaphores;

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    VkSemaphore waitSemaphores[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipelineStageFlags waitStages[2] = {};
    u32 waitCount = 0;
    VkSemaphore signalSemaphores[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    u32 signalCount = 0;
    if (useSemaphores) {
        waitSemaphores[waitCount] = static_cast<VkSemaphore>(slot.imageAvailable);
        waitStages[waitCount] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        ++waitCount;
        signalSemaphores[signalCount++] = static_cast<VkSemaphore>(slot.renderFinished);
    }

#if defined(VK_VERSION_1_2) || defined(VK_KHR_timeline_semaphore)
    VkTimelineSemaphoreSubmitInfo timelineInfo{};
    u64 signalValues[2] = {0, 0};
    u64 waitValues[2] = {0, 0};
    const u64 nextTimelineValue = slot.timelineValue + 1;
    const bool signalTimeline = slot.timelineSemaphore != nullptr;
    const bool waitTimeline = signalTimeline && slot.timelineValue > 0;
    if (waitTimeline) {
        waitSemaphores[waitCount] = static_cast<VkSemaphore>(slot.timelineSemaphore);
        waitStages[waitCount] = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        waitValues[waitCount] = slot.timelineValue;
        ++waitCount;
    }
    if (signalTimeline) {
        if (signalCount > 0) {
            signalValues[0] = 0;
            signalValues[1] = nextTimelineValue;
        } else {
            signalValues[0] = nextTimelineValue;
        }
        signalSemaphores[signalCount++] = static_cast<VkSemaphore>(slot.timelineSemaphore);
        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo.waitSemaphoreValueCount = waitCount;
        timelineInfo.pWaitSemaphoreValues = waitCount > 0 ? waitValues : nullptr;
        timelineInfo.signalSemaphoreValueCount = signalCount;
        timelineInfo.pSignalSemaphoreValues = signalValues;
        submitInfo.pNext = &timelineInfo;
    }
#else
    const bool signalTimeline = false;
    const bool waitTimeline = false;
    const u64 nextTimelineValue = 0;
    (void)nextTimelineValue;
    (void)waitTimeline;
#endif

    if (waitCount > 0) {
        submitInfo.waitSemaphoreCount = waitCount;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
    }
    if (signalCount > 0) {
        submitInfo.signalSemaphoreCount = signalCount;
        submitInfo.pSignalSemaphores = signalSemaphores;
    }

    VkFence fence = static_cast<VkFence>(slot.inFlightFence);
    VkQueue queue = static_cast<VkQueue>(desc.device->queues().graphics);
    if (vkQueueSubmit(queue, 1, &submitInfo, fence) != VK_SUCCESS) {
        result.message = "vkQueueSubmit failed";
        return result;
    }

    if (signalTimeline) {
        slot.timelineValue = nextTimelineValue;
        result.timelineSignaled = true;
        result.timelineWaited = waitTimeline;
        result.timelineValueAfter = slot.timelineValue;
    }
    slot.fenceSignaled = true;
    result.submitted = true;
    result.ok = true;
    result.message =
        useSemaphores ? "vkQueueSubmit with WSI semaphores" : "vkQueueSubmit headless (no WSI present)";
    return result;
#else
    (void)desc;
    result.ok = true;
    result.submitted = false;
    result.headless = true;
    result.message = "queue submit stub — Vulkan backend disabled";
    return result;
#endif
}

GraphicsQueueSubmitResult submitTransferQueue(const GraphicsQueueSubmitDesc& desc) {
    GraphicsQueueSubmitResult result;
    result.headless = true;
    result.semaphoresUsed = false;

    if (desc.device == nullptr || !desc.device->isValid() || desc.frameManager == nullptr ||
        !desc.frameManager->isReady()) {
        result.message = "submitTransferQueue requires valid device and frame manager";
        return result;
    }

#if defined(FUSE_VULKAN_BACKEND)
    FrameSyncData& slot = desc.frameManager->current();
    auto transferBuffer = static_cast<VkCommandBuffer>(slot.commands.transferCommandBuffer);
    if (transferBuffer == VK_NULL_HANDLE) {
        result.ok = true;
        result.submitted = false;
        result.message = "transfer command buffer null — skipped";
        return result;
    }

    if (!desc.commandsAlreadyRecorded) {
        if (vkResetCommandBuffer(transferBuffer, 0) != VK_SUCCESS) {
            result.message = "transfer command buffer reset failed";
            return result;
        }
        if (!recordMinimalSubmitCommands(transferBuffer)) {
            result.message = "transfer command buffer record failed";
            return result;
        }
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &transferBuffer;

    VkQueue queue = static_cast<VkQueue>(desc.device->queues().transfer);
    if (queue == VK_NULL_HANDLE) {
        queue = static_cast<VkQueue>(desc.device->queues().graphics);
    }
    if (vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
        result.message = "transfer vkQueueSubmit failed";
        return result;
    }

    result.submitted = true;
    result.ok = true;
    result.message = "vkQueueSubmit transfer (no WSI, no in-flight fence)";
    return result;
#else
    (void)desc;
    result.ok = true;
    result.submitted = false;
    result.message = "transfer queue submit stub — Vulkan backend disabled";
    return result;
#endif
}

} // namespace fuse::renderer
