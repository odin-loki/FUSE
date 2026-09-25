#pragma once

// WP-5.1 mesh-shader visibility path (renderer plan Phase 5, tier T1): a task / mesh pipeline that
// culls per meshlet (frustum, backface cone, two-phase Hi-Z) and writes the WP-1.4 visibility buffer,
// with a T0 fallback switch to the WP-1.4 indirect path (VisBuffer's vkCmdDrawIndexedIndirectCount).
//
// It sits between the WP-1.3 instance culler and the WP-1.4 targets; the per-instance decision stays
// the culler's (its args are the input), the per-meshlet one is new. Per frame:
//
//   culler.beginFrame(...); vis.beginFrame(...);
//   path.beginFrame(serial, {viewProj, camera, sceneHandle, flags}, culler);
//   MeshletGraphRefs m = path.importInto(graph);
//   path.addCulledFrame(graph, m, visRefs, sceneRefs, sceneHandle, culler, cullRefs);
//     mesh path:  cull.reset + cull.phase1, meshlet.reset, meshlet.expand1, [vis.clear], meshlet.phase1,
//                 [vis.export], cull.hiz, cull.phase2, meshlet.expand2, meshlet.phase2 (late + phase-2
//                 draws), [vis.export], cull.hiz
//     fallback:   VisBuffer::addCulledFrame (the WP-1.4 indirect path, unchanged)
//   // or the pieces: addDraw(graph, m, visRefs, scene, culler, cullRefs, CullPhase::Phase1 / Phase2)
//   // between the culler's passes, with vis.addExport + culler.addHizBuild as in VisBuffer.
//
// Selection (selectMeshletPath): the mesh path runs when MeshletPathMode is Auto or MeshShader, the
// device's effective tier is >= T1 and taskShader + meshShader are enabled (RendererCaps, WP-0.1;
// VulkanDeviceDesc::maxTier / FUSE_RENDER_TIER_MAX = T0 masks them) and the kernels are built.
// Otherwise init() selects the fallback and fallbackReason() says why (MeshShader mode then fails).
//
// Output identity: the mesh shader emits exactly the triangles of the WP-1.4 index range in the same
// vertex order with the same vertex transform (vis_common fuse_vis_clip on the same VPOS), gl_PrimitiveID
// = meshlet.triangleOffset + t == the MTRI index == the indirect path's gl_PrimitiveID, and the WP-1.4
// fragment shaders write the target, so every covered pixel holds the same (instance, triangle) and
// depth as the indirect path. Depth-tie rule: Atomic64 is order independent (nearest quantum, then the
// smaller id). Raster (D32, LESS) keeps the FIRST fragment of an exact depth tie, so identity needs
// the same rasterisation order for tied primitives: both paths draw one instance's triangles in MTRI
// order (indirect: index order in one draw; mesh: task groups by ID, surviving meshlets compacted in
// meshlet order, primitives in index order - VK_EXT_mesh_shader primitive order), EXCEPT that the mesh
// path draws deferred meshlets (early pass failed last frame's Hi-Z) in phase 2, and neither path
// orders DIFFERENT instances deterministically (the culler appends them with atomics). Exact
// cross-instance or early/late ties are therefore draw-order dependent in both paths alike; the gate
// counts tied pixels separately (none occur on its scene) and compares Atomic64 words bit for bit.
//
// Requirements: the VisBuffer's (bufferDeviceAddress, drawIndirectCount, dynamicRendering,
// geometryShader for PrimitiveId in the fragment stage) plus taskShader + meshShader, and the
// render-graph task / mesh stage bits (rg::kStageTask / kStageMesh). Meshes without meshlet data
// (bounds-only GpuMesh) are not drawn by the mesh path (counted in kMeshletCountSkipped); meshlets
// above 64 vertices / 124 triangles are not drawn (kMeshletCountOversize); task-group records beyond
// groupCapacity are dropped (kMeshletCountOverflow). Steady-state frames make no heap allocations.

#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/meshlet/meshlet_types.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/render_tier.hpp>
#include <fuse/renderer/visbuffer/visbuffer.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::meshlet {

