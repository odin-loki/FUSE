#pragma once

// WP-5.4 compute software rasteriser for micro-triangle clusters (renderer plan Phase 5; execution doc
// WP-5.4). Clusters (WP-1.2 meshlets of the instances the WP-1.3 culler draws) are classified by
// screen-space triangle size: small ones are rasterised in compute with 64-bit atomicMin, the rest
// by the hardware rasteriser, and BOTH write the same WP-1.4 Atomic64 visibility target (vis_format.hpp:
// depth unorm24 | instance 20 | triangle 20) in the same frame, so the WP-1.4 export / decode and
// the WP-1.5 resolve read a mixed frame like any other. Per frame:
//
//   culler.beginFrame(...); vis.beginFrame(...);               // vis: VisMode::Atomic64
//   sw.beginFrame(serial, {viewProj, sceneHandle, mode, thresholds}, culler);
//   SwRasterGraphRefs s = sw.importInto(graph);
//   sw.addCulledFrame(graph, s, visRefs, sceneRefs, sceneHandle, culler, cullRefs);
//     cull.reset + cull.phase1, swraster.reset, swraster.expand1, vis.clear, swraster.classify1,
//     swraster.sw1, swraster.hw1, vis.export, cull.hiz, cull.phase2, swraster.expand2,
//     swraster.classify2, swraster.sw2, swraster.hw2, vis.export, cull.hiz
//   // or the pieces: addDraw(..., CullPhase::Phase1 / Phase2) between the culler's passes.
//
// Classification and rasterisation rules (fill rule, snapping, depth) are documented in
// swraster_kernel.hpp, the single-source CPU reference the GPU equals bit for bit. Modes:
//   Classify       SW iff the cluster is SW-safe (every AABB corner strictly in front and inside the
//                  depth range, screen rect <= maxClusterPixels) and its estimated triangle size
//                  sqrt(rectW * rectH / triangles) <= triangleThresholdPixels;
//   ForceSoftware  every SW-safe cluster (the SW-vs-HW gate's "same cluster set");
//   ForceHardware  every cluster through the HW path.
// The HW path draws the HW list with one instanced non-indexed draw per phase (instance = cluster,
// vertex = triangle slot x 3) through the WP-1.4 vertex transform and a fragment shader writing the
// same word as the WP-1.4 atomic fragment shader; it needs neither geometryShader (no gl_PrimitiveID)
// nor mesh shaders, so the whole path runs at tier T0.
//
// SW vs HW (fuse_rp_swraster_vk_*): the same cluster set rasterised in software and in hardware agrees
// on >= 99.9% of pixels. Differences come from vertex snapping of a float x / w on the hardware side
// (the SW side snaps the exact quotient), depth interpolated in float vs in integers (one 2^-24
// quantum; only matters between surfaces that close) and the tie-break of centres exactly on an edge
// (Vulkan leaves it to the implementation; the SW side is top-left). The gate counts each kind.
//
// Requirements: the VisBuffer in Atomic64 mode (fragmentStoresAndAtomics, shaderInt64, 64-bit buffer
// or image atomics) plus bufferDeviceAddress. Limits: records / clusters per region are capped by
// clusterCapacity (<= 65535, dropped ones counted in kSwCountOverflow); meshlets above 64 v / 124 t are
// not drawn (kSwCountOversize); meshes without meshlet data are not drawn (kSwCountSkipped).
// Steady-state frames make no heap allocations.

#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/swraster/swraster_types.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/visbuffer/visbuffer.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::swraster {

enum class SwRasterMode : u8 {
    Classify = 0,  ///< by estimated triangle size (default)
    ForceSoftware, ///< every SW-safe cluster in software
    ForceHardware, ///< every cluster in hardware
};

/// Kernel sources (both are embedded when their toolchain exists).
enum class SwRasterKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct SwRasterCapabilities {
    bool usable = false;
    const char* reason = "no device"; ///< "ok" when usable
};

/// What the device supports (the VisBuffer's Atomic64 requirements).
SwRasterCapabilities querySwRasterCapabilities(const VulkanDevice* device);

/// Converts the frame thresholds into the kernel's sub-pixel units (clamped: threshold to [0, 64] px,
/// cluster extent to [0, kSwMaxClusterPixels] px). Pure; CPU-testable.
void swRasterThresholds(f32 triangleThresholdPixels, f32 maxClusterPixels, u32& triangleThreshold, u32& maxClusterExtent);

struct SwRasterDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    /// Target (must be VisMode::Atomic64), extent; must be initialised and outlive the rasteriser.
    visbuffer::VisBuffer* vis = nullptr;
    SwRasterKernelLanguage language = SwRasterKernelLanguage::Auto;
    u32 clusterCapacity = 16384; ///< classify records, SW clusters and HW clusters per region (<= 65535)
    u32 framesInFlight = 2;      ///< constants ring slots
    bool results = false;        ///< allocate the per-meshlet results buffer (SwRasterFrameDesc::results)
    const char* name = "swraster";
};

