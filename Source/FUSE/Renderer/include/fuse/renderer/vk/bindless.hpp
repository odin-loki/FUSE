#pragma once

#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

static constexpr u32 kMaxTextures = 65536;
static constexpr u32 kMaxBuffers = 65536;
static constexpr u32 kMaxSamplers = 1024;
/// Fallback descriptor array length when the device reports no descriptor limits.
static constexpr u32 kBindlessGpuArrayCapacity = 1024;
/// Budgets that clamp the device-derived array lengths (descriptor pool memory). Sampled images and
/// storage buffers may use the whole CPU heap; storage images and UBOs are rarer and capped lower.
static constexpr u32 kBindlessSampledImageBudget = kMaxTextures;
static constexpr u32 kBindlessStorageImageBudget = 16384;
static constexpr u32 kBindlessStorageBufferBudget = kMaxBuffers;
static constexpr u32 kBindlessUniformBufferBudget = 16384;
/// Per-stage descriptors of each type left for non-bindless sets bound next to the bindless set
/// (per-stage limits count the whole pipeline layout).
static constexpr u32 kBindlessReservedPerStageDescriptors = 32;

/// Descriptor array length of each binding in the Vulkan bindless set.
/// Texture slots index both image arrays (storage <= sampled); buffer slots index both buffer
/// arrays (uniform <= storage). Once the Vulkan set exists the CPU heap caps follow these lengths,
/// so a descriptor write can never land outside its array.
struct BindlessArraySizes {
    u32 storageImages = kBindlessGpuArrayCapacity;
    u32 sampledImages = kBindlessGpuArrayCapacity;
    u32 samplers = kMaxSamplers;
    u32 storageBuffers = kBindlessGpuArrayCapacity;
    u32 uniformBuffers = kBindlessGpuArrayCapacity;
};

/// Sizes the bindless arrays from the device's update-after-bind descriptor limits: per type
/// min(limit - reserve, budget), then scaled down so the per-stage resource total and the all-pools
/// update-after-bind total are respected. Zero limits (unknown) fall back to kBindlessGpuArrayCapacity.
BindlessArraySizes computeBindlessArraySizes(const VulkanDescriptorLimits& limits);

/// Vulkan bindless set layout bindings (master plan B2.3).
static constexpr u32 kBindlessBindingStorageImages = 0;
static constexpr u32 kBindlessBindingSampledImages = 1;
static constexpr u32 kBindlessBindingSamplers = 2;
static constexpr u32 kBindlessBindingStorageBuffers = 3;
static constexpr u32 kBindlessBindingUniformBuffers = 4;

/// WP-0.4: one storage buffer holding `uint64_t addresses[bufferSlotCapacity]`, the device address
/// of the buffer registered at each buffer slot (0 when the slot is free or the buffer has no
/// address). Shaders reach any registered buffer through buffer_reference without a descriptor.
static constexpr u32 kBindlessBindingBufferAddressTable = 5;
static constexpr u32 kBindlessBindingCount = 6;

// --- 32-bit shader handle (WP-0.4). Shared with shaders/common/bindless.glsl; keep in sync. -------
//
//   bits  0..19  slot index            (kBindlessHandleIndexMask; every heap cap is <= 2^20)
//   bits 20..23  BindlessResourceType  (0 = invalid, so a zero-filled material row is "no resource")
//   bits 24..31  slot generation, low 8 bits (debug / CPU-side stale detection only; shaders
//                index with the low 20 bits and never compare generations)
//
// The index selects the element of the descriptor array the type names: SampledImage -> binding 1,
// StorageImage -> binding 0, Sampler -> binding 2, StorageBuffer -> binding 3 and the address table
// (binding 5), UniformBuffer -> binding 4. Texture handles index both image arrays; buffer handles
// both buffer arrays (a slot is written to exactly one of them, per its storage/uniform flag).
enum class BindlessResourceType : u8 {
    Invalid = 0,
    SampledImage = 1,
    StorageImage = 2,
    Sampler = 3,
    StorageBuffer = 4,
    UniformBuffer = 5,
};

