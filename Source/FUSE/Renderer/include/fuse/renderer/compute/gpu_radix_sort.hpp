#pragma once

// GPU radix sort (Vulkan compute) — stable LSD sort of (key, value) pairs.
//
// Keys: u32, or u64 (RadixSortKeyType::U64). Values: u32, carried with their key. 8-bit digits,
// ceil(keyBits / 8) passes; each pass = per-tile histogram, multi-level exclusive scan, stable
// scatter (shaders/compute/radix_*.comp, compiled to SPIR-V at build time by glslangValidator).
// No kernel waits on another workgroup (reduce-then-scan, not decoupled look-back), so it is
// correct on implementations without forward-progress guarantees (Lavapipe).
//
// Two ways to use it:
//  * sort()/sort64(): blocking host convenience (upload, sort, read back) on the device's
//    compute queue, with internal grow-only buffers.
//  * recordSort(): records into a caller command buffer over caller buffers bound once through
//    GpuRadixSortBinding (see GpuRadixSortBuffers for the required buffer contents / usage).
// Stub builds (no Vulkan backend) compile; create() then returns an invalid sorter.

#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

class GpuAllocator;

enum class RadixSortKeyType : u8 {
    U32 = 0,
    U64 = 1,
};

struct GpuRadixSortDesc {
    /// Directory holding radix_{histogram,scan,scan_add,scatter}.comp.spv and
    /// radix_{histogram,scatter}_k64.comp.spv. Null = the build-time output directory baked into
    /// fuse_rhi (FUSE_RADIX_SORT_SHADER_DIR) when the shaders were built.
    const char* shaderDir = nullptr;
    /// Allocator for the host-path buffers; null = the sorter creates its own.
    GpuAllocator* allocator = nullptr;
    const char* debugName = "fuse.radix_sort";
};

/// Caller buffers for recordSort (opaque VkBuffer handles). All need STORAGE_BUFFER usage and
/// room for `capacity` elements: keys/keysTemp 4 or 8 bytes per key (u64 keys little-endian),
/// values/valuesTemp 4 bytes, scratch GpuRadixSort::scratchBytes(capacity). keys/values need
/// TRANSFER_SRC|TRANSFER_DST too when a sort can have an odd pass count (final copy back).
struct GpuRadixSortBuffers {
    void* keys = nullptr;
    void* values = nullptr;
    void* keysTemp = nullptr;
    void* valuesTemp = nullptr;
    void* scratch = nullptr;
};

struct GpuRadixSortStats {
    u32 count = 0;
    u32 passes = 0;
    u32 dispatches = 0;
    u32 scanLevels = 0;
    /// GPU time of the recorded sort (timestamp queries); < 0 when timestamps are unsupported.
    f64 gpuMs = -1.0;
    /// Host wall time of the blocking call (upload + sort + readback).
    f64 totalMs = 0.0;
};

/// Descriptor sets (ping: keys -> temp, pong: temp -> keys) bound to one GpuRadixSortBuffers set.
class GpuRadixSortBinding {
public:
    ~GpuRadixSortBinding();
    GpuRadixSortBinding(const GpuRadixSortBinding&) = delete;
    GpuRadixSortBinding& operator=(const GpuRadixSortBinding&) = delete;

    bool isValid() const { return m_valid; }
    u32 capacity() const { return m_capacity; }
    RadixSortKeyType keyType() const { return m_keyType; }
    const GpuRadixSortBuffers& buffers() const { return m_buffers; }

private:
    friend class GpuRadixSort;
    GpuRadixSortBinding() = default;

    void* m_device = nullptr; // VkDevice
    void* m_pool = nullptr;   // VkDescriptorPool
    void* m_sets[2] = {nullptr, nullptr};
    GpuRadixSortBuffers m_buffers{};
    u32 m_capacity = 0;
    RadixSortKeyType m_keyType = RadixSortKeyType::U32;
    bool m_valid = false;
};

class GpuRadixSort {
public:
    static constexpr u32 kDigitBits = 8;
    static constexpr u32 kWorkgroupSize = 256;
    static constexpr u32 kTileSize = 4096;      ///< elements per histogram/scatter workgroup
    static constexpr u32 kScanChunk = 1024;     ///< entries per scan workgroup