enum class MeshletPathMode : u8 {
    Auto = 0,   ///< mesh shaders when the device is T1+, else the WP-1.4 indirect path
    MeshShader, ///< mesh shaders or init() fails
    Indirect,   ///< always the WP-1.4 indirect path (the T0 behaviour)
};

/// Kernel sources (both are embedded when their toolchain exists).
enum class MeshletKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct MeshletPathSelection {
    bool meshShaders = false;
    const char* reason = "no device"; ///< "ok" when meshShaders
};

/// The tier switch (pure; CPU-testable): mesh shaders iff mode != Indirect, caps.valid, the effective
/// tier is >= T1 and taskShader + meshShader are enabled.
MeshletPathSelection selectMeshletPath(const RendererCaps& caps, MeshletPathMode mode);

struct MeshletCapabilities {
    bool meshPath = false;            ///< selectMeshletPath(caps, Auto) and the VisBuffer raster requirements
    const char* reason = "no device"; ///< "ok" when meshPath
    u32 maxTaskGroups = 0;            ///< maxTaskWorkGroupCount[0] (records per region are clamped to it)
    u32 maxMeshOutputVertices = 0;
    u32 maxMeshOutputPrimitives = 0;
};

MeshletCapabilities queryMeshletCapabilities(const VulkanDevice* device);

struct MeshletPathDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    /// Targets, mode (Raster / Atomic64) and extent; must be initialised and outlive the path.
    visbuffer::VisBuffer* vis = nullptr;
    MeshletPathMode mode = MeshletPathMode::Auto;
    MeshletKernelLanguage language = MeshletKernelLanguage::Auto;
    u32 groupCapacity = 16384; ///< task-group records per region (32 meshlets each)
    u32 framesInFlight = 2;    ///< constants ring slots
    bool results = false;      ///< allocate the per-meshlet results buffer (MeshletFrameDesc::results)
    const char* name = "meshlet";
};

struct MeshletFrameDesc {
    f32 viewProj[16] = {}; ///< == the VisBuffer's and the culler's
    f32 camera[3] = {0.f, 0.f, 0.f};
    u32 sceneHandle = 0;   ///< GpuScene::headerHandle()
    bool frustum = true;
    bool cone = false;     ///< backface cone culling: closed geometry only (meshlet_cull_kernel.hpp)
    bool occlusion = true; ///< per-meshlet Hi-Z (only while the culler's occlusion is on)
    bool results = false;  ///< write MeshletResult words (needs MeshletPathDesc::results)
};

/// Render-graph handles of the path's buffers for one frame (importInto(); all invalid on the fallback).
struct MeshletGraphRefs {
    rg::BufferRef groups;
    rg::BufferRef masks;
    rg::BufferRef counts;
    rg::BufferRef results;
    rg::BufferRef constants;
};

struct MeshletStats {
    u32 drawPasses = 0;   ///< meshlet.phase* passes added this frame (fallback: vis.phase* passes)
    u32 groupCapacity = 0;
};

class MeshletPath {
public:
    MeshletPath() = default;
    ~MeshletPath();
    MeshletPath(const MeshletPath&) = delete;
    MeshletPath& operator=(const MeshletPath&) = delete;

    /// Selects the path (see the header comment). Mesh path: creates the pipelines and buffers; fails
    /// (nothing created) when a resource cannot be made or MeshShader mode cannot be honoured.
    /// Fallback: only records the VisBuffer. Fails in the stub backend and without a VisBuffer.
    bool init(const MeshletPathDesc& desc);
    /// The caller must have retired every frame that used the path.
    void destroy();
    bool valid() const { return m_initialized; }

    bool usesMeshShaders() const { return m_mesh; }
    /// "ok" on the mesh path, else why the fallback was selected.
    const char* fallbackReason() const { return m_reason; }
    const char* kernelLanguage() const { return m_language; }

    /// Starts a frame after culler.beginFrame and vis.beginFrame (copies the culler's constants).
    bool beginFrame(u64 frameSerial, const MeshletFrameDesc& frame, const culling::InstanceCuller& culler);
    MeshletGraphRefs importInto(rg::Graph& graph);

