#include <fuse/renderer/vk/image_readback.hpp>

#include <cstring>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

#if defined(FUSE_VULKAN_BACKEND)
namespace {

u32 findHostReadableMemoryType(VkPhysicalDevice physicalDevice, u32 typeFilter) {
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) != 0u && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    return 0;
}

} // namespace
#endif

bool readbackColorImage(VulkanDevice& device, void* image, u32 width, u32 height, u32& trackedLayout,
                        std::vector<u8>& outRgba) {
    outRgba.clear();
#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid() || image == nullptr || width == 0u || height == 0u) {
        return false;
    }

    auto vkDevice = static_cast<VkDevice>(device.nativeHandle());
    auto physicalDevice = static_cast<VkPhysicalDevice>(device.nativePhysicalDevice());
    auto queue = static_cast<VkQueue>(device.queues().graphics);
    if (queue == VK_NULL_HANDLE) {
        return false;
    }
    device.waitIdle();

    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(width) * height * 4u;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool ok = false;

    do {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = byteCount;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(vkDevice, &bufferInfo, nullptr, &staging) != VK_SUCCESS) {
            break;
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(vkDevice, staging, &requirements);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex =
            findHostReadableMemoryType(physicalDevice, requirements.memoryTypeBits);
        if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &stagingMemory) != VK_SUCCESS) {
            break;
        }
        vkBindBufferMemory(vkDevice, staging, stagingMemory, 0);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = device.queues().graphicsFamily;
        if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
            break;
        }
        VkCommandBufferAllocateInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdInfo.commandPool = pool;
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(vkDevice, &cmdInfo, &cmd) != VK_SUCCESS) {
            break;
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Tracked layout from the last recorded frame; UNDEFINED means nothing rendered yet.
        const auto restoreLayout = static_cast<VkImageLayout>(trackedLayout);
        VkImageMemoryBarrier toSrc{};
        toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSrc.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toSrc.oldLayout = restoreLayout;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image = static_cast<VkImage>(image);
        toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &toSrc);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(image), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging, 1, &region);

        const VkImageLayout finalLayout =
            restoreLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : restoreLayout;
        if (finalLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            VkImageMemoryBarrier back = toSrc;
            back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            back.newLayout = finalLayout;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &back);
        }
        VkBufferMemoryBarrier hostRead{};
        hostRead.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hostRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostRead.buffer = staging;
        hostRead.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                             &hostRead, 0, nullptr);
        vkEndCommandBuffer(cmd);

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(vkDevice, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
            break;
        }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS ||
            vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            break;
        }
        trackedLayout = static_cast<u32>(finalLayout);

        void* mapped = nullptr;
        if (vkMapMemory(vkDevice, stagingMemory, 0, byteCount, 0, &mapped) != VK_SUCCESS) {
            break;
        }
        outRgba.resize(static_cast<usize>(byteCount));
        std::memcpy(outRgba.data(), mapped, outRgba.size());
        vkUnmapMemory(vkDevice, stagingMemory);
        ok = true;
    } while (false);

    if (fence != VK_NULL_HANDLE) {
        vkDestroyFence(vkDevice, fence, nullptr);
    }
    if (pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(vkDevice, pool, nullptr);
    }
    if (staging != VK_NULL_HANDLE) {
        vkDestroyBuffer(vkDevice, staging, nullptr);
    }
    if (stagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(vkDevice, stagingMemory, nullptr);
    }
    return ok;
#else
    (void)device;
    (void)image;
    (void)width;
    (void)height;
    (void)trackedLayout;
    return false;
#endif
}

} // namespace fuse::renderer
