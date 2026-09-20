#include <fuse/renderer/resource_manager.hpp>

#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/gpu_alloc_stats.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {

void* packedSamplerHandle(const SamplerDesc& desc) {
    return reinterpret_cast<void*>(static_cast<u64>(desc.minFilter) |
                                   (static_cast<u64>(desc.magFilter) << 16) |
                                   (static_cast<u64>(desc.addressMode) << 32));
}

#if defined(FUSE_VULKAN_BACKEND)
void destroyOwnedVulkanSampler(VulkanDevice* device, void* handle) {
    if (device == nullptr || !device->isValid() || !bindlessNativeHandleReady(handle)) {
        return;
    }
    vkDestroySampler(static_cast<VkDevice>(device->nativeHandle()), static_cast<VkSampler>(handle),
                     nullptr);
}

void* createVulkanSampler(VulkanDevice& device, const SamplerDesc& desc) {
    if (!device.isValid() || device.nativeHandle() == nullptr) {
        return nullptr;
    }

    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = static_cast<VkFilter>(desc.magFilter);
    info.minFilter = static_cast<VkFilter>(desc.minFilter);
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    const auto addressMode = static_cast<VkSamplerAddressMode>(desc.addressMode);
    info.addressModeU = addressMode;
    info.addressModeV = addressMode;
    info.addressModeW = addressMode;
    info.mipLodBias = desc.mipLodBias;
    info.anisotropyEnable = VK_FALSE;
    info.maxAnisotropy = 1.0f;
    if (desc.anisotropy && device.info().samplerAnisotropy) {
        info.anisotropyEnable = VK_TRUE;
        const float limit = std::max(1.f, device.info().maxSamplerAnisotropy);
        info.maxAnisotropy = std::clamp(desc.maxAnisotropy, 1.f, limit);
    }
    if (desc.compareEnable) {
        info.compareEnable = VK_TRUE;
        info.compareOp = static_cast<VkCompareOp>(desc.compareOp);
    } else {
        info.compareEnable = VK_FALSE;
        info.compareOp = VK_COMPARE_OP_ALWAYS;
    }
    info.minLod = std::max(0.0f, desc.minLod);
    info.maxLod = std::max(info.minLod, desc.maxLod);
    info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    info.unnormalizedCoordinates = VK_FALSE;

    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(static_cast<VkDevice>(device.nativeHandle()), &info, nullptr, &sampler) !=
        VK_SUCCESS) {
        return nullptr;
    }
    return sampler;
}
#endif

constexpr usize kStagingAlignBytes = 256;

bool hasBufferUsage(BufferUsage usage, BufferUsage flag) {
    return (static_cast<u32>(usage) & static_cast<u32>(flag)) != 0;
}

BufferUsage withBufferUsage(BufferUsage usage, BufferUsage flag) {
    return static_cast<BufferUsage>(static_cast<u32>(usage) | static_cast<u32>(flag));
}

ImageUsage withImageUsage(ImageUsage usage, ImageUsage flag) {
    return static_cast<ImageUsage>(static_cast<u32>(usage) | static_cast<u32>(flag));
}

bool memoryNeedsHostMapping(MemoryUsage usage) {
    return usage == MemoryUsage::CpuToGpu || usage == MemoryUsage::GpuToCpu;
}

#if defined(FUSE_VULKAN_BACKEND)
struct TransferSubmitTarget {
    VkQueue queue;
    u32 family;
};

TransferSubmitTarget pickTransferTarget(VulkanDevice& device) {
    const VulkanQueues& queues = device.queues();
    if (queues.transfer != nullptr) {
        return {static_cast<VkQueue>(queues.transfer), queues.transferFamily};
    }
    return {static_cast<VkQueue>(queues.graphics), queues.graphicsFamily};
}

bool usedDedicatedTransferQueue(VulkanDevice& device) {
    const VulkanQueues& queues = device.queues();
    return queues.transfer != nullptr && queues.transfer != queues.graphics;
}

constexpr u64 kOneShotCopyFenceTimeoutNs = 1000000000ull;

struct OneShotCopyOutcome {
    bool submitted = false;
    bool usedFence = false;
    bool waitTimedOut = false;
};

