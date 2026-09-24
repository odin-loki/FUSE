#include <fuse/renderer/rg/legacy_barriers.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer::rg {

void recordLegacyImageBarrier(void* commandBuffer, void* image, u32 oldLayout, u32 newLayout, u32 aspectMask,
                              u32 srcStages, u32 srcAccess, u32 dstStages, u32 dstAccess) {
#if defined(FUSE_VULKAN_BACKEND)
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = static_cast<VkImageLayout>(oldLayout);
    barrier.newLayout = static_cast<VkImageLayout>(newLayout);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = static_cast<VkImage>(image);
    barrier.subresourceRange.aspectMask = aspectMask;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(static_cast<VkCommandBuffer>(commandBuffer), srcStages, dstStages, 0, 0, nullptr, 0, nullptr,
                         1, &barrier);
#else
    (void)commandBuffer;
    (void)image;
    (void)oldLayout;
    (void)newLayout;
    (void)aspectMask;
    (void)srcStages;
    (void)srcAccess;
    (void)dstStages;
    (void)dstAccess;
#endif
}

void recordLegacyBufferBarrier(void* commandBuffer, void* buffer, u32 srcStages, u32 srcAccess, u32 dstStages,
                               u32 dstAccess) {
#if defined(FUSE_VULKAN_BACKEND)
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = static_cast<VkBuffer>(buffer);
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(static_cast<VkCommandBuffer>(commandBuffer), srcStages, dstStages, 0, 0, nullptr, 1, &barrier,
                         0, nullptr);
#else
    (void)commandBuffer;
    (void)buffer;
    (void)srcStages;
    (void)srcAccess;
    (void)dstStages;
    (void)dstAccess;
#endif
}

} // namespace fuse::renderer::rg
