#include <fuse/renderer/resource_manager.hpp>

#include <fuse/renderer/vk/allocator.hpp>

#include <cstring>

namespace fuse::renderer {

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

    m_ready = true;
    return ensureStagingRing();
}

void ResourceManager::destroy() {
    if (!m_ready) {
        return;
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
    m_allocator.reset();
    m_device = nullptr;
    m_bindless = nullptr;
    m_stagingOffset = 0;
    m_ready = false;
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

    (void)initialData;
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
    entry.handle = reinterpret_cast<void*>(static_cast<u64>(desc.minFilter) |
                                           (static_cast<u64>(desc.magFilter) << 16) |
                                           (static_cast<u64>(desc.addressMode) << 32));
    entry.bindlessIndex = m_bindless->registerSampler(entry.handle);
    if (entry.bindlessIndex == UINT32_MAX) {
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