OneShotCopyOutcome submitOneShotAndWait(VkDevice vkDevice, VkQueue queue, VkCommandBuffer cmd,
                                        bool requireIdleSuccess) {
    OneShotCopyOutcome out{};

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    const bool haveFence =
        vkCreateFence(vkDevice, &fenceInfo, nullptr, &fence) == VK_SUCCESS && fence != VK_NULL_HANDLE;

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    if (vkQueueSubmit(queue, 1, &submitInfo, haveFence ? fence : VK_NULL_HANDLE) != VK_SUCCESS) {
        if (haveFence) {
            vkDestroyFence(vkDevice, fence, nullptr);
        }
        return out;
    }

    if (!haveFence) {
        const VkResult idle = vkQueueWaitIdle(queue);
        out.submitted = requireIdleSuccess ? (idle == VK_SUCCESS) : true;
        return out;
    }

    const VkResult waitResult =
        vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, kOneShotCopyFenceTimeoutNs);
    if (waitResult == VK_SUCCESS) {
        out.submitted = true;
        out.usedFence = true;
    } else {
        vkQueueWaitIdle(queue);
        out.waitTimedOut = waitResult == VK_TIMEOUT;
        out.submitted = false;
    }
    vkDestroyFence(vkDevice, fence, nullptr);
    return out;
}

OneShotCopyOutcome oneShotCopyBuffer(VulkanDevice& device, void* srcHandle, void* dstHandle,
                                     usize srcOffset, usize size) {
    if (!device.isValid() || device.nativeHandle() == nullptr || size == 0) {
        return {};
    }
    if (!bindlessNativeHandleReady(srcHandle) || !bindlessNativeHandleReady(dstHandle)) {
        return {};
    }

    const TransferSubmitTarget target = pickTransferTarget(device);
    if (target.queue == VK_NULL_HANDLE) {
        return {};
    }

    const VkDevice vkDevice = static_cast<VkDevice>(device.nativeHandle());

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = target.family;

    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        return {};
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vkDevice, &allocInfo, &cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        return {};
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        return {};
    }

    VkBufferCopy region{};
    region.srcOffset = srcOffset;
    region.dstOffset = 0;
    region.size = size;
    vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(srcHandle), static_cast<VkBuffer>(dstHandle), 1,
                    &region);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        return {};
    }

    const OneShotCopyOutcome outcome = submitOneShotAndWait(vkDevice, target.queue, cmd, false);
    vkDestroyCommandPool(vkDevice, pool, nullptr);
    return outcome;
}

OneShotCopyOutcome oneShotCopyBufferToImage(VulkanDevice& device, void* srcHandle, void* dstImage,
                                            usize srcOffset, u32 width, u32 height, u32 depth) {
    if (!device.isValid() || device.nativeHandle() == nullptr || width == 0 || height == 0) {
        return {};
    }
    if (!bindlessNativeHandleReady(srcHandle) || !bindlessNativeHandleReady(dstImage)) {
        return {};
    }

    const TransferSubmitTarget target = pickTransferTarget(device);
    if (target.queue == VK_NULL_HANDLE) {
        return {};
    }

    const VkDevice vkDevice = static_cast<VkDevice>(device.nativeHandle());
    const u32 extentDepth = depth > 0 ? depth : 1u;
    const VkImage image = static_cast<VkImage>(dstImage);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = target.family;

    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        return {};
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vkDevice, &allocInfo, &cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        return {};
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        return {};
    }

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy region{};
    region.bufferOffset = srcOffset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, extentDepth};
    vkCmdCopyBufferToImage(cmd, static_cast<VkBuffer>(srcHandle), image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toShader{};
    toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.image = image;
    toShader.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toShader.subresourceRange.levelCount = 1;
    toShader.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &toShader);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        return {};
    }

    const OneShotCopyOutcome outcome = submitOneShotAndWait(vkDevice, target.queue, cmd, true);
    vkDestroyCommandPool(vkDevice, pool, nullptr);
    return outcome;
}

TransferSubmitTarget pickBlitTarget(VulkanDevice& device) {
    const VulkanQueues& queues = device.queues();
    if (queues.graphics != nullptr) {
        return {static_cast<VkQueue>(queues.graphics), queues.graphicsFamily};
    }
    return pickTransferTarget(device);
}

