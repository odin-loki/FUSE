#pragma once

// WP-7.1 light tree on the GPU: the flattened tables of a LightTree in host-visible ring slots (one per frame in
// flight, read through buffer device addresses), and the "light_tree.sample" compute pass (shaders/light_tree/
// lt_sample.{slang,comp}) that answers LightTreeQuery[] with LightTreeSample[] equal to LightTree::sample /
// LightTree::pmf bit for bit.
//
//   gpu.beginFrame(serial, tree);        // copies the tree into this frame's slot only when that slot holds an
//                                        // older LightTree::version() (steady state: no copy)
//   LightTreeGraphRefs refs = gpu.importInto(graph);
//   push.lightTree = gpu.headerAddress();          // consumers (WP-7.2 ReSTIR, Relight RL-4.4) include
//                                                  // lt_common.{glsl,slang} and call lt_sample / lt_pmf;
//   pass.use(refs.tree, rg::Access::StorageRead, refs.range, rg::kStageCompute);   // declare the slot
//   gpu.addSamplePass(graph, refs, queries, queriesAddress, results, resultsAddress, count);   // or the batch pass
//   gpu.collectRetired(completedSerial);
//
// Ring slots: frame serial % framesInFlight. The caller must not reuse a slot before the frame that last read it
// completed (the usual frames-in-flight contract). A tree larger than a slot reallocates the ring (the old
// buffer retires at the current serial). Steady-state frames make no heap allocation.

#include <fuse/renderer/light_tree/light_tree.hpp>
#include <fuse/renderer/light_tree/light_tree_types.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::light_tree {

enum class LightTreeKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct LightTreeCapabilities {
    bool gpu = false;
    const char* reason = "no device"; ///< "ok" when usable
};

LightTreeCapabilities queryLightTreeCapabilities(const VulkanDevice* device);

/// Byte layout of one ring slot for a tree size (every section 256-aligned).
struct LightTreeSlotLayout {
    u64 header = 0;
    u64 nodes = 0;
    u64 emitters = 0;
    u64 directional = 0;
    u64 bytes = 0;

    static LightTreeSlotLayout compute(u32 nodeCount, u32 emitterCount, u32 directionalCount);
};

struct LightTreeGpuDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    /// Bound for the sample pass (descriptor-buffer backend: pipelines carry its create flags); the kernel
    /// itself reads only buffer device addresses.
    BindlessDescriptors* bindless = nullptr;
    LightTreeKernelLanguage language = LightTreeKernelLanguage::Auto;
    u32 framesInFlight = 3;
    u32 initialLights = 1024; ///< slot sized for this many lights (grows by reallocation)
};

struct LightTreeGraphRefs {
    rg::BufferRef tree;       ///< the ring buffer
    rg::BufferRange range{};  ///< this frame's slot
    u64 header = 0;           ///< BDA of this frame's LightTreeHeader
};

struct LightTreeGpuStats {
    u32 uploads = 0;       ///< slot copies (cumulative)
    u64 uploadBytes = 0;   ///< cumulative
    u32 reallocations = 0;
    u32 retired = 0;       ///< buffers waiting for collectRetired
    u32 passes = 0;        ///< sample passes added this frame
};

class LightTreeGpu {
public:
    LightTreeGpu() = default;
    ~LightTreeGpu();
    LightTreeGpu(const LightTreeGpu&) = delete;
    LightTreeGpu& operator=(const LightTreeGpu&) = delete;

    /// Fails (false, nothing created) without a capable device, without a built kernel of the requested
    /// language, or in the stub backend.
    bool init(const LightTreeGpuDesc& desc);
    /// The caller must have retired every frame that used the buffers.
    void destroy();
    bool valid() const { return m_initialized; }

    /// Selects this frame's slot and brings it up to date with `tree`. False on allocation failure.
    bool beginFrame(u64 frameSerial, const LightTree& tree);
    LightTreeGraphRefs importInto(rg::Graph& graph);
    /// "light_tree.sample": LightTreeQuery[count] at queriesAddress -> LightTreeSample[count] at resultsAddress.
    /// `queries` / `results` are the graph refs of the buffers holding them (declared StorageRead / StorageWrite
    /// over [queriesOffset, + count x 48) and [resultsOffset, + count x 32)).
    bool addSamplePass(rg::Graph& graph, const LightTreeGraphRefs& refs, rg::BufferRef queries, u64 queriesOffset,
                       u64 queriesAddress, rg::BufferRef results, u64 resultsOffset, u64 resultsAddress, u32 count);
    u32 collectRetired(u64 completedSerial);

    const char* kernelLanguage() const { return m_language; }
    u64 headerAddress() const { return m_headerAddress; }
    const LightTreeSlotLayout& slotLayout() const { return m_layout; }
    const LightTreeGpuStats& stats() const { return m_stats; }
    const Buffer& ringBuffer() const { return m_ring; }

private:
    struct Retired {
        Buffer buffer{};
        u64 serial = 0;
    };
    struct PassRecord {
        LightTreeGpu* self = nullptr;
        LightTreePush push{};
        u32 groups = 1;
    };
    static constexpr u32 kMaxPasses = 16u;
    static constexpr u32 kMaxSlots = 8u;

    bool createPipeline();
    bool ensureCapacity(u64 slotBytes);
    static void recordDispatch(const rg::PassContext& context, void* user);

    LightTreeGpuDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    Buffer m_ring{};
    u8 m_ringQueue = rg::kNoQueue;
    u64 m_slotStride = 0;
    u32 m_slot = 0;
    u64 m_slotVersion[kMaxSlots] = {};
    LightTreeSlotLayout m_layout{};
    u64 m_headerAddress = 0;
    std::vector<Retired> m_retired;
    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;
    void* m_layoutHandle = nullptr; ///< VkPipelineLayout
    void* m_pipeline = nullptr;     ///< VkPipeline
    LightTreeGpuStats m_stats{};
};

} // namespace fuse::renderer::light_tree