    static std::unique_ptr<GpuRadixSort> create(VulkanDevice& device, const GpuRadixSortDesc& desc = {});
    ~GpuRadixSort();

    GpuRadixSort(const GpuRadixSort&) = delete;
    GpuRadixSort& operator=(const GpuRadixSort&) = delete;

    bool isValid() const { return m_valid; }
    const std::string& message() const { return m_message; }

    /// Largest element count one sort accepts (tile dispatch limit and maxStorageBufferRange).
    u32 maxCount(RadixSortKeyType keyType) const;
    /// Bytes of scratch (histograms + scan levels) needed to sort `count` elements.
    static u64 scratchBytes(u32 count);
    static u32 passCount(u32 keyBits) { return (keyBits + kDigitBits - 1u) / kDigitBits; }

    std::unique_ptr<GpuRadixSortBinding> bind(const GpuRadixSortBuffers& buffers, u32 capacity,
                                              RadixSortKeyType keyType) const;

    /// Records a stable ascending sort of the first `count` (key, value) pairs of the bound
    /// keys/values buffers; only the low `keyBits` bits of each key are compared (higher bits
    /// must be zero for a total order on the full key). The command buffer must belong to the
    /// device's compute queue family. The sort is a chain of render graph v2 passes
    /// (histogram / scan / scan_add / scatter per digit, copy-back on odd pass counts); every
    /// barrier is derived by the graph from the passes' declared buffer accesses, including the
    /// one ordering it after earlier work on the buffers. Afterwards the caller synchronises with
    /// srcStage COMPUTE_SHADER|TRANSFER, srcAccess SHADER_WRITE|TRANSFER_WRITE. Returns false
    /// (records nothing) on invalid arguments. Not thread-safe (one graph per sorter).
    bool recordSort(void* commandBuffer, const GpuRadixSortBinding& binding, u32 count, u32 keyBits,
                    GpuRadixSortStats* stats = nullptr) const;
    /// Render graph passes the last recordSort()/sort() built (0 before the first call).
    u32 lastGraphPassCount() const;
    /// vkCmdPipelineBarrier[2] calls the graph recorded for the last sort.
    u32 lastGraphBarrierCalls() const;

    /// Blocking host sorts. Keys/values are overwritten with the sorted result only on success.
    bool sort(u32* keys, u32* values, u32 count, u32 keyBits = 32, GpuRadixSortStats* stats = nullptr);
    bool sort64(u64* keys, u32* values, u32 count, u32 keyBits = 64, GpuRadixSortStats* stats = nullptr);

private:
    GpuRadixSort() = default;
    bool initialize(VulkanDevice& device, const GpuRadixSortDesc& desc);
    void shutdown();
    bool sortHost(void* keys, u32* values, u32 count, u32 keyBits, RadixSortKeyType keyType,
                  GpuRadixSortStats* stats);
    bool ensureHostBuffers(u32 count, RadixSortKeyType keyType);
    /// Adds the sort's render graph passes (rg::Graph*, RadixPassData*, SortRefs*: opaque here so
    /// the header stays free of the implementation types). Returns the dispatch count.
    u32 appendSortPasses(void* graph, void* passData, const GpuRadixSortBinding* binding, const void* refs, u32 count,
                         u32 passes) const;
    void releaseHostBuffers();

    struct HostState;
    struct GraphState;

    VulkanDevice* m_device = nullptr;
    bool m_valid = false;
    std::string m_message;
    void* m_setLayout = nullptr;      // VkDescriptorSetLayout
    void* m_pipelineLayout = nullptr; // VkPipelineLayout
    // [0] histogram, [1] scan, [2] scan_add, [3] scatter, [4] histogram_k64, [5] scatter_k64
    void* m_pipelines[6] = {};
    u32 m_maxWorkgroupsX = 0;
    u64 m_maxStorageBufferRange = 0;
    f64 m_timestampPeriodNs = 0.0;
    bool m_timestamps = false;
    std::unique_ptr<HostState> m_host;
    std::unique_ptr<GraphState> m_graph;
};

} // namespace fuse::renderer
