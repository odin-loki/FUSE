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

/// Global descriptor table scaffolding — CPU heap with generation handles until B2.4 pool wiring.
class BindlessDescriptors {
public:
    void init(const VulkanDevice& device);
    void destroy(const VulkanDevice& device);

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
    void* descriptorSetHandle() const { return m_set; }
    void* poolHandle() const { return m_pool; }
    bool vulkanDescriptorsReady() const { return m_pool != nullptr && m_layout != nullptr && m_set != nullptr; }
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
    };

    BindlessSlotHandle allocateSlot(std::vector<Slot>& slots, std::vector<u32>& freeList, u32 maxCount,
                                    BindlessHeapKind kind, bool storageFlag, u32 flaggedLimit);
    void freeSlot(std::vector<Slot>& slots, std::vector<u32>& freeList, BindlessSlotHandle handle);
    const std::vector<Slot>& slotsFor(BindlessHeapKind kind) const;
    std::vector<Slot>& slotsFor(BindlessHeapKind kind);
    u32 maxCountFor(BindlessHeapKind kind) const;
    void updateVulkanDescriptor(BindlessSlotHandle handle, const Texture* texture, const Buffer* buffer,
                                void* samplerHandle, bool clear);

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
    bool m_initialized = false;
    u32 m_descriptorUpdateCount = 0;
    u32 m_descriptorClearCount = 0;
};

} // namespace fuse::renderer
