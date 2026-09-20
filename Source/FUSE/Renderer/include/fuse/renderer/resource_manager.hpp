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
    bool generateMips(TextureHandle handle);

    void destroyTexture(TextureHandle handle);
    void destroyBuffer(BufferHandle handle);
    void destroySampler(SamplerHandle handle);

    Texture* getTexture(TextureHandle handle);
    const Texture* getTexture(TextureHandle handle) const;
    Buffer* getBuffer(BufferHandle handle);
    const Buffer* getBuffer(BufferHandle handle) const;
    bool readBuffer(BufferHandle handle, void* dst, usize size);
    bool readTexture(TextureHandle handle, void* dst, usize size);

    usize stagingRingCapacity() const { return m_stagingRingCapacity; }
    usize stagingRingOffset() const { return m_stagingOffset; }
    u32 stagingRingWrapCount() const { return m_stagingRingWrapCount; }

    bool lastGpuTextureCopySubmitted() const { return m_lastGpuTextureCopySubmitted; }
    u32 lastGpuTextureCopyBytes() const { return m_lastGpuTextureCopyBytes; }
    bool lastGpuCopyUsedTransferQueue() const { return m_lastGpuCopyUsedTransferQueue; }
    bool lastGpuCopyUsedFence() const { return m_lastGpuCopyUsedFence; }
    bool lastGpuCopyWaitTimedOut() const { return m_lastGpuCopyWaitTimedOut; }
    u32 lastMipGenerateCount() const { return m_lastMipGenerateCount; }
    bool lastMipGenerateOk() const { return m_lastMipGenerateOk; }
    u32 lastTextureReadbackBytes() const { return m_lastTextureReadbackBytes; }

    LiveCounts liveCounts() const;
    const GpuAllocStats* allocatorStats() const;

private:
    void destroyAllResources();
    bool ensureStagingRing();
    void copyInitialDataViaStaging(Buffer& dest, const void* initialData, usize size);
    void copyTextureInitialDataViaStaging(Texture& dest, const void* initialData);

    VulkanDevice* m_device = nullptr;
    BindlessDescriptors* m_bindless = nullptr;
    std::unique_ptr<GpuAllocator> m_allocator;
    fuse::HandleMap<Texture> m_textures;
    fuse::HandleMap<Buffer> m_buffers;
    fuse::HandleMap<SamplerEntry> m_samplers;
    BufferHandle m_stagingRing{};
    usize m_stagingRingCapacity = 0;
    usize m_stagingOffset = 0;
    u32 m_stagingRingWrapCount = 0;
    bool m_lastGpuTextureCopySubmitted = false;
    u32 m_lastGpuTextureCopyBytes = 0;
    bool m_lastGpuCopyUsedTransferQueue = false;
    bool m_lastGpuCopyUsedFence = false;
    bool m_lastGpuCopyWaitTimedOut = false;
    u32 m_lastMipGenerateCount = 0;
    bool m_lastMipGenerateOk = false;
    u32 m_lastTextureReadbackBytes = 0;
    bool m_ready = false;
};

} // namespace fuse::renderer
