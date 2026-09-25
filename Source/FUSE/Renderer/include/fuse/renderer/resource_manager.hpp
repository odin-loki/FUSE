#pragma once

#include <fuse/handle_map.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

#include <deque>
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

    /// Deferred-destroy bookkeeping: a texture/buffer destroyed while an upload into it is still in
    /// flight is released only after that upload's batch retires (no CPU wait in destroy).
    struct DestroyStats {
        /// Released at once (no upload in flight).
        u32 immediate = 0;
        /// Queued behind an in-flight upload serial.
        u32 deferred = 0;
        /// Deferred entries released after their upload retired.
        u32 retired = 0;
        /// Destroys that had to wait on an upload fence (always 0: kept to prove it).
        u32 fenceWaits = 0;
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
    /// Blits mip i-1 -> mip i (linear filter) for the whole chain, transitioning from each level's
    /// tracked layout (so uploaded mip 0 data is preserved); leaves every level SHADER_READ_ONLY.
    bool generateMips(TextureHandle handle);

    /// The handle is invalid on return. If an upload into the resource is still in flight, the GPU
    /// object is released once that upload's batch retires (see `collectDeferredDestroys`).
    void destroyTexture(TextureHandle handle);
    void destroyBuffer(BufferHandle handle);
    void destroySampler(SamplerHandle handle);

    Texture* getTexture(TextureHandle handle);
    const Texture* getTexture(TextureHandle handle) const;
    Buffer* getBuffer(BufferHandle handle);
    const Buffer* getBuffer(BufferHandle handle) const;
    bool readBuffer(BufferHandle handle, void* dst, usize size);
    /// Reads mip 0 (every array layer); texel size follows the texture format.
    bool readTexture(TextureHandle handle, void* dst, usize size);
    /// Copy mip 0 from `src` to `dst`. Both images must already be in shader-read layout and share extent and format.
    bool copyTexture(TextureHandle src, TextureHandle dst);
    /// Reads one mip level (every array layer). The level is returned to its prior layout
    /// (TRANSFER_SRC_OPTIMAL when it was UNDEFINED).
    bool readTexture(TextureHandle handle, void* dst, usize size, u32 mipLevel);

    /// Tracked `VkImageLayout` (as u32) of `mipLevel`; 0 (UNDEFINED) for unknown handles.
    u32 textureLayout(TextureHandle handle, u32 mipLevel = 0) const;
    /// Record a transition of every mip level done outside ResourceManager (render graph, CUDA).
    void setTextureLayout(TextureHandle handle, u32 layout);

    /// Asynchronous uploads through the staging ring (B2.11 gate 3.4). Data is copied into the
    /// ring immediately (the source may be reused on return); the GPU copy is recorded into the
    /// open transfer batch and submitted by `flushUploads` (or when a ticket is waited on / the
    /// ring needs the space). Graphics-queue work submitted after the flush sees the data; other
    /// consumers wait on the ticket. Buffers must carry BufferUsage::TransferDst (added
    /// automatically when created with initial data); host-visible buffers are written directly.
    UploadTicket uploadBuffer(BufferHandle handle, const void* data, usize size, usize dstOffset = 0);
    /// Uploads mip 0 / layer 0 (R8G8B8A8-sized, as createTexture's initial data) and leaves the
    /// image in SHADER_READ_ONLY_OPTIMAL. The texture needs ImageUsage::TransferDst.
    UploadTicket uploadTexture(TextureHandle handle, const void* data);
    UploadTicket flushUploads();
    bool isUploadComplete(UploadTicket ticket);
    bool waitUpload(UploadTicket ticket, u64 timeoutNs = UploadQueue::kDefaultFenceTimeoutNs);
    bool waitAllUploads(u64 timeoutNs = UploadQueue::kDefaultFenceTimeoutNs);
    /// Retires completed upload batches and releases deferred destroys whose uploads retired.
    u32 retireUploads();
    bool hasPendingUploads() const { return m_uploads.hasPendingWork(); }
    /// Every ticket with serial <= this value has completed.
    u64 completedUploadSerial() const { return m_uploads.completedSerial(); }
    /// Ticket of the most recent upload (initial data included).
    UploadTicket lastUploadTicket() const { return m_lastUploadTicket; }
    const UploadQueueStats& uploadStats() const { return m_uploads.stats(); }

    usize stagingRingCapacity() const { return m_stagingRingCapacity; }
    usize stagingRingOffset() const { return m_uploads.ringHead(); }
    u32 stagingRingWrapCount() const { return m_uploads.stats().ringWraps; }
    usize stagingRingBytesInFlight() const { return m_uploads.ringBytesInUse(); }

    bool lastGpuTextureCopySubmitted() const { return m_lastGpuTextureCopySubmitted; }
    u32 lastGpuTextureCopyBytes() const { return m_lastGpuTextureCopyBytes; }
    bool lastGpuCopyUsedTransferQueue() const { return m_lastGpuCopyUsedTransferQueue; }
    bool lastGpuCopyUsedFence() const { return m_lastGpuCopyUsedFence; }
    bool lastGpuCopyWaitTimedOut() const { return m_lastGpuCopyWaitTimedOut; }
    /// True when the last image copy released and acquired across two queue families.
    bool lastOwnershipTransfer() const { return m_lastOwnershipTransfer; }
    u32 lastMipGenerateCount() const { return m_lastMipGenerateCount; }
    bool lastMipGenerateOk() const { return m_lastMipGenerateOk; }
    u32 lastTextureReadbackBytes() const { return m_lastTextureReadbackBytes; }

    LiveCounts liveCounts() const;
    /// Destroyed resources still waiting for an in-flight upload to retire.
    u32 pendingDestroyCount() const {
        return static_cast<u32>(m_deferredTextures.size() + m_deferredBuffers.size());
    }
    const DestroyStats& destroyStats() const { return m_destroyStats; }
    /// Release deferred destroys whose upload serial has completed (non-blocking poll).
    u32 collectDeferredDestroys();
    const GpuAllocStats* allocatorStats() const;

private:
    void destroyAllResources();
    bool ensureStagingRing();
    UploadTicket stageBufferUpload(Buffer& dest, const void* data, usize size, usize dstOffset);
    UploadTicket stageTextureUpload(Texture& dest, const void* data);
    /// Waits (bounded) until the upload batch `serial` has retired; submits it first if still open.
    bool waitUploadSerial(u64 serial);
    void releaseTexture(Texture& texture);
    void releaseBuffer(Buffer& buffer);
    /// True when a copy into a resource stamped with `serial` may still be pending on the GPU.
    bool uploadInFlight(u64 serial);

    template <typename T>
    struct DeferredDestroy {
        u64 serial = 0;
        T resource{};
    };

    VulkanDevice* m_device = nullptr;
    BindlessDescriptors* m_bindless = nullptr;
    std::unique_ptr<GpuAllocator> m_allocator;
    fuse::HandleMap<Texture> m_textures;
    fuse::HandleMap<Buffer> m_buffers;
    fuse::HandleMap<SamplerEntry> m_samplers;
    BufferHandle m_stagingRing{};
    usize m_stagingRingCapacity = 0;
    UploadQueue m_uploads;
    UploadTicket m_lastUploadTicket{};
    std::deque<DeferredDestroy<Texture>> m_deferredTextures;
    std::deque<DeferredDestroy<Buffer>> m_deferredBuffers;
    DestroyStats m_destroyStats{};
    bool m_lastGpuTextureCopySubmitted = false;
    u32 m_lastGpuTextureCopyBytes = 0;
    bool m_lastGpuCopyUsedTransferQueue = false;
    bool m_lastGpuCopyUsedFence = false;
    bool m_lastGpuCopyWaitTimedOut = false;
    bool m_lastOwnershipTransfer = false;
    u32 m_lastMipGenerateCount = 0;
    bool m_lastMipGenerateOk = false;
    u32 m_lastTextureReadbackBytes = 0;
    bool m_ready = false;
};

} // namespace fuse::renderer
