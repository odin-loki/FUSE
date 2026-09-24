#pragma once

// WP-1.3 GPU instance culling (renderer plan Phase 1: "compute instance culling (frustum plus
// two-phase Hi-Z occlusion)", "vkCmdDrawIndexedIndirectCount submission").
//
// Everything is a render-graph v2 pass (no manual barriers); the scene is read only through the
// WP-1.1 header handle + BDA. Per frame:
//
//   culler.beginFrame(serial, {viewProj, scene.instanceHighWater(), occlusion, cameraCut});
//   GpuSceneGraphRefs scene = gpuScene.importInto(graph);
//   CullGraphRefs cull = culler.importInto(graph);
//   culler.addPhase1(graph, cull, scene, gpuScene.headerHandle());   // "cull.reset" + "cull.phase1"
//   graph.addPass("draw.phase1", ...)  + InstanceCuller::useDraws(pass, cull, CullPhase::Phase1)
//                                      // callback: culler.recordDraws(cmd, CullPhase::Phase1)
//   culler.addHizBuild(graph, cull, depth, depthHandle);            // "cull.hiz" (phase-1 depth)
//   culler.addPhase2(graph, cull, scene, gpuScene.headerHandle());   // "cull.phase2" (dispatch indirect)
//   graph.addPass("draw.phase2", ...)  + useDraws(..., CullPhase::Phase2)
//   culler.addHizBuild(graph, cull, depth, depthHandle);            // full depth: next frame's phase 1
//   ... executor.execute(graph); later culler.collectRetired(completedSerial);
//
// Buffers (persistent, GpuAllocator, device local except the constants ring, bindless storage-buffer
// slots; args and counts also carry BufferUsage::Indirect):
//   args        2 x capacity VkDrawIndexedIndirectCommand: [0, capacity) phase 1, [capacity, 2 capacity) phase 2
//   counts      16 u32 (cull_types.hpp CullCount): draw counts, candidate count, Hi-Z counter,
//               phase-2 VkDispatchIndirectCommand
//   candidates  capacity u32 instance slots that failed phase 1
//   results     capacity u32 CullResult per instance slot (debug / parity / other consumers)
//   constants   framesInFlight x CullConstants, host visible, read through BDA
//   Hi-Z        R32_SFLOAT, square power of two, full mip chain (hiz_build_kernel.hpp), storage view
//               per mip for the build and one sampled view (all mips) for the tests; lives across
//               frames in SHADER_READ_ONLY_OPTIMAL.
//
// CPU cost per frame is constant: beginFrame writes one CullConstants, the pass callbacks record a
// fixed number of commands (dispatch sizes come from the instance count or from the GPU). Steady-
// state frames make no heap allocations (buffers only grow in beginFrame, doubling).
//
// Stub backend / no device: init() fails; the CPU reference kernels (instance_cull_kernel.hpp,
// cull_reference.hpp) remain available.

#include <fuse/renderer/culling/cull_types.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::culling {

enum class CullPhase : u8 {
    Phase1 = 0,
    Phase2 = 1,
};

/// Kernel sources (both are embedded when their toolchain exists).
enum class CullKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct InstanceCullerDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr; ///< Hi-Z image
    BindlessDescriptors* bindless = nullptr;
    u32 instanceCapacity = 1024; ///< initial; grows by doubling in beginFrame
    u32 framesInFlight = 2;      ///< constants ring slots
    CullKernelLanguage language = CullKernelLanguage::Auto;
    const char* name = "culling";
};

struct CullFrameDesc {
    f32 viewProj[16] = {}; ///< column-major, Vulkan clip space, forward depth (cull_types.hpp)
    u32 instanceCount = 0; ///< GpuScene::instanceHighWater()
    bool frustum = true;
    bool occlusion = true;
    /// Discard last frame's Hi-Z and view (teleport, cut): phase 1 draws nothing, every frustum-
    /// visible instance is tested against this frame's Hi-Z in phase 2.
    bool cameraCut = false;
};

/// Render-graph handles of the culler's resources for one frame (importInto()).
struct CullGraphRefs {
    rg::BufferRef args;
    rg::BufferRef counts;
    rg::BufferRef candidates;
    rg::BufferRef results;
    rg::BufferRef constants;
    rg::TextureRef hiz;
};

struct CullerStats {
    u32 capacity = 0;
    u32 reallocations = 0;
    u32 hizRebuilds = 0; ///< Hi-Z (re)creations (resolution changes)
    u32 retired = 0;     ///< resources waiting for collectRetired
};

class InstanceCuller {
public:
    InstanceCuller() = default;
    ~InstanceCuller();
    InstanceCuller(const InstanceCuller&) = delete;
    InstanceCuller& operator=(const InstanceCuller&) = delete;

    /// Needs a Vulkan device with bufferDeviceAddress + drawIndirectCount, an allocator and bindless.
    bool init(const InstanceCullerDesc& desc);
    /// The caller must have retired every frame that used the culler.
    void destroy();
    bool valid() const { return m_initialized; }

    /// (Re)creates the Hi-Z pyramid for a depth extent (<= kMaxDepthExtent); resets the history.
    bool setResolution(u32 depthWidth, u32 depthHeight);
    u32 depthWidth() const { return m_depthWidth; }
    u32 depthHeight() const { return m_depthHeight; }

    /// Starts a frame: grows the buffers if needed, fills this frame's CullConstants (planes from
    /// viewProj, last frame's viewProj, history flag). Call setResolution() first.
    bool beginFrame(u64 frameSerial, const CullFrameDesc& frame);
    CullGraphRefs importInto(rg::Graph& graph);