struct SwRasterFrameDesc {
    f32 viewProj[16] = {}; ///< == the VisBuffer's and the culler's
    u32 sceneHandle = 0;   ///< GpuScene::headerHandle()
    SwRasterMode mode = SwRasterMode::Classify;
    f32 triangleThresholdPixels = 4.f; ///< Classify: SW iff sqrt(rect area / triangles) <= this
    f32 maxClusterPixels = static_cast<f32>(kSwMaxClusterPixels); ///< SW only for rects <= this (px)
    bool results = false;      ///< write SwResult words (needs SwRasterDesc::results)
    bool skipSoftware = false; ///< debug: build the lists, rasterise nothing in software
    bool skipHardware = false; ///< debug: build the lists, draw nothing in hardware
};

/// Render-graph handles of the rasteriser's buffers for one frame (importInto()).
struct SwRasterGraphRefs {
    rg::BufferRef groups;
    rg::BufferRef software;
    rg::BufferRef hardware;
    rg::BufferRef counts;
    rg::BufferRef results;
    rg::BufferRef constants;
};

struct SwRasterStats {
    u32 phases = 0;   ///< addDraw calls this frame
    u32 capacity = 0; ///< per region
};

class SwRasterizer {
public:
    SwRasterizer() = default;
    ~SwRasterizer();
    SwRasterizer(const SwRasterizer&) = delete;
    SwRasterizer& operator=(const SwRasterizer&) = delete;

    /// Creates the pipelines and buffers. Fails (nothing created) without a device (stub backend),
    /// without an Atomic64 VisBuffer, when no kernel of the requested language is built or a resource
    /// cannot be made.
    bool init(const SwRasterDesc& desc);
    /// The caller must have retired every frame that used the rasteriser.
    void destroy();
    bool valid() const { return m_initialized; }
    const char* kernelLanguage() const { return m_language; }

    /// Starts a frame after culler.beginFrame and vis.beginFrame.
    bool beginFrame(u64 frameSerial, const SwRasterFrameDesc& frame, const culling::InstanceCuller& culler);
    SwRasterGraphRefs importInto(rg::Graph& graph);

    /// One cull phase: Phase1 = "swraster.reset", "swraster.expand1", "vis.clear", "swraster.classify1",
    /// "swraster.sw1", "swraster.hw1"; Phase2 = "swraster.expand2", "swraster.classify2", "swraster.sw2",
    /// "swraster.hw2".
    void addDraw(rg::Graph& graph, const SwRasterGraphRefs& refs, const visbuffer::VisGraphRefs& vis,
                 const gpu_scene::GpuSceneGraphRefs& scene, const culling::InstanceCuller& culler,
                 const culling::CullGraphRefs& cullRefs, culling::CullPhase phase);
    /// The whole two-phase frame (VisBuffer::addCulledFrame's structure with the SW + HW cluster draws).
    void addCulledFrame(rg::Graph& graph, const SwRasterGraphRefs& refs, const visbuffer::VisGraphRefs& vis,
                        const gpu_scene::GpuSceneGraphRefs& scene, u32 sceneHandle, culling::InstanceCuller& culler,
                        const culling::CullGraphRefs& cullRefs);

    // --- inspection ------------------------------------------------------------------------------
    const SwRasterConstants& constants() const { return m_constants; }
    u32 capacity() const { return m_capacity; }
    void* countsBuffer() const { return m_counts.buffer.handle; }
    u64 groupsBytes() const { return m_groups.buffer.desc.size; }
    u64 listBytes() const { return m_software.buffer.desc.size; } ///< SW or HW list (2 regions)
    u64 resultsBytes() const { return m_results.buffer.desc.size; }
    const SwRasterStats& stats() const { return m_stats; }

private:
    struct OwnedBuffer {
        Buffer buffer{};
        BindlessSlotHandle slot{};
        u32 handle = 0;
    };
    enum PassKind : u8 { kPassExpand = 0, kPassClassify, kPassSoftware, kPassHardware };
    struct PassRecord {
        SwRasterizer* self = nullptr;
        SwRasterPush push{};
        u8 kind = kPassExpand;
        u32 groups = 0; ///< expand: workgroups
    };

    bool createPipelines();
    bool createBuffer(OwnedBuffer& out, u64 bytes, bool indirect, bool hostVisible, const char* name);
    void destroyBuffer(OwnedBuffer& buffer);
    static void recordReset(const rg::PassContext& context, void* user);
    static void recordPass(const rg::PassContext& context, void* user);

    SwRasterDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u32 m_capacity = 0;
    u64 m_frameSerial = 0;

    OwnedBuffer m_groups{};
    OwnedBuffer m_software{};
    OwnedBuffer m_hardware{};
    OwnedBuffer m_counts{};
    OwnedBuffer m_results{};
    OwnedBuffer m_constantsRing{};
    u32 m_ringSlot = 0;
    u64 m_constantsAddress = 0;
    SwRasterConstants m_constants{};
    u8 m_queues[6] = {rg::kNoQueue, rg::kNoQueue, rg::kNoQueue, rg::kNoQueue, rg::kNoQueue, rg::kNoQueue};

    PassRecord m_passes[8] = {}; ///< 4 per phase

    void* m_computeLayout = nullptr;  ///< VkPipelineLayout (bindless + 16-byte push, compute)
    void* m_graphicsLayout = nullptr; ///< VkPipelineLayout (bindless + 16-byte push, vertex | fragment)
    void* m_expandPipeline = nullptr;
    void* m_classifyPipeline = nullptr;
    void* m_rasterPipeline = nullptr;
    void* m_hardwarePipeline = nullptr;

    SwRasterStats m_stats{};
};

} // namespace fuse::renderer::swraster