static constexpr u32 kBindlessHandleIndexBits = 20;
static constexpr u32 kBindlessHandleIndexMask = (1u << kBindlessHandleIndexBits) - 1u;
static constexpr u32 kBindlessHandleTypeShift = 20;
static constexpr u32 kBindlessHandleTypeMask = 0xFu;
static constexpr u32 kBindlessHandleGenerationShift = 24;
static constexpr u32 kBindlessHandleGenerationMask = 0xFFu;
static constexpr u32 kBindlessInvalidShaderHandle = 0u;

[[nodiscard]] constexpr u32 packBindlessShaderHandle(BindlessResourceType type, u32 index, u32 generation) {
    return (index > kBindlessHandleIndexMask || type == BindlessResourceType::Invalid)
               ? kBindlessInvalidShaderHandle
               : ((generation & kBindlessHandleGenerationMask) << kBindlessHandleGenerationShift) |
                     ((static_cast<u32>(type) & kBindlessHandleTypeMask) << kBindlessHandleTypeShift) | index;
}
[[nodiscard]] constexpr u32 bindlessShaderHandleIndex(u32 handle) { return handle & kBindlessHandleIndexMask; }
[[nodiscard]] constexpr BindlessResourceType bindlessShaderHandleType(u32 handle) {
    return static_cast<BindlessResourceType>((handle >> kBindlessHandleTypeShift) & kBindlessHandleTypeMask);
}
[[nodiscard]] constexpr u32 bindlessShaderHandleGeneration(u32 handle) {
    return (handle >> kBindlessHandleGenerationShift) & kBindlessHandleGenerationMask;
}

/// GPU descriptor mechanism behind the bindless set.
enum class BindlessBackend : u8 {
    None = 0,         ///< CPU heap only (stub backend, no device, or creation failed)
    DescriptorSet,    ///< descriptor indexing: UPDATE_AFTER_BIND pool + one VkDescriptorSet
    DescriptorBuffer, ///< VK_EXT_descriptor_buffer: host-visible descriptor buffer, vkGetDescriptorEXT
};

enum class BindlessBackendPreference : u8 {
    Auto = 0,         ///< DescriptorBuffer when RendererCaps::descriptorBuffer, else DescriptorSet
    DescriptorSet,
    DescriptorBuffer, ///< falls back to DescriptorSet when the device has no descriptor buffer
};

[[nodiscard]] const char* bindlessBackendName(BindlessBackend backend);

struct BindlessDesc {
    BindlessBackendPreference backend = BindlessBackendPreference::Auto;
    /// Caps on the device-derived array lengths (also bound descriptor-buffer memory:
    /// count x descriptor size). Storage images <= textures, UBOs <= buffers still hold.
    u32 maxTextures = kMaxTextures;
    u32 maxBuffers = kMaxBuffers;
    u32 maxSamplers = kMaxSamplers;
    /// Create the buffer-address table (binding 5). Needs bufferDeviceAddress (T0).
    bool bufferAddressTable = true;
};

enum class BindlessHeapKind : u8 {
    Texture,
    Buffer,
    Sampler,
};

/// Generation-checked slot into a bindless heap table.
struct BindlessSlotHandle {
    BindlessHeapKind kind = BindlessHeapKind::Texture;
    u32 index = UINT32_MAX;
    u32 generation = 0;

    bool isValid() const { return index != UINT32_MAX; }

    bool operator==(const BindlessSlotHandle& other) const {
        return kind == other.kind && index == other.index && generation == other.generation;
    }

    bool operator!=(const BindlessSlotHandle& other) const { return !(*this == other); }

    static BindlessSlotHandle invalid() { return {}; }
};

/// Shader-visible descriptor array location for a heap slot.
struct BindlessBindingIndex {
    u32 binding = 0;
    u32 arrayIndex = 0;
};

BindlessBindingIndex bindlessTextureBinding(u32 slotIndex, bool storage = false);
BindlessBindingIndex bindlessBufferBinding(u32 slotIndex, bool uniform = false);
BindlessBindingIndex bindlessSamplerBinding(u32 slotIndex);

/// Per-kind heap ceiling (kMaxTextures / kMaxBuffers / kMaxSamplers).
u32 bindlessHeapMaxCapacity(BindlessHeapKind kind);

/// Clamps a requested heap size to [0, per-kind maximum].
u32 clampHeapCapacity(BindlessHeapKind kind, u32 requested);

/// True when the heap table has no reserved slots (capacity == 0).
bool bindlessHeapIsEmpty(BindlessHeapKind kind, u32 heapCapacity);