OneShotCopyOutcome oneShotGenerateMips(VulkanDevice& device, const Texture& texture, u32& blitCount) {
    blitCount = 0;
    if (!device.isValid() || device.nativeHandle() == nullptr) {
        return {};
    }
    if (!bindlessNativeHandleReady(texture.image) || texture.desc.mipLevels <= 1) {
        return {};
    }

    const TransferSubmitTarget target = pickBlitTarget(device);
    if (target.queue == VK_NULL_HANDLE) {
        return {};
    }

    const VkDevice vkDevice = static_cast<VkDevice>(device.nativeHandle());
    const VkImage image = static_cast<VkImage>(texture.image);
    const u32 mipLevels = texture.desc.mipLevels;
    const u32 layerCount = texture.desc.arrayLayers > 0 ? texture.desc.arrayLayers : 1u;
    i32 mipWidth = static_cast<i32>(texture.desc.width > 0 ? texture.desc.width : 1u);
    i32 mipHeight = static_cast<i32>(texture.desc.height > 0 ? texture.desc.height : 1u);
    i32 mipDepth = static_cast<i32>(texture.desc.depth > 0 ? texture.desc.depth : 1u);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = target.family;

    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        return {};
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vkDevice, &allocInfo, &cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        return {};
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        return {};
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = layerCount;
    barrier.subresourceRange.levelCount = 1;

    if (mipLevels > 1) {
        barrier.subresourceRange.baseMipLevel = 1;
        barrier.subresourceRange.levelCount = mipLevels - 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
    }

    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);

    for (u32 mip = 1; mip < mipLevels; ++mip) {
        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mipWidth, mipHeight, mipDepth};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = mip - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = layerCount;
        const i32 dstWidth = std::max(mipWidth / 2, 1);
        const i32 dstHeight = std::max(mipHeight / 2, 1);
        const i32 dstDepth = std::max(mipDepth / 2, 1);
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {dstWidth, dstHeight, dstDepth};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = mip;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = layerCount;

        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        barrier.subresourceRange.baseMipLevel = mip;
        barrier.subresourceRange.levelCount = 1;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);

        mipWidth = dstWidth;
        mipHeight = dstHeight;
        mipDepth = dstDepth;
        ++blitCount;
    }

    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        blitCount = 0;
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        return {};
    }

    const OneShotCopyOutcome outcome = submitOneShotAndWait(vkDevice, target.queue, cmd, true);
    vkDestroyCommandPool(vkDevice, pool, nullptr);
    if (!outcome.submitted) {
        blitCount = 0;
    }
    return outcome;
}
#endif

} // namespace

ResourceManager::~ResourceManager() {
    destroy();
}

bool ResourceManager::init(VulkanDevice& device, BindlessDescriptors& bindless) {
    Desc defaults{};
    return init(device, bindless, defaults);
}

bool ResourceManager::init(VulkanDevice& device, BindlessDescriptors& bindless, const Desc& desc) {
    if (m_ready) {
        return true;
    }

    m_device = &device;
    m_bindless = &bindless;
    m_stagingRingCapacity = desc.stagingRingBytes;

    m_allocator = GpuAllocator::create(device);
    if (!m_allocator || !m_allocator->isValid()) {
        return false;
    }
    m_allocator->setStatsName("fuse_rhi_gpu");

    m_ready = true;
    return ensureStagingRing();
}

void ResourceManager::destroy() {
    if (!m_ready) {
        return;
    }

    destroyAllResources();

    m_allocator.reset();
    m_device = nullptr;
    m_bindless = nullptr;
    m_stagingOffset = 0;
    m_stagingRingWrapCount = 0;
    m_ready = false;
}