    /// "cull.reset" (counts <- {0,0,0,0,0,1,1,0}, vkCmdUpdateBuffer) and "cull.phase1".
    void addPhase1(rg::Graph& graph, const CullGraphRefs& refs, const gpu_scene::GpuSceneGraphRefs& scene,
                   u32 sceneHandle);
    /// "cull.hiz": single-pass Hi-Z from `depth` (declared SampledRead; depthHandle = bindless
    /// sampled handle of a DEPTH-aspect view of it, extent == setResolution()).
    void addHizBuild(rg::Graph& graph, const CullGraphRefs& refs, rg::TextureRef depth, u32 depthHandle);
    /// "cull.phase2": vkCmdDispatchIndirect over the phase-1 candidates.
    void addPhase2(rg::Graph& graph, const CullGraphRefs& refs, const gpu_scene::GpuSceneGraphRefs& scene,
                   u32 sceneHandle);

    /// Declares the IndirectRead of a phase's args and count on the caller's draw pass.
    void useDraws(rg::PassBuilder& pass, const CullGraphRefs& refs, CullPhase phase) const;
    /// vkCmdDrawIndexedIndirectCount for a phase. Bind the pipeline and the index buffer first: the
    /// args carry each mesh's draw range in GpuScene::indexBuffer() (WP-1.4 index layout), or
    /// {3 x triangleCount, 0, 0} for meshes without one (caller-bound index / vertex buffers).
    void recordDraws(void* commandBuffer, CullPhase phase) const;

    /// Destroys buffers / images retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- inspection ------------------------------------------------------------------------------
    const CullConstants& constants() const { return m_constants; }
    u32 capacity() const { return m_capacity; }
    bool historyValid() const { return (m_constants.flags & kCullHistoryValid) != 0u; }
    const char* kernelLanguage() const { return m_language; }
    const CullerStats& stats() const { return m_stats; }
    void* argsBuffer() const { return m_args.buffer.handle; }
    void* countsBuffer() const { return m_counts.buffer.handle; }
    void* resultsBuffer() const { return m_results.buffer.handle; }
    void* hizImage() const { return m_hiz.image; }
    u32 hizDim() const { return m_hizDim; }
    u32 hizMipCount() const { return m_hizMipCount; }
    u64 argsBytes() const { return m_args.buffer.desc.size; }
    u64 resultsBytes() const { return m_results.buffer.desc.size; }

private:
    struct OwnedBuffer {
        Buffer buffer{};       ///< GpuAllocator buffer (BufferUsage::Indirect for args / counts)
        BindlessSlotHandle slot{};
        u32 handle = 0;        ///< bindless shader handle
    };
    struct Retired {
        OwnedBuffer buffers[4] = {};
        u32 bufferCount = 0;
        Texture image{};
        void* views[kMaxHizMips + 1u] = {};
        BindlessSlotHandle slots[kMaxHizMips + 1u] = {};
        u64 serial = 0;
    };
    static constexpr u32 kMaxHizBuilds = 4u;

    bool createPipelines();
    bool createBuffer(OwnedBuffer& out, u64 bytes, bool indirect, bool hostVisible, const char* name);
    void destroyBuffer(OwnedBuffer& buffer);
    bool ensureCapacity(u32 instances);
    void destroyHiz(bool retire);
    void retireBuffer(OwnedBuffer& buffer);
    static void recordReset(const rg::PassContext& context, void* user);
    static void recordPhase1(const rg::PassContext& context, void* user);
    static void recordPhase2(const rg::PassContext& context, void* user);
    static void recordHiz(const rg::PassContext& context, void* user);

    InstanceCullerDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    u32 m_capacity = 0;

    OwnedBuffer m_args{};
    OwnedBuffer m_counts{};
    OwnedBuffer m_candidates{};
    OwnedBuffer m_results{};
    OwnedBuffer m_constantsRing{};
    u32 m_ringSlot = 0;
    u64 m_constantsAddress = 0; ///< this frame's slot

    // Hi-Z pyramid.
    Texture m_hiz{};                      ///< image + all-mip sampled view
    void* m_hizMipViews[kMaxHizMips] = {}; ///< VkImageView per mip (storage)
    BindlessSlotHandle m_hizMipSlots[kMaxHizMips] = {};
    BindlessSlotHandle m_hizSampledSlot{};
    BindlessSlotHandle m_sampler{};
    u32 m_hizDim = 0;
    u32 m_hizMipCount = 0;
    u32 m_depthWidth = 0;
    u32 m_depthHeight = 0;
    u32 m_hizLayout = 0; ///< VkImageLayout tracker across frames (graph import)
    u8 m_hizQueue = rg::kNoQueue;

    // Frame state.
    CullConstants m_constants{};
    f32 m_lastViewProj[16] = {};
    bool m_lastFrameBuilt = false; ///< a Hi-Z build was added in the previous frame
    bool m_builtThisFrame = false;
    /// Recorded by the pass callbacks at execute time (the graph keeps `user` pointers).
    struct CullRecord {
        InstanceCuller* self = nullptr;
        CullPush push{};
        u32 groups = 0; ///< phase 1: workgroups (phase 2 dispatches indirectly)
    };
    struct HizRecord {
        InstanceCuller* self = nullptr;
        HizPush push{};
    };
    CullRecord m_cullRecords[2] = {}; ///< phase 1, phase 2
    HizRecord m_hizRecords[kMaxHizBuilds] = {};
    u32 m_hizBuilds = 0;

    // Pipelines.
    void* m_layout = nullptr;      ///< VkPipelineLayout (bindless set + 32-byte push)
    void* m_cullPipeline = nullptr;
    void* m_hizPipeline = nullptr;

    std::vector<Retired> m_retired;
    CullerStats m_stats{};
};

} // namespace fuse::renderer::culling