    /// The visibility draw of one cull phase. Mesh path: Phase1 = "meshlet.reset", "meshlet.expand1",
    /// ["vis.clear"], "meshlet.phase1" (clears the raster target); Phase2 = "meshlet.expand2",
    /// "meshlet.phase2" (late + phase-2 draws). Fallback: VisBuffer::addDraw(phase, clear on Phase1).
    void addDraw(rg::Graph& graph, const MeshletGraphRefs& refs, const visbuffer::VisGraphRefs& vis,
                 const gpu_scene::GpuSceneGraphRefs& scene, const culling::InstanceCuller& culler,
                 const culling::CullGraphRefs& cullRefs, culling::CullPhase phase);
    /// The whole two-phase frame (VisBuffer::addCulledFrame with the mesh path's draws, or it verbatim).
    void addCulledFrame(rg::Graph& graph, const MeshletGraphRefs& refs, const visbuffer::VisGraphRefs& vis,
                        const gpu_scene::GpuSceneGraphRefs& scene, u32 sceneHandle, culling::InstanceCuller& culler,
                        const culling::CullGraphRefs& cullRefs);

    // --- inspection ------------------------------------------------------------------------------
    const MeshletConstants& constants() const { return m_constants; }
    u32 groupCapacity() const { return m_capacity; }
    void* countsBuffer() const { return m_counts.buffer.handle; }
    void* groupsBuffer() const { return m_groups.buffer.handle; }
    void* resultsBuffer() const { return m_results.buffer.handle; }
    u64 groupsBytes() const { return m_groups.buffer.desc.size; }
    u64 resultsBytes() const { return m_results.buffer.desc.size; }
    const MeshletStats& stats() const { return m_stats; }

private:
    struct OwnedBuffer {
        Buffer buffer{};
        BindlessSlotHandle slot{};
        u32 handle = 0;
    };
    struct ExpandRecord {
        MeshletPath* self = nullptr;
        MeshletExpandPush push{};
        u32 groups = 0;
    };
    struct DrawRecord {
        MeshletPath* self = nullptr;
        rg::TextureRef vis;
        rg::TextureRef depth;
        bool clear = false;
        bool phase2 = false;
    };

    bool createPipelines();
    bool createBuffer(OwnedBuffer& out, u64 bytes, bool indirect, bool hostVisible, const char* name);
    void destroyBuffer(OwnedBuffer& buffer);
    static void recordReset(const rg::PassContext& context, void* user);
    static void recordExpand(const rg::PassContext& context, void* user);
    static void recordDraw(const rg::PassContext& context, void* user);

    MeshletPathDesc m_desc{};
    bool m_initialized = false;
    bool m_mesh = false;
    const char* m_reason = "not initialised";
    const char* m_language = "none";
    u32 m_capacity = 0;
    u64 m_frameSerial = 0;

    OwnedBuffer m_groups{};
    OwnedBuffer m_masks{};
    OwnedBuffer m_counts{};
    OwnedBuffer m_results{};
    OwnedBuffer m_constantsRing{};
    u32 m_ringSlot = 0;
    u64 m_constantsAddress = 0;
    MeshletConstants m_constants{};
    MeshletDrawPush m_push{};
    u8 m_queues[5] = {rg::kNoQueue, rg::kNoQueue, rg::kNoQueue, rg::kNoQueue, rg::kNoQueue};

    ExpandRecord m_expand[2] = {};
    DrawRecord m_draws[2] = {};

    void* m_expandLayout = nullptr;   ///< VkPipelineLayout (bindless + 16-byte push, compute)
    void* m_drawLayout = nullptr;     ///< VkPipelineLayout (bindless + 96-byte push, task | mesh | fragment)
    void* m_expandPipeline = nullptr;
    void* m_drawPipeline = nullptr;
    void* m_drawIndirect = nullptr;   ///< PFN_vkCmdDrawMeshTasksIndirectEXT

    MeshletStats m_stats{};
};

} // namespace fuse::renderer::meshlet