void ResourceManager::destroyAllResources() {
    std::vector<SamplerHandle> samplerHandles;
    m_samplers.forEachOccupied(
        [&](SamplerHandle handle) { samplerHandles.push_back(handle); });
    for (SamplerHandle handle : samplerHandles) {
        destroySampler(handle);
    }

    std::vector<TextureHandle> textureHandles;
    m_textures.forEachOccupied(
        [&](TextureHandle handle) { textureHandles.push_back(handle); });
    for (TextureHandle handle : textureHandles) {
        destroyTexture(handle);
    }

    std::vector<BufferHandle> bufferHandles;
    m_buffers.forEachOccupied([&](BufferHandle handle) {
        if (handle != m_stagingRing) {
            bufferHandles.push_back(handle);
        }
    });
    for (BufferHandle handle : bufferHandles) {
        destroyBuffer(handle);
    }

    if (m_stagingRing.isValid() && m_allocator) {
        Buffer* staging = getBuffer(m_stagingRing);
        if (staging != nullptr) {
            if (m_bindless != nullptr && staging->bindlessIndex != UINT32_MAX) {
                m_bindless->unregisterBuffer(staging->bindlessIndex);
            }
            m_allocator->destroyBuffer(*staging);
        }
        m_buffers.remove(m_stagingRing);
        m_stagingRing = BufferHandle{};
    }

    m_textures = fuse::HandleMap<Texture>{};
    m_buffers = fuse::HandleMap<Buffer>{};
    m_samplers = fuse::HandleMap<SamplerEntry>{};
}

ResourceManager::LiveCounts ResourceManager::liveCounts() const {
    LiveCounts counts{};
    counts.textures = m_textures.size();
    counts.buffers = m_buffers.size();
    if (m_stagingRing.isValid()) {
        counts.buffers -= 1;
    }
    counts.samplers = m_samplers.size();
    return counts;
}

const GpuAllocStats* ResourceManager::allocatorStats() const {
    if (m_allocator == nullptr) {
        return nullptr;
    }
    return &m_allocator->stats();
}

TextureHandle ResourceManager::createTexture(const TextureDesc& desc, const void* initialData) {
    m_lastGpuTextureCopySubmitted = false;
    m_lastGpuTextureCopyBytes = 0;
    m_lastGpuCopyUsedTransferQueue = false;
    m_lastGpuCopyUsedFence = false;
    m_lastGpuCopyWaitTimedOut = false;

    if (!m_ready || m_allocator == nullptr) {
        return TextureHandle{};
    }

    TextureDesc allocDesc = desc;
    if (initialData != nullptr) {
        allocDesc.usage = withImageUsage(desc.usage, ImageUsage::TransferDst);
    }

    Texture texture{};
    if (!m_allocator->createImage(allocDesc, texture)) {
        return TextureHandle{};
    }

    texture.bindlessIndex = m_bindless->registerTexture(texture, false);
    if (texture.bindlessIndex == UINT32_MAX) {
        m_allocator->destroyImage(texture);
        return TextureHandle{};
    }

    if (initialData != nullptr) {
        copyTextureInitialDataViaStaging(texture, initialData);
    }
    return m_textures.insert(std::move(texture));
}

BufferHandle ResourceManager::createBuffer(const BufferDesc& desc, const void* initialData) {
    if (!m_ready || m_allocator == nullptr) {
        return BufferHandle{};
    }

    BufferDesc allocDesc = desc;
    if (initialData != nullptr && !memoryNeedsHostMapping(desc.memoryUsage)) {
        allocDesc.usage = withBufferUsage(desc.usage, BufferUsage::TransferDst);
    }

    Buffer buffer{};
    if (!m_allocator->createBuffer(allocDesc, buffer)) {
        return BufferHandle{};
    }

    buffer.bindlessIndex = m_bindless->registerBuffer(buffer);
    if (buffer.bindlessIndex == UINT32_MAX) {
        m_allocator->destroyBuffer(buffer);
        return BufferHandle{};
    }

    if (initialData != nullptr) {
        if (buffer.mapped != nullptr) {
            std::memcpy(buffer.mapped, initialData, desc.size);
        } else {
            copyInitialDataViaStaging(buffer, initialData, desc.size);
        }
    }

    return m_buffers.insert(std::move(buffer));
}

