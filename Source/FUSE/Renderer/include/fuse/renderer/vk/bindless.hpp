#pragma once

#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

static constexpr u32 kMaxTextures = 65536;
static constexpr u32 kMaxBuffers = 65536;
static constexpr u32 kMaxSamplers = 1024;

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

/// True when `requested` exceeds the per-kind maximum and would be clamped.
inline bool bindlessHeapCapacityWouldClamp(BindlessHeapKind kind, u32 requested) {
    return requested != clampHeapCapacity(kind, requested);
}

/// True when the heap table has no reserved slots (capacity == 0).
bool bindlessHeapIsEmpty(BindlessHeapKind kind, u32 heapCapacity);

/// True when a slot index is outside the current heap table.
bool bindlessSlotIndexOutOfRange(BindlessHeapKind kind, u32 index, u32 heapCapacity);

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
    /// Preflight entry point — false when bindless is not ready or handle fails validateSlot.
    bool preflightSlotHandle(BindlessSlotHandle handle) const { return validateSlot(handle); }
    /// True when index is in range but generation does not match or slot is unoccupied.
    bool slotGenerationMismatch(BindlessSlotHandle handle) const;
    /// Preferred guard for stale handles — true when handle fails validateSlot.
    bool rejectStaleSlotHandle(BindlessSlotHandle handle) const;
    /// Preflight before free/unregister — in-range occupied slot with matching generation.
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
    /// Binding lookup with preflight; returns false when handle fails rejectStaleSlotHandle.
    bool tryBindingIndexForHandle(BindlessSlotHandle handle, BindlessBindingIndex& out) const;
    /// Binding lookup by heap index without a generation handle; empty when unoccupied.
    BindlessBindingIndex bindingIndexForSlot(BindlessHeapKind kind, u32 index) const;

    /// Preflight before allocate — false when uninitialized or heap cannot grow/reuse.
    bool canAllocateSlot(BindlessHeapKind kind) const;
    /// True when `canAllocateSlot` would reject a new allocation.
    bool heapAtCapacity(BindlessHeapKind kind) const;
    /// Preflight before resize — false when uninitialized (matches resizeHeap guard).
    bool canResizeHeap(BindlessHeapKind kind, u32 requestedCapacity) const;

    /// Sparse table growth stub — never shrinks; rejects above per-kind caps.
    bool resizeHeap(BindlessHeapKind kind, u32 newCapacity);
    u32 heapCapacity(BindlessHeapKind kind) const;
    u32 heapMaxCapacity(BindlessHeapKind kind) const { return maxCountFor(kind); }
    u32 heapLiveCount(BindlessHeapKind kind) const;
    u32 heapFreeCount(BindlessHeapKind kind) const;
    /// True when the heap table has no reserved slots (capacity == 0).
    bool heapIsEmpty(BindlessHeapKind kind) const;

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

    u32 registeredTextureCount() const;
    u32 registeredBufferCount() const;
    u32 registeredSamplerCount() const;

private:
    struct Slot {
        u32 generation = 0;
        bool occupied = false;
        bool storage = false;
    };

    BindlessSlotHandle allocateSlot(std::vector<Slot>& slots, std::vector<u32>& freeList, u32 maxCount,
                                    BindlessHeapKind kind, bool storageFlag);
    void freeSlot(std::vector<Slot>& slots, std::vector<u32>& freeList, BindlessSlotHandle handle);
    const std::vector<Slot>& slotsFor(BindlessHeapKind kind) const;
    std::vector<Slot>& slotsFor(BindlessHeapKind kind);
    u32 maxCountFor(BindlessHeapKind kind) const;

    void* m_pool = nullptr;
    void* m_layout = nullptr;
    void* m_set = nullptr;

    std::vector<Slot> m_textureSlots;
    std::vector<Slot> m_bufferSlots;
    std::vector<Slot> m_samplerSlots;
    std::vector<u32> m_freeTextureIndices;
    std::vector<u32> m_freeBufferIndices;
    std::vector<u32> m_freeSamplerIndices;
    bool m_initialized = false;
};

} // namespace fuse::renderer
