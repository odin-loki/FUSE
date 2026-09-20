#include <fuse/renderer/resource_manager.hpp>

#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/gpu_alloc_stats.hpp>

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
    info.mipLodBias = 0.0f;
    info.anisotropyEnable = VK_FALSE;
    info.maxAnisotropy = 1.0f;
    info.compareEnable = VK_FALSE;
    info.compareOp = VK_COMPARE_OP_ALWAYS;
    info.minLod = 0.0f;
    info.maxLod = VK_LOD_CLAMP_NONE;
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

void stageTextureInitialData(Buffer* staging, usize& stagingOffset, usize stagingCapacity,
                             const TextureDesc& desc, const void* initialData) {
    if (staging == nullptr || staging->mapped == nullptr || initialData == nullptr) {
        return;
    }

    const usize bytes = gpu_alloc_detail::estimateImageBytes(desc);
    if (bytes == 0 || bytes > stagingCapacity) {
        return;
    }
    if (stagingOffset + bytes > stagingCapacity) {
        stagingOffset = 0;
    }

    std::memcpy(static_cast<u8*>(staging->mapped) + stagingOffset, initialData, bytes);
    stagingOffset += bytes;
}

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
    if (!m_ready || m_allocator == nullptr) {
        return TextureHandle{};
    }

    Texture texture{};
    if (!m_allocator->createImage(desc, texture)) {
        return TextureHandle{};
    }

    texture.bindlessIndex = m_bindless->registerTexture(texture, false);
    if (texture.bindlessIndex == UINT32_MAX) {
        m_allocator->destroyImage(texture);
        return TextureHandle{};
    }

    stageTextureInitialData(getBuffer(m_stagingRing), m_stagingOffset, m_stagingRingCapacity, desc,
                            initialData);
    return m_textures.insert(std::move(texture));
}

BufferHandle ResourceManager::createBuffer(const BufferDesc& desc, const void* initialData) {
    if (!m_ready || m_allocator == nullptr) {
        return BufferHandle{};
    }

    Buffer buffer{};
    if (!m_allocator->createBuffer(desc, buffer)) {
        return BufferHandle{};
    }

    buffer.bindlessIndex = m_bindless->registerBuffer(buffer);
    if (buffer.bindlessIndex == UINT32_MAX) {
        m_allocator->destroyBuffer(buffer);
        return BufferHandle{};
    }

    if (initialData != nullptr && buffer.mapped != nullptr) {
        std::memcpy(buffer.mapped, initialData, desc.size);
    }

    return m_buffers.insert(std::move(buffer));
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

} // namespace fuse::renderer