void ResourceManager::copyInitialDataViaStaging(Buffer& dest, const void* initialData, usize size) {
    m_lastGpuCopyUsedTransferQueue = false;
    m_lastGpuCopyUsedFence = false;
    m_lastGpuCopyWaitTimedOut = false;

    if (initialData == nullptr || size == 0) {
        return;
    }

    Buffer* staging = getBuffer(m_stagingRing);
    if (staging == nullptr || staging->mapped == nullptr) {
        return;
    }

    if (size > m_stagingRingCapacity) {
        return;
    }
    if (m_stagingOffset + size > m_stagingRingCapacity) {
        m_stagingOffset = 0;
        ++m_stagingRingWrapCount;
    }

    const usize srcOffset = m_stagingOffset;
    std::memcpy(static_cast<u8*>(staging->mapped) + srcOffset, initialData, size);

#if defined(FUSE_VULKAN_BACKEND)
    if (m_device != nullptr && hasBufferUsage(dest.desc.usage, BufferUsage::TransferDst)) {
        const OneShotCopyOutcome outcome =
            oneShotCopyBuffer(*m_device, staging->handle, dest.handle, srcOffset, size);
        m_lastGpuCopyUsedFence = outcome.usedFence;
        m_lastGpuCopyWaitTimedOut = outcome.waitTimedOut;
        if (outcome.submitted) {
            m_lastGpuCopyUsedTransferQueue = usedDedicatedTransferQueue(*m_device);
        }
    }
#else
    (void)dest;
#endif

    m_stagingOffset += size;
    m_stagingOffset = (m_stagingOffset + (kStagingAlignBytes - 1u)) & ~(kStagingAlignBytes - 1u);
}

void ResourceManager::copyTextureInitialDataViaStaging(Texture& dest, const void* initialData) {
    if (initialData == nullptr) {
        return;
    }

    const usize bytes = gpu_alloc_detail::estimateImageBytes(dest.desc);
    if (bytes == 0) {
        return;
    }

    Buffer* staging = getBuffer(m_stagingRing);
    if (staging == nullptr || staging->mapped == nullptr) {
        return;
    }

    if (bytes > m_stagingRingCapacity) {
        return;
    }
    if (m_stagingOffset + bytes > m_stagingRingCapacity) {
        m_stagingOffset = 0;
        ++m_stagingRingWrapCount;
    }

    const usize srcOffset = m_stagingOffset;
    std::memcpy(static_cast<u8*>(staging->mapped) + srcOffset, initialData, bytes);

#if defined(FUSE_VULKAN_BACKEND)
    if (m_device != nullptr && m_device->isValid() && bindlessNativeHandleReady(dest.image)) {
        const OneShotCopyOutcome outcome =
            oneShotCopyBufferToImage(*m_device, staging->handle, dest.image, srcOffset, dest.desc.width,
                                     dest.desc.height, dest.desc.depth);
        m_lastGpuCopyUsedFence = outcome.usedFence;
        m_lastGpuCopyWaitTimedOut = outcome.waitTimedOut;
        if (outcome.submitted) {
            m_lastGpuTextureCopySubmitted = true;
            m_lastGpuTextureCopyBytes = static_cast<u32>(bytes);
            m_lastGpuCopyUsedTransferQueue = usedDedicatedTransferQueue(*m_device);
        }
    }
#else
    (void)dest;
#endif

    m_stagingOffset += bytes;
    m_stagingOffset = (m_stagingOffset + (kStagingAlignBytes - 1u)) & ~(kStagingAlignBytes - 1u);
}

SamplerHandle ResourceManager::createSampler(const SamplerDesc& desc) {
    if (!m_ready) {
        return SamplerHandle{};
    }

    SamplerEntry entry{};
#if defined(FUSE_VULKAN_BACKEND)
    if (m_device != nullptr && m_device->isValid()) {
        entry.handle = createVulkanSampler(*m_device, desc);
        if (entry.handle == nullptr) {
            return SamplerHandle{};
        }
    } else {
        entry.handle = packedSamplerHandle(desc);
    }
#else
    entry.handle = packedSamplerHandle(desc);
#endif
    entry.bindlessIndex = m_bindless->registerSampler(entry.handle);
    if (entry.bindlessIndex == UINT32_MAX) {
#if defined(FUSE_VULKAN_BACKEND)
        destroyOwnedVulkanSampler(m_device, entry.handle);
#endif
        return SamplerHandle{};
    }

    (void)desc.name;
    return m_samplers.insert(std::move(entry));
}

