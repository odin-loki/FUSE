#pragma once

#include <fuse/handle_map.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>

#include <memory>

namespace fuse::renderer {

/// Handle-based GPU resource registry with bindless index assignment (B2.3).
class ResourceManager {
public:
    struct Desc {
        usize stagingRingBytes = 64u * 1024u * 1024u;
    };

    struct LiveCounts {
        u32 textures = 0;
        u32 buffers = 0;
        u32 samplers = 0;
    };

    ResourceManager() = default;
    ~ResourceManager();

    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    bool init(VulkanDevice& device, BindlessDescriptors& bindless);
    bool init(VulkanDevice& device, BindlessDescriptors& bindless, const Desc& desc);
    void destroy();

    bool isReady() const { return m_ready; }

    TextureHandle createTexture(const TextureDesc& desc, const void* initialData = nullptr);
    BufferHandle createBuffer(const BufferDesc& desc, const void* initialData = nullptr);
    SamplerHandle createSampler(const SamplerDesc& desc);

    void destroyTexture(TextureHandle handle);
    void destroyBuffer(BufferHandle handle);
    void destroySampler(SamplerHandle handle);

    Texture* getTexture(TextureHandle handle);
    const Texture* getTexture(TextureHandle handle) const;
    Buffer* getBuffer(BufferHandle handle);
    const Buffer* getBuffer(BufferHandle handle) const;

    usize stagingRingCapacity() const { return m_stagingRingCapacity; }
    usize stagingRingOffset() const { return m_stagingOffset; }

    LiveCounts liveCounts() const;
    const GpuAllocStats* allocatorStats() const;

private:
    void destroyAllResources();
    bool ensureStagingRing();

    VulkanDevice* m_device = nullptr;
    BindlessDescriptors* m_bindless = nullptr;
    std::unique_ptr<GpuAllocator> m_allocator;
    fuse::HandleMap<Texture> m_textures;
    fuse::HandleMap<Buffer> m_buffers;
    fuse::HandleMap<SamplerEntry> m_samplers;
    BufferHandle m_stagingRing{};
    usize m_stagingRingCapacity = 0;
    usize m_stagingOffset = 0;
    bool m_ready = false;
};

} // namespace fuse::renderer