/// True when a slot index is outside the current heap table.
bool bindlessSlotIndexOutOfRange(BindlessHeapKind kind, u32 index, u32 heapCapacity);

/// True when slot table is full and the free list has no recyclable indices.
bool bindlessHeapAtCapacity(u32 slotCount, u32 freeCount, u32 maxCapacity);

/// True when handle generation matches the live slot row (occupied required).
bool bindlessSlotGenerationMatches(BindlessSlotHandle handle, u32 liveGeneration, bool occupied);

/// True when a native handle looks like a real Vulkan object (stub allocators use small integers).
bool bindlessNativeHandleReady(void* nativeHandle);

/// Preflight for bindless slot lookup / free without touching heap tables (B2.3 deepen follow-up).
struct BindlessSlotPreflight {
    bool initialized = false;
    bool handle_valid = false;
    bool heap_empty = true;
    bool index_in_range = false;
    bool generation_match = false;
    bool slot_occupied = false;

    [[nodiscard]] bool can_validate() const {
        return initialized && handle_valid && !heap_empty && index_in_range && generation_match &&
               slot_occupied;
    }

    [[nodiscard]] bool is_stale() const { return !can_validate(); }

    /// True only for in-range handles whose generation or occupancy no longer matches.
    [[nodiscard]] bool is_generation_mismatch() const {
        return initialized && handle_valid && !heap_empty && index_in_range &&
               (!generation_match || !slot_occupied);
    }
};

BindlessSlotPreflight preflightBindlessSlotHandle(BindlessSlotHandle handle, u32 heapCapacity, u32 slotGeneration,
                                                  bool occupied, bool initialized);

/// Fast early-out before heap table lookup — uninitialized, invalid handle, or empty heap.
bool shouldSkipBindlessSlotLookup(BindlessSlotHandle handle, u32 heapCapacity, bool initialized);

/// Packs binding + array index into a single u32 for material tables / push data.
u32 packBindlessBindingIndex(u32 binding, u32 arrayIndex);
bool unpackBindlessBindingIndex(u32 packed, u32& binding, u32& arrayIndex);

/// Packs kind + index + generation into a single u64 for SSBO rows / network payloads.
u64 packBindlessSlotHandle(BindlessSlotHandle handle);
BindlessSlotHandle unpackBindlessSlotHandle(u64 packed);

/// Opaque GPU-side state of the bindless heap (Vulkan handles as void*; POD so the class stays
/// header-light). Descriptor-buffer fields are zero on the DescriptorSet backend.
struct BindlessGpuHeapState {
    // VK_EXT_descriptor_buffer backend.
    void* descriptorBuffer = nullptr;       ///< VkBuffer (RESOURCE + SAMPLER descriptor buffer usage)
    void* descriptorMemory = nullptr;       ///< VkDeviceMemory (host-visible, coherent)
    u8* descriptorMapped = nullptr;
    u64 descriptorBufferAddress = 0;
    u64 descriptorBufferSize = 0;           ///< vkGetDescriptorSetLayoutSizeEXT
    u32 descriptorBufferUsage = 0;          ///< VkBufferUsageFlags for VkDescriptorBufferBindingInfoEXT
    u64 bindingOffsets[kBindlessBindingCount] = {};
    u32 descriptorSizes[kBindlessBindingCount] = {};
    void* fnGetDescriptor = nullptr;        ///< PFN_vkGetDescriptorEXT
    void* fnBindDescriptorBuffers = nullptr;///< PFN_vkCmdBindDescriptorBuffersEXT
    void* fnSetDescriptorBufferOffsets = nullptr; ///< PFN_vkCmdSetDescriptorBufferOffsetsEXT
    // Buffer-address table (both backends).
    void* addressTable = nullptr;           ///< VkBuffer
    void* addressTableMemory = nullptr;     ///< VkDeviceMemory
    u64* addressTableMapped = nullptr;
    u64 addressTableAddress = 0;
    u32 addressTableEntries = 0;
};