bool ResourceManager::generateMips(TextureHandle handle) {
    m_lastMipGenerateCount = 0;
    m_lastMipGenerateOk = false;

    if (!m_ready) {
        return false;
    }

    Texture* texture = getTexture(handle);
    if (texture == nullptr || texture->desc.mipLevels <= 1) {
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (m_device == nullptr || !m_device->isValid() || !bindlessNativeHandleReady(texture->image)) {
        return false;
    }

    const OneShotCopyOutcome outcome =
        oneShotGenerateMips(*m_device, *texture, m_lastMipGenerateCount);
    m_lastMipGenerateOk = outcome.submitted;
    if (!m_lastMipGenerateOk) {
        m_lastMipGenerateCount = 0;
    }
    return m_lastMipGenerateOk;
#else
    (void)texture;
    return false;
#endif
}

void ResourceManager::destroyTexture(TextureHandle handle) {
    Texture* texture = getTexture(handle);
    if (texture == nullptr || m_allocator == nullptr) {
        return;
    }
    if (texture->bindlessIndex != UINT32_MAX) {
        m_bindless->unregisterTexture(texture->bindlessIndex);
    }
    m_allocator->destroyImage(*texture);
    m_textures.remove(handle);
}

void ResourceManager::destroyBuffer(BufferHandle handle) {
    if (handle == m_stagingRing) {
        return;
    }
    Buffer* buffer = getBuffer(handle);
    if (buffer == nullptr || m_allocator == nullptr) {
        return;
    }
    if (buffer->bindlessIndex != UINT32_MAX) {
        m_bindless->unregisterBuffer(buffer->bindlessIndex);
    }
    m_allocator->destroyBuffer(*buffer);
    m_buffers.remove(handle);
}

void ResourceManager::destroySampler(SamplerHandle handle) {
    SamplerEntry* sampler = m_samplers.get(handle);
    if (sampler == nullptr) {
        return;
    }
    if (sampler->bindlessIndex != UINT32_MAX) {
        m_bindless->unregisterSampler(sampler->bindlessIndex);
    }
#if defined(FUSE_VULKAN_BACKEND)
    destroyOwnedVulkanSampler(m_device, sampler->handle);
#endif
    m_samplers.remove(handle);
}

Texture* ResourceManager::getTexture(TextureHandle handle) {
    return m_textures.get(handle);
}

const Texture* ResourceManager::getTexture(TextureHandle handle) const {
    return m_textures.get(handle);
}

Buffer* ResourceManager::getBuffer(BufferHandle handle) {
    return m_buffers.get(handle);
}

const Buffer* ResourceManager::getBuffer(BufferHandle handle) const {
    return m_buffers.get(handle);
}

bool ResourceManager::ensureStagingRing() {
    if (m_stagingRing.isValid() || m_stagingRingCapacity == 0) {
        return true;
    }

    BufferDesc desc{};
    desc.size = m_stagingRingCapacity;
    desc.usage = BufferUsage::TransferSrc;
    desc.memoryUsage = MemoryUsage::CpuToGpu;
    desc.name = "staging_ring";

    m_stagingRing = createBuffer(desc);
    return m_stagingRing.isValid();
}

bool ResourceManager::readBuffer(BufferHandle handle, void* dst, usize size) {
    Buffer* src = getBuffer(handle);
    if (src == nullptr || dst == nullptr || size == 0) {
        return false;
    }

    const usize copySize = size < src->desc.size ? size : src->desc.size;
    if (copySize == 0) {
        return false;
    }

    if (src->mapped != nullptr) {
        if (m_allocator != nullptr) {
            return m_allocator->readMapped(*src, dst, copySize, 0);
        }
        std::memcpy(dst, src->mapped, copySize);
        return true;
    }

    if (!m_ready || m_allocator == nullptr || m_device == nullptr || !m_device->isValid()) {
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND)
    BufferDesc stagingDesc{};
    stagingDesc.size = copySize;
    stagingDesc.usage = BufferUsage::TransferDst;
    stagingDesc.memoryUsage = MemoryUsage::GpuToCpu;
    stagingDesc.name = "readback_staging";

    Buffer staging{};
    if (!m_allocator->createBuffer(stagingDesc, staging) || staging.mapped == nullptr) {
        m_allocator->destroyBuffer(staging);
        return false;
    }

    const OneShotCopyOutcome copy =
        oneShotCopyBuffer(*m_device, src->handle, staging.handle, 0, copySize);
    if (!copy.submitted) {
        m_allocator->destroyBuffer(staging);
        return false;
    }

    const bool copied = m_allocator->readMapped(staging, dst, copySize, 0);
    m_allocator->destroyBuffer(staging);
    return copied;
#else
    return false;
#endif
}

#if defined(FUSE_VULKAN_BACKEND)
namespace {

OneShotCopyOutcome oneShotCopyImageToBuffer(VulkanDevice& device, void* srcImage, void* dstHandle,
                                            u32 width, u32 height, u32 depth, u32 arrayLayers) {
    if (!device.isValid() || device.nativeHandle() == nullptr || width == 0 || height == 0) {
        return {};
    }
    if (!bindlessNativeHandleReady(srcImage) || !bindlessNativeHandleReady(dstHandle)) {
        return {};
    }

    const TransferSubmitTarget target = pickTransferTarget(device);
    if (target.queue == VK_NULL_HANDLE) {
        return {};
    }

    const VkDevice vkDevice = static_cast<VkDevice>(device.nativeHandle());
    const u32 extentDepth = depth > 0 ? depth : 1u;
    const u32 layerCount = arrayLayers > 0 ? arrayLayers : 1u;
    const VkImage image = static_cast<VkImage>(srcImage);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = target.family;

    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        return {};
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vkDevice, &allocInfo, &cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        return {};
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        return {};
    }

    VkImageMemoryBarrier toTransferSrc{};
    toTransferSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransferSrc.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toTransferSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransferSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toTransferSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransferSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferSrc.image = image;
    toTransferSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransferSrc.subresourceRange.levelCount = 1;
    toTransferSrc.subresourceRange.layerCount = layerCount;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toTransferSrc);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.layerCount = layerCount;
    region.imageExtent = {width, height, extentDepth};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           static_cast<VkBuffer>(dstHandle), 1, &region);

    VkBufferMemoryBarrier toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = static_cast<VkBuffer>(dstHandle);
    toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                         1, &toHost, 0, nullptr);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        return {};
    }

    const OneShotCopyOutcome outcome = submitOneShotAndWait(vkDevice, target.queue, cmd, true);
    vkDestroyCommandPool(vkDevice, pool, nullptr);
    return outcome;
}

} // namespace
#endif

