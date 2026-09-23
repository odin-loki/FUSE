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

// VkImageLayout values (as stored in Texture::layout) used outside the Vulkan-only paths.
constexpr u32 kImageLayoutShaderReadOnly = 5u; // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL

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

    // Resources rest on the graphics family: uploads from a dedicated transfer family hand
    // ownership back to graphics, so a copy on the transfer family would read contents the spec
    // leaves undefined (no ownership transfer). Read back on graphics; transfer only as fallback.
    const VulkanQueues& queues = device.queues();
    const TransferSubmitTarget target = queues.graphics != nullptr
                                            ? TransferSubmitTarget{static_cast<VkQueue>(queues.graphics),
                                                                   queues.graphicsFamily}
                                            : pickTransferTarget(device);
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

TransferSubmitTarget pickBlitTarget(VulkanDevice& device) {
    const VulkanQueues& queues = device.queues();
    if (queues.graphics != nullptr) {
        return {static_cast<VkQueue>(queues.graphics), queues.graphicsFamily};
    }
    return pickTransferTarget(device);
}

/// Layout barrier over mips [baseMip, baseMip + mipCount) of every layer. A defined `oldLayout` may
/// hold writes from earlier submissions (uploads, blits, external passes), so it waits on all
/// commands and makes every write available; UNDEFINED only needs the execution dependency.
void recordImageLayoutBarrier(VkCommandBuffer cmd, VkImage image, u32 baseMip, u32 mipCount, u32 layerCount,
                              VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags dstAccess,
                              VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = baseMip;
    barrier.subresourceRange.levelCount = mipCount;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = layerCount;
    barrier.srcAccessMask = oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0u : VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

VkImageLayout restingLayoutFor(const Texture& texture) {
    return (static_cast<u32>(texture.desc.usage) & static_cast<u32>(ImageUsage::Sampled)) != 0u
               ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
               : VK_IMAGE_LAYOUT_GENERAL;
}

OneShotCopyOutcome oneShotGenerateMips(VulkanDevice& device, Texture& texture, u32& blitCount) {
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

    // Mips 1.. are overwritten by the blits; mip 0 keeps its contents, so it leaves its real
    // layout (an UNDEFINED old layout would allow the driver to discard uploaded texels).
    recordImageLayoutBarrier(cmd, image, 1, mipLevels - 1, layerCount,
                             static_cast<VkImageLayout>(texture.mipTailLayout),
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT);
    recordImageLayoutBarrier(cmd, image, 0, 1, layerCount, static_cast<VkImageLayout>(texture.layout),
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = layerCount;
    barrier.subresourceRange.levelCount = 1;

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

    const VkImageLayout finalLayout = restingLayoutFor(texture);
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = finalLayout;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
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
    } else {
        texture.layout = static_cast<u32>(finalLayout);
        texture.mipTailLayout = static_cast<u32>(finalLayout);
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
    if (!ensureStagingRing()) {
        return false;
    }
    if (m_stagingRing.isValid()) {
        const Buffer* staging = getBuffer(m_stagingRing);
        if (staging != nullptr) {
            m_uploads.init(m_device, staging->handle, staging->mapped, m_stagingRingCapacity);
        }
    }
    return true;
}

void ResourceManager::destroy() {
    if (!m_ready) {
        return;
    }

    // In-flight copies still read the staging ring and write live resources.
    m_uploads.destroy();
    for (DeferredDestroy<Texture>& entry : m_deferredTextures) {
        releaseTexture(entry.resource);
    }
    for (DeferredDestroy<Buffer>& entry : m_deferredBuffers) {
        releaseBuffer(entry.resource);
    }
    m_destroyStats.retired += static_cast<u32>(m_deferredTextures.size() + m_deferredBuffers.size());
    m_deferredTextures.clear();
    m_deferredBuffers.clear();
    destroyAllResources();

    m_allocator.reset();
    m_device = nullptr;
    m_bindless = nullptr;
    m_lastUploadTicket = UploadTicket{};
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
    collectDeferredDestroys();

    TextureDesc allocDesc = desc;
    if (initialData != nullptr) {
        allocDesc.usage = withImageUsage(desc.usage, ImageUsage::TransferDst);
    }

    Texture texture{};
    if (!m_allocator->createImage(allocDesc, texture)) {
        return TextureHandle{};
    }

    // Sampled textures bind through the sampled-image array; storage-only textures through the
    // storage-image array (the GPU write is skipped when the view lacks the matching usage bit).
    const u32 imageUsageBits = static_cast<u32>(texture.desc.usage);
    const bool storageOnly = (imageUsageBits & static_cast<u32>(ImageUsage::Storage)) != 0u &&
                             (imageUsageBits & static_cast<u32>(ImageUsage::Sampled)) == 0u;
    texture.bindlessIndex = m_bindless->registerTexture(texture, storageOnly);
    if (texture.bindlessIndex == UINT32_MAX) {
        m_allocator->destroyImage(texture);
        return TextureHandle{};
    }

    if (initialData != nullptr) {
        // Submitted now (no CPU wait): later graphics-queue work is ordered after the copy.
        stageTextureUpload(texture, initialData);
        m_lastUploadTicket = m_uploads.flush();
    }
    return m_textures.insert(std::move(texture));
}

BufferHandle ResourceManager::createBuffer(const BufferDesc& desc, const void* initialData) {
    if (!m_ready || m_allocator == nullptr) {
        return BufferHandle{};
    }
    collectDeferredDestroys();

    BufferDesc allocDesc = desc;
    if (initialData != nullptr && !memoryNeedsHostMapping(desc.memoryUsage)) {
        allocDesc.usage = withBufferUsage(desc.usage, BufferUsage::TransferDst);
    }

    Buffer buffer{};
    if (!m_allocator->createBuffer(allocDesc, buffer)) {
        return BufferHandle{};
    }

    // Uniform-only buffers bind through the UBO heap; everything else uses the storage heap
    // (the GPU write is skipped when the buffer lacks the matching usage bit).
    const u32 usageBits = static_cast<u32>(buffer.desc.usage);
    const bool uniformOnly = (usageBits & static_cast<u32>(BufferUsage::Uniform)) != 0u &&
                             (usageBits & static_cast<u32>(BufferUsage::Storage)) == 0u;
    buffer.bindlessIndex = m_bindless->registerBuffer(buffer, uniformOnly);
    if (buffer.bindlessIndex == UINT32_MAX) {
        m_allocator->destroyBuffer(buffer);
        return BufferHandle{};
    }

    if (initialData != nullptr) {
        if (buffer.mapped != nullptr) {
            std::memcpy(buffer.mapped, initialData, desc.size);
        } else {
            stageBufferUpload(buffer, initialData, desc.size, 0);
            m_lastUploadTicket = m_uploads.flush();
        }
    }

    return m_buffers.insert(std::move(buffer));
}

UploadTicket ResourceManager::stageBufferUpload(Buffer& dest, const void* data, usize size, usize dstOffset) {
    m_lastGpuCopyUsedTransferQueue = false;
    m_lastGpuCopyUsedFence = false;
    m_lastGpuCopyWaitTimedOut = false;

    if (data == nullptr || size == 0 || dstOffset > dest.desc.size || size > dest.desc.size - dstOffset) {
        return UploadTicket{};
    }
    if (dest.mapped != nullptr) {
        std::memcpy(static_cast<u8*>(dest.mapped) + dstOffset, data, size);
        return UploadTicket{0, true};
    }
    if (!hasBufferUsage(dest.desc.usage, BufferUsage::TransferDst)) {
        return UploadTicket{};
    }

    usize srcOffset = 0;
    if (!m_uploads.stage(data, size, srcOffset)) {
        m_lastGpuCopyWaitTimedOut = m_uploads.lastStageTimedOut();
        return UploadTicket{};
    }
    const UploadTicket ticket = m_uploads.pendingTicket();
    if (m_uploads.recordBufferCopy(dest.handle, srcOffset, dstOffset, size)) {
        m_lastGpuCopyUsedFence = true;
        m_lastGpuCopyUsedTransferQueue = m_uploads.stats().dedicatedTransferQueue;
    }
    dest.lastUploadSerial = ticket.serial;
    m_lastUploadTicket = ticket;
    return ticket;
}

UploadTicket ResourceManager::stageTextureUpload(Texture& dest, const void* data) {
    m_lastGpuTextureCopySubmitted = false;
    m_lastGpuTextureCopyBytes = 0;
    m_lastGpuCopyUsedTransferQueue = false;
    m_lastGpuCopyUsedFence = false;
    m_lastGpuCopyWaitTimedOut = false;

    if (data == nullptr) {
        return UploadTicket{};
    }
    // Mip 0 of layer 0 only: the caller's data holds exactly that level.
    const usize bytes = gpu_alloc_detail::mipLevelBytes(dest.desc, 0);
    if (bytes == 0) {
        return UploadTicket{};
    }

    usize srcOffset = 0;
    if (!m_uploads.stage(data, bytes, srcOffset)) {
        m_lastGpuCopyWaitTimedOut = m_uploads.lastStageTimedOut();
        return UploadTicket{};
    }
    const UploadTicket ticket = m_uploads.pendingTicket();
    if (bindlessNativeHandleReady(dest.image) &&
        m_uploads.recordImageCopy(dest.image, srcOffset, dest.desc.width, dest.desc.height, dest.desc.depth)) {
        m_lastGpuTextureCopySubmitted = true;
        m_lastGpuTextureCopyBytes = static_cast<u32>(bytes);
        m_lastGpuCopyUsedFence = true;
        m_lastGpuCopyUsedTransferQueue = m_uploads.stats().dedicatedTransferQueue;
        // The recorded copy ends with mip 0 in SHADER_READ_ONLY_OPTIMAL (upload_queue.cpp).
        dest.layout = kImageLayoutShaderReadOnly;
    }
    dest.lastUploadSerial = ticket.serial;
    m_lastUploadTicket = ticket;
    return ticket;
}

UploadTicket ResourceManager::uploadBuffer(BufferHandle handle, const void* data, usize size, usize dstOffset) {
    Buffer* buffer = getBuffer(handle);
    if (!m_ready || buffer == nullptr || handle == m_stagingRing) {
        return UploadTicket{};
    }
    return stageBufferUpload(*buffer, data, size, dstOffset);
}

UploadTicket ResourceManager::uploadTexture(TextureHandle handle, const void* data) {
    Texture* texture = getTexture(handle);
    if (!m_ready || texture == nullptr) {
        return UploadTicket{};
    }
    return stageTextureUpload(*texture, data);
}

UploadTicket ResourceManager::flushUploads() {
    const UploadTicket ticket = m_uploads.flush();
    collectDeferredDestroys();
    return ticket;
}

bool ResourceManager::isUploadComplete(UploadTicket ticket) {
    const bool complete = m_uploads.isComplete(ticket);
    collectDeferredDestroys();
    return complete;
}

bool ResourceManager::waitUpload(UploadTicket ticket, u64 timeoutNs) {
    const bool ok = m_uploads.wait(ticket, timeoutNs);
    collectDeferredDestroys();
    return ok;
}

bool ResourceManager::waitAllUploads(u64 timeoutNs) {
    const bool ok = m_uploads.waitAll(timeoutNs);
    collectDeferredDestroys();
    return ok;
}

u32 ResourceManager::retireUploads() {
    const u32 retired = m_uploads.retireCompleted();
    collectDeferredDestroys();
    return retired;
}

bool ResourceManager::waitUploadSerial(u64 serial) {
    if (serial == 0 || serial <= m_uploads.completedSerial()) {
        return true;
    }
    return m_uploads.wait(UploadTicket{serial, true});
}

bool ResourceManager::uploadInFlight(u64 serial) {
    if (serial == 0 || serial <= m_uploads.completedSerial()) {
        return false;
    }
    return !m_uploads.isComplete(UploadTicket{serial, true});
}

u32 ResourceManager::collectDeferredDestroys() {
    if (m_deferredTextures.empty() && m_deferredBuffers.empty()) {
        return 0;
    }
    m_uploads.retireCompleted();
    const u64 completed = m_uploads.completedSerial();
    u32 released = 0;
    for (auto it = m_deferredTextures.begin(); it != m_deferredTextures.end();) {
        if (it->serial <= completed) {
            releaseTexture(it->resource);
            it = m_deferredTextures.erase(it);
            ++released;
        } else {
            ++it;
        }
    }
    for (auto it = m_deferredBuffers.begin(); it != m_deferredBuffers.end();) {
        if (it->serial <= completed) {
            releaseBuffer(it->resource);
            it = m_deferredBuffers.erase(it);
            ++released;
        } else {
            ++it;
        }
    }
    m_destroyStats.retired += released;
    return released;
}

void ResourceManager::releaseTexture(Texture& texture) {
    if (m_bindless != nullptr && texture.bindlessIndex != UINT32_MAX) {
        m_bindless->unregisterTexture(texture.bindlessIndex);
    }
    if (m_allocator != nullptr) {
        m_allocator->destroyImage(texture);
    }
}

void ResourceManager::releaseBuffer(Buffer& buffer) {
    if (m_bindless != nullptr && buffer.bindlessIndex != UINT32_MAX) {
        m_bindless->unregisterBuffer(buffer.bindlessIndex);
    }
    if (m_allocator != nullptr) {
        m_allocator->destroyBuffer(buffer);
    }
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

    // The blit runs on the graphics queue and reads mip 0: its upload (possibly on a dedicated
    // transfer queue) must have landed. Other in-flight uploads are left alone.
    if (!waitUploadSerial(texture->lastUploadSerial)) {
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
    Texture owned = *texture;
    m_textures.remove(handle);
    if (uploadInFlight(owned.lastUploadSerial)) {
        // A recorded but unsubmitted copy must reach the GPU while the image is still alive.
        if (owned.lastUploadSerial == m_uploads.pendingTicket().serial) {
            m_uploads.flush();
        }
        m_deferredTextures.push_back(DeferredDestroy<Texture>{owned.lastUploadSerial, owned});
        ++m_destroyStats.deferred;
        collectDeferredDestroys();
        return;
    }
    releaseTexture(owned);
    ++m_destroyStats.immediate;
    collectDeferredDestroys();
}

void ResourceManager::destroyBuffer(BufferHandle handle) {
    if (handle == m_stagingRing) {
        return;
    }
    Buffer* buffer = getBuffer(handle);
    if (buffer == nullptr || m_allocator == nullptr) {
        return;
    }
    Buffer owned = *buffer;
    m_buffers.remove(handle);
    if (uploadInFlight(owned.lastUploadSerial)) {
        if (owned.lastUploadSerial == m_uploads.pendingTicket().serial) {
            m_uploads.flush();
        }
        m_deferredBuffers.push_back(DeferredDestroy<Buffer>{owned.lastUploadSerial, owned});
        ++m_destroyStats.deferred;
        collectDeferredDestroys();
        return;
    }
    releaseBuffer(owned);
    ++m_destroyStats.immediate;
    collectDeferredDestroys();
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
    if (!waitUploadSerial(src->lastUploadSerial)) {
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

struct ImageReadbackRegion {
    u32 mipLevel = 0;
    u32 width = 1;
    u32 height = 1;
    u32 depth = 1;
    u32 layerCount = 1;
    /// Mips sharing the tracked layout of `mipLevel` (all move together so tracking stays exact).
    u32 rangeBaseMip = 0;
    u32 rangeMipCount = 1;
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout restoreLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
};

OneShotCopyOutcome oneShotCopyImageToBuffer(VulkanDevice& device, void* srcImage, void* dstHandle,
                                            const ImageReadbackRegion& region) {
    if (!device.isValid() || device.nativeHandle() == nullptr || region.width == 0 || region.height == 0) {
        return {};
    }
    if (!bindlessNativeHandleReady(srcImage) || !bindlessNativeHandleReady(dstHandle)) {
        return {};
    }

    // Images rest on the graphics family (uploads release them there), so read back on it.
    const TransferSubmitTarget target = pickBlitTarget(device);
    if (target.queue == VK_NULL_HANDLE) {
        return {};
    }

    const VkDevice vkDevice = static_cast<VkDevice>(device.nativeHandle());
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

    recordImageLayoutBarrier(cmd, image, region.rangeBaseMip, region.rangeMipCount, region.layerCount,
                             region.oldLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = region.mipLevel;
    copy.imageSubresource.layerCount = region.layerCount;
    copy.imageExtent = {region.width, region.height, region.depth};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VkBuffer>(dstHandle), 1,
                           &copy);

    VkBufferMemoryBarrier toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = static_cast<VkBuffer>(dstHandle);
    toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                         &toHost, 0, nullptr);

    if (region.restoreLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        VkImageMemoryBarrier restore{};
        restore.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        restore.srcAccessMask = 0; // the copy only read the image
        restore.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        restore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        restore.newLayout = region.restoreLayout;
        restore.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        restore.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        restore.image = image;
        restore.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        restore.subresourceRange.baseMipLevel = region.rangeBaseMip;
        restore.subresourceRange.levelCount = region.rangeMipCount;
        restore.subresourceRange.layerCount = region.layerCount;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &restore);
    }

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
    return readTexture(handle, dst, size, 0u);
}

bool ResourceManager::readTexture(TextureHandle handle, void* dst, usize size, u32 mipLevel) {
    m_lastTextureReadbackBytes = 0;

    Texture* texture = getTexture(handle);
    if (texture == nullptr || dst == nullptr || size == 0) {
        return false;
    }
    const u32 mipLevels = texture->desc.mipLevels > 0 ? texture->desc.mipLevels : 1u;
    if (mipLevel >= mipLevels) {
        return false;
    }

    const auto mipExtent = [mipLevel](u32 extent) {
        return std::max((extent > 0 ? extent : 1u) >> mipLevel, 1u);
    };
    const u32 width = mipExtent(texture->desc.width);
    const u32 height = mipExtent(texture->desc.height);
    const u32 depth = mipExtent(texture->desc.depth);
    const u32 layers = texture->desc.arrayLayers > 0 ? texture->desc.arrayLayers : 1u;
    const usize needed = gpu_alloc_detail::mipLevelBytes(texture->desc, mipLevel) * static_cast<usize>(layers);
    if (needed == 0) {
        return false;
    }
    const usize copySize = size < needed ? size : needed;

    if (!m_ready || m_allocator == nullptr || m_device == nullptr || !m_device->isValid()) {
        return false;
    }
    if (!waitUploadSerial(texture->lastUploadSerial)) {
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

    u32& trackedLayout = mipLevel == 0 ? texture->layout : texture->mipTailLayout;
    ImageReadbackRegion region{};
    region.mipLevel = mipLevel;
    region.width = width;
    region.height = height;
    region.depth = depth;
    region.layerCount = layers;
    region.rangeBaseMip = mipLevel == 0 ? 0u : 1u;
    region.rangeMipCount = mipLevel == 0 ? 1u : mipLevels - 1u;
    region.oldLayout = static_cast<VkImageLayout>(trackedLayout);
    region.restoreLayout = region.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                                                          : region.oldLayout;

    const OneShotCopyOutcome copy = oneShotCopyImageToBuffer(*m_device, texture->image, staging.handle, region);
    if (!copy.submitted) {
        m_allocator->destroyBuffer(staging);
        return false;
    }
    trackedLayout = static_cast<u32>(region.restoreLayout);

    const bool copied = m_allocator->readMapped(staging, dst, copySize, 0);
    if (copied) {
        m_lastTextureReadbackBytes = static_cast<u32>(copySize);
    }
    m_allocator->destroyBuffer(staging);
    return copied;
#else
    (void)copySize;
    (void)width;
    (void)height;
    (void)depth;
    return false;
#endif
}

u32 ResourceManager::textureLayout(TextureHandle handle, u32 mipLevel) const {
    const Texture* texture = getTexture(handle);
    if (texture == nullptr) {
        return 0u;
    }
    return mipLevel == 0 ? texture->layout : texture->mipTailLayout;
}

void ResourceManager::setTextureLayout(TextureHandle handle, u32 layout) {
    Texture* texture = getTexture(handle);
    if (texture == nullptr) {
        return;
    }
    texture->layout = layout;
    texture->mipTailLayout = layout;
}

} // namespace fuse::renderer