/// Global bindless descriptor heap (plan §5.2 "bindless registry", WP-0.4).
///
/// One descriptor set layout (set index chosen by the pipeline layout, 0 by convention):
///   0 storage images, 1 sampled images, 2 samplers, 3 storage buffers, 4 uniform buffers,
///   5 buffer-address table. Arrays are PARTIALLY_BOUND; on the DescriptorSet backend they are
///   UPDATE_AFTER_BIND per type the device supports.
///
/// Slots are generation-checked (BindlessSlotHandle). Two ways to release a slot:
///  - freeSlot / unregister*: immediate reuse. Only legal when no submitted GPU work can still
///    read the slot (legacy B2 contract; ResourceManager uses it).
///  - retireSlot(handle, serial) + collectRetired(completedSerial): the handle is invalid at once
///    (generation bump), but the descriptor and index stay reserved until the frame / timeline
///    serial that last used it has completed. Use this for per-frame churn.
///
/// Backends: init(device) keeps the DescriptorSet backend (existing consumers bind
/// descriptorSetHandle() with vkCmdBindDescriptorSets). init(device, BindlessDesc{}) picks
/// VK_EXT_descriptor_buffer when the device has it; such pipelines must OR pipelineCreateFlags()
/// into their create flags and bind through bind() (or descriptorBufferBindingInfo()).
class BindlessDescriptors {
public:
    /// Legacy entry point: DescriptorSet backend, default caps.
    void init(const VulkanDevice& device);
    /// Returns true when a GPU backend was created (false: CPU heap only, still usable).
    bool init(const VulkanDevice& device, const BindlessDesc& desc);
    void destroy(const VulkanDevice& device);

    BindlessBackend backend() const { return m_backend; }
    /// VkPipelineCreateFlags every pipeline built on layoutHandle() must carry
    /// (VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT on the descriptor-buffer backend, else 0).
    u32 pipelineCreateFlags() const;
    /// Binds the heap at `setIndex` of `pipelineLayout` (VkCommandBuffer, VkPipelineBindPoint,
    /// VkPipelineLayout). Descriptor-buffer backend: binds this heap as descriptor buffer 0 of the
    /// command buffer (replacing any other descriptor-buffer bindings) and sets the set offset.
    void bind(void* commandBuffer, u32 pipelineBindPoint, void* pipelineLayout, u32 setIndex = 0) const;
    /// Descriptor-buffer backend: for callers that bind several descriptor buffers themselves.
    u64 descriptorBufferAddress() const { return m_gpu.descriptorBufferAddress; }
    u64 descriptorBufferSize() const { return m_gpu.descriptorBufferSize; }
    u32 descriptorBufferUsage() const { return m_gpu.descriptorBufferUsage; }
    /// Device address of the buffer-address table (0 when absent), e.g. for push constants.
    u64 bufferAddressTableAddress() const { return m_gpu.addressTableAddress; }
    /// Address stored for a buffer slot (0 when free / unknown).
    u64 bufferAddressAt(u32 index) const;

    /// 32-bit shader handle for a live slot (kBindlessInvalidShaderHandle when stale).
    u32 shaderHandle(BindlessSlotHandle handle) const;
    /// True when `packed` names a live slot of the matching type and generation (low 8 bits).
    bool validateShaderHandle(u32 packed) const;
    /// Slot handle a shader handle refers to (invalid when stale).
    BindlessSlotHandle slotHandleFromShaderHandle(u32 packed) const;

    // --- deferred release (frame-fence / timeline tied) ---------------------------------------
    /// Serial stamped by retireSlot(handle): the frame being recorded (monotonic).
    void setFrameSerial(u64 serial) { m_frameSerial = serial; }
    u64 frameSerial() const { return m_frameSerial; }
    /// Invalidates `handle` now; its index (and descriptor) is reclaimed by
    /// collectRetired(completed) once completed >= retireSerial. False for stale handles.
    bool retireSlot(BindlessSlotHandle handle);
    bool retireSlot(BindlessSlotHandle handle, u64 retireSerial);
    /// Reclaims every retired slot whose serial <= completedSerial. Returns the count.
    u32 collectRetired(u64 completedSerial);
    /// collectRetired(vkGetSemaphoreCounterValue(timelineSemaphore)).
    u32 collectRetiredFromTimeline(void* timelineSemaphore);
    u32 retiredCount() const { return static_cast<u32>(m_retired.size()); }
    u32 retiredCount(BindlessHeapKind kind) const;
    /// True when `index` of `kind` is retired and not yet reclaimed.
    bool isSlotRetired(BindlessHeapKind kind, u32 index) const;