bool ResourceManager::readTexture(TextureHandle handle, void* dst, usize size) {
    m_lastTextureReadbackBytes = 0;

    const Texture* texture = getTexture(handle);
    if (texture == nullptr || dst == nullptr || size == 0) {
        return false;
    }

    const u32 width = texture->desc.width > 0 ? texture->desc.width : 1u;
    const u32 height = texture->desc.height > 0 ? texture->desc.height : 1u;
    const u32 depth = texture->desc.depth > 0 ? texture->desc.depth : 1u;
    const u32 layers = texture->desc.arrayLayers > 0 ? texture->desc.arrayLayers : 1u;
    const usize needed =
        static_cast<usize>(width) * static_cast<usize>(height) * 4u * static_cast<usize>(depth) *
        static_cast<usize>(layers);
    if (needed == 0) {
        return false;
    }
    const usize copySize = size < needed ? size : needed;

    if (!m_ready || m_allocator == nullptr || m_device == nullptr || !m_device->isValid()) {
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (!bindlessNativeHandleReady(texture->image)) {
        return false;
    }

    BufferDesc stagingDesc{};
    stagingDesc.size = needed;
    stagingDesc.usage = BufferUsage::TransferDst;
    stagingDesc.memoryUsage = MemoryUsage::GpuToCpu;
    stagingDesc.name = "texture_readback_staging";

    Buffer staging{};
    if (!m_allocator->createBuffer(stagingDesc, staging) || staging.mapped == nullptr) {
        m_allocator->destroyBuffer(staging);
        return false;
    }

    const OneShotCopyOutcome copy =
        oneShotCopyImageToBuffer(*m_device, texture->image, staging.handle, width, height, depth,
                                 layers);
    if (!copy.submitted) {
        m_allocator->destroyBuffer(staging);
        return false;
    }

    std::memcpy(dst, staging.mapped, copySize);
    m_lastTextureReadbackBytes = static_cast<u32>(copySize);
    m_allocator->destroyBuffer(staging);
    return true;
#else
    (void)copySize;
    return false;
#endif
}

} // namespace fuse::renderer
