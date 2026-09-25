#include <fuse/renderer/rg/legacy_barriers.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer::rg {

void recordLegacyImageBarrier(void* commandBuffer, void* image, u32 oldLayout, u32 newLayout, u32 aspectMask,
                              u32 srcStages, u32 srcAccess, u32 dstStages, u32 dstAccess, u32 baseMip,
                              u32 levelCount, u32 baseLayer, u32 layerCount, u32 srcQueueFamily,
                              u32 dstQueueFamily) {
#if defined(FUSE_VULKAN_BACKEND)
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = static_cast<VkImageLayout>(oldLayout);
    barrier.newLayout = static_cast<VkImageLayout>(newLayout);
    barrier.srcQueueFamilyIndex = srcQueueFamily;
    barrier.dstQueueFamilyIndex = dstQueueFamily;
    barrier.image = static_cast<VkImage>(image);
    barrier.subresourceRange.aspectMask = aspectMask;
    barrier.subresourceRange.baseMipLevel = baseMip;
    barrier.subresourceRange.levelCount = levelCount == 0u ? 1u : levelCount;
    barrier.subresourceRange.baseArrayLayer = baseLayer;
    barrier.subresourceRange.layerCount = layerCount == 0u ? 1u : layerCount;
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
    (void)baseMip;
    (void)levelCount;
    (void)baseLayer;
    (void)layerCount;
    (void)srcQueueFamily;
    (void)dstQueueFamily;
#endif
}

void recordLegacyBufferBarrier(void* commandBuffer, void* buffer, u32 srcStages, u32 srcAccess, u32 dstStages,
                               u32 dstAccess, u32 srcQueueFamily, u32 dstQueueFamily) {
#if defined(FUSE_VULKAN_BACKEND)
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = srcQueueFamily;
    barrier.dstQueueFamilyIndex = dstQueueFamily;
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
    (void)srcQueueFamily;
    (void)dstQueueFamily;
#endif
}

} // namespace fuse::renderer::rg