    // --- sampler heap -----------------------------------------------------------------------
    /// Returns the slot of a cached VkSampler matching `desc` (created + registered on first use,
    /// reference counted). Stub / no-device builds get a CPU-only slot.
    BindlessSlotHandle acquireSampler(const SamplerDesc& desc);
    /// Drops one reference; the last one retires the slot at frameSerial() and destroys the
    /// VkSampler when collectRetired reclaims it.
    void releaseSampler(BindlessSlotHandle handle);
    u32 samplerCacheSize() const { return static_cast<u32>(m_samplerCache.size()); }

    BindlessSlotHandle allocateTextureSlot(bool storage = false);
    BindlessSlotHandle allocateBufferSlot(bool uniform = false);
    BindlessSlotHandle allocateSamplerSlot();
    void freeTextureSlot(BindlessSlotHandle handle);
    void freeBufferSlot(BindlessSlotHandle handle);
    void freeSamplerSlot(BindlessSlotHandle handle);
    void freeSlot(BindlessSlotHandle handle);

    bool validateSlot(BindlessSlotHandle handle) const;
    /// Non-mutating preflight for slot lookup / free guards (B2.3 deepen follow-up).
    BindlessSlotPreflight preflightSlot(BindlessSlotHandle handle) const;
    /// True when index is in range but generation does not match or slot is unoccupied.
    bool slotGenerationMismatch(BindlessSlotHandle handle) const;
    /// Preferred guard for stale handles — true when handle fails validateSlot.
    bool rejectStaleSlotHandle(BindlessSlotHandle handle) const;
    /// Fast early-out before heap table lookup — uninitialized, invalid handle, or empty heap.
    bool shouldSkipSlotLookup(BindlessSlotHandle handle) const;
    /// True when freeSlot would release a live, generation-matched handle.
    bool canFreeSlot(BindlessSlotHandle handle) const;
    bool slotIndexOutOfRange(BindlessHeapKind kind, u32 index) const;
    bool isSlotOccupied(BindlessHeapKind kind, u32 index) const;
    u32 slotGeneration(BindlessHeapKind kind, u32 index) const;
    bool slotIsStorageTexture(u32 index) const;
    bool slotIsUniformBuffer(u32 index) const;

    /// Current handle for an occupied slot; invalid when unoccupied or out of range.
    BindlessSlotHandle slotHandleAt(BindlessHeapKind kind, u32 index) const;

    /// Shader binding for a validated slot handle; returns empty binding when invalid.
    BindlessBindingIndex bindingIndexForHandle(BindlessSlotHandle handle) const;
    /// Binding lookup by heap index without a generation handle; empty when unoccupied.
    BindlessBindingIndex bindingIndexForSlot(BindlessHeapKind kind, u32 index) const;

    /// Sparse table growth stub — never shrinks; rejects above per-kind caps.
    bool resizeHeap(BindlessHeapKind kind, u32 newCapacity);
    u32 heapCapacity(BindlessHeapKind kind) const;
    u32 heapMaxCapacity(BindlessHeapKind kind) const { return maxCountFor(kind); }
    u32 heapLiveCount(BindlessHeapKind kind) const;
    u32 heapFreeCount(BindlessHeapKind kind) const;
    /// True when the heap table has no reserved slots (capacity == 0).
    bool heapIsEmpty(BindlessHeapKind kind) const;
    /// True when slot table is at the per-kind ceiling with no free-list entries.
    bool heapAtCapacity(BindlessHeapKind kind) const;

    u32 registerTexture(const Texture& texture, bool storage = false);
    u32 registerBuffer(const Buffer& buffer, bool uniform = false);
    u32 registerSampler(void* samplerHandle);
    void unregisterTexture(u32 index);
    void unregisterBuffer(u32 index);
    void unregisterSampler(u32 index);

    /// Handle-returning register/unregister — preferred over legacy index API.
    BindlessSlotHandle registerTextureSlot(const Texture& texture, bool storage = false);
    BindlessSlotHandle registerBufferSlot(const Buffer& buffer, bool uniform = false);
    BindlessSlotHandle registerSamplerSlot(void* samplerHandle);
    void unregisterSlot(BindlessSlotHandle handle);

