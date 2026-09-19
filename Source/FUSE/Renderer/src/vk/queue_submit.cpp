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

    VkPipelineStageFlags waitStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (useSemaphores) {
        VkSemaphore imageAvailable = static_cast<VkSemaphore>(slot.imageAvailable);
        VkSemaphore renderFinished = static_cast<VkSemaphore>(slot.renderFinished);
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &imageAvailable;
        submitInfo.pWaitDstStageMask = &waitStages;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderFinished;
    }

    VkFence fence = static_cast<VkFence>(slot.inFlightFence);
    VkQueue queue = static_cast<VkQueue>(desc.device->queues().graphics);
    if (vkQueueSubmit(queue, 1, &submitInfo, fence) != VK_SUCCESS) {
        result.message = "vkQueueSubmit failed";
        return result;
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

} // namespace fuse::renderer