    void* layoutHandle() const { return m_layout; }
    /// Null on the descriptor-buffer backend.
    void* descriptorSetHandle() const { return m_set; }
    void* poolHandle() const { return m_pool; }
    bool vulkanDescriptorsReady() const {
        return m_layout != nullptr && ((m_pool != nullptr && m_set != nullptr) || m_gpu.descriptorMapped != nullptr);
    }
    /// Usable slots per kind: the GPU array length once Vulkan descriptors exist, else the CPU heap.
    u32 gpuTextureCapacity() const { return vulkanDescriptorsReady() ? m_arraySizes.sampledImages : kMaxTextures; }
    u32 gpuBufferCapacity() const { return vulkanDescriptorsReady() ? m_arraySizes.storageBuffers : kMaxBuffers; }
    u32 gpuSamplerCapacity() const { return vulkanDescriptorsReady() ? m_arraySizes.samplers : kMaxSamplers; }
    /// Storage-texture / uniform-buffer slots must also fit the (smaller) storage-image / UBO array.
    u32 gpuStorageTextureCapacity() const {
        return vulkanDescriptorsReady() ? m_arraySizes.storageImages : kMaxTextures;
    }
    u32 gpuUniformBufferCapacity() const {
        return vulkanDescriptorsReady() ? m_arraySizes.uniformBuffers : kMaxBuffers;
    }
    /// Array lengths the Vulkan layout was created with (fallback sizes before init / without Vulkan).
    const BindlessArraySizes& arraySizes() const { return m_arraySizes; }

    u32 descriptorUpdateCount() const { return m_descriptorUpdateCount; }
    u32 descriptorClearCount() const { return m_descriptorClearCount; }

    u32 registeredTextureCount() const;
    u32 registeredBufferCount() const;
    u32 registeredSamplerCount() const;

private:
    struct Slot {
        u32 generation = 0;
        bool occupied = false;
        bool storage = false;
        bool descriptorWritten = false;
        bool retired = false;
        /// VkSampler created by acquireSampler (destroyed when the slot is reclaimed).
        void* ownedSampler = nullptr;
    };

    struct RetiredSlot {
        BindlessHeapKind kind = BindlessHeapKind::Texture;
        u32 index = 0;
        u64 serial = 0;
    };

    struct SamplerCacheEntry {
        SamplerDesc desc{};
        BindlessSlotHandle slot{};
        u32 refs = 0;
    };

    BindlessSlotHandle allocateSlot(std::vector<Slot>& slots, std::vector<u32>& freeList, u32 maxCount,
                                    BindlessHeapKind kind, bool storageFlag, u32 flaggedLimit);
    void freeSlot(std::vector<Slot>& slots, std::vector<u32>& freeList, BindlessSlotHandle handle);
    const std::vector<Slot>& slotsFor(BindlessHeapKind kind) const;
    std::vector<Slot>& slotsFor(BindlessHeapKind kind);
    u32 maxCountFor(BindlessHeapKind kind) const;
    void updateVulkanDescriptor(BindlessSlotHandle handle, const Texture* texture, const Buffer* buffer,
                                void* samplerHandle, bool clear);
    std::vector<u32>& freeListFor(BindlessHeapKind kind);
    void releaseSlotResources(BindlessHeapKind kind, u32 index);
    bool createGpuHeap(const VulkanDevice& device, const BindlessDesc& desc);
    void destroyGpuHeap(const VulkanDevice& device);

    const VulkanDevice* m_device = nullptr;
    void* m_pool = nullptr;
    void* m_layout = nullptr;
    void* m_set = nullptr;
    BindlessArraySizes m_arraySizes{};

    std::vector<Slot> m_textureSlots;
    std::vector<Slot> m_bufferSlots;
    std::vector<Slot> m_samplerSlots;
    std::vector<u32> m_freeTextureIndices;
    std::vector<u32> m_freeBufferIndices;
    std::vector<u32> m_freeSamplerIndices;
    std::vector<RetiredSlot> m_retired;
    std::vector<SamplerCacheEntry> m_samplerCache;
    BindlessGpuHeapState m_gpu{};
    BindlessBackend m_backend = BindlessBackend::None;
    u64 m_frameSerial = 0;
    bool m_initialized = false;
    u32 m_descriptorUpdateCount = 0;
    u32 m_descriptorClearCount = 0;
};

} // namespace fuse::renderer
