#pragma once

// WP-3.2 virtual shadow maps: page rendering, filtering and local lights (renderer plan Phase 3;
// execution doc WP-3.2). Builds on WP-3.1's VirtualShadowMap (page table, physical pool, render list).
//
//   shadows.beginFrame(serial, {&vsm, directionalSlot, scene, instances, locals, filter});  // after vsm.beginFrame
//   VsmShadowGraphRefs s = shadows.importInto(graph);
//   vsm.addFrame(graph, v, depth, sceneRefs);             // WP-3.1: marking ... vsm.render + vsm.clear
//   shadows.addRaster(graph, s, &v, sceneRefs);           // page rendering (below)
//   shadows.addSamplingUse(graph, s, &v, rg::kStageCompute);   // before light.shade
//   lighting.beginFrame(... LightingFrameDesc::shadows = shadows.shadowConstantsAddress() ...);
//
// Passes (all compute, all on the render graph; no manual barriers):
//   vsm.raster_reset    counters / dirty mask of the raster work buffer (transfer)
//   vsm.raster          vkCmdDispatchIndirect on WP-3.1's render-list args: exactly the render list's
//                       pages are rendered (one workgroup each, casters of the GPU scene culled per page,
//                       R32_UINT atomic-min depth into the physical pool, per-level depth key / centre);
//                       a static frame's render list is empty, so it renders nothing (cached pages)
//   vsm.local_dirty     GPU-scene bounds changes (WP-3.1's ping-pong records) -> local lights to re-render
//   vsm.local_list      local render list {light | face << 8, atlas page} + its dispatch args
//   vsm.local_clear     dispatch indirect: clears the listed atlas pages
//   vsm.local_raster    dispatch indirect: spot page / cube faces (perspective, near-plane clipped)
// Sampling: shaders/shadow_vsm/vsm_shadow.{glsl,slang} (fuse_vsm_shadow), used by light.shade and the
// forward pass through lc_ltc when LightingFrameConstants::shadowsLo / Hi is set; hard, PCF and PCSS
// filters (VsmFilterDesc). addSamplingUse declares the sampled images / page table for the reading
// stages (a pass without commands, so the consumers need no change); declareSampling adds the same
// uses to a pass the caller builds. vsm.probe samples explicit points (gates).
//
// Local lights: a spot light shadows through one 128^2 page (a square frustum around its outer cone,
// half angle capped at 80 degrees), a point light through six (cube faces), in an R32_UINT atlas
// (localPagesX x localPagesY pages). Pages are assigned in list order and kept while the list keeps
// the light; a light re-renders when it is new, its parameters or pages change, or a caster touching
// its range sphere moved (vsm.local_dirty); otherwise its pages stay cached (0 local pages rendered).
//
// Kernels: Slang primary (-fp-mode precise) with GLSL twins (`precise`), embedded by cmake/rp_wp32.cmake.
// CPU references: vsm_raster_reference.hpp. Steady-state frames make no heap allocations.

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/shadow/vsm/virtual_shadow_map.hpp>
#include <fuse/renderer/shadow/vsm_raster/vsm_raster_types.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::vsm {

struct VsmShadowsDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    u32 localPagesX = 8;           ///< local atlas pages per axis (<= kMaxLocalPagesPerAxis)
    u32 localPagesY = 4;
    u32 framesInFlight = 3;        ///< shadow-constant ring slots
    VsmKernelLanguage language = VsmKernelLanguage::Auto;
    const char* name = "vsm_shadows";
};

/// One shadowed local light.
struct VsmLocalLightDesc {
    u32 slot = kNoSlot;            ///< GpuScene light slot
    u32 type = kLocalSpot;         ///< VsmLocalType
    f32 position[3] = {0.f, 0.f, 0.f};
    f32 direction[3] = {0.f, -1.f, 0.f}; ///< spot: the direction the light points
    f32 range = 0.f;
    f32 cosOuter = 0.7f;           ///< spot: cos(outer half angle)
    f32 nearPlane = 0.05f;
    f32 lightSize = 0.f;           ///< PCSS: emitter radius
};

/// Local shadow from a scene light row (Point / Spot with a range); false for other lights.
bool makeLocalLight(u32 slot, const gpu_scene::GpuLight& light, VsmLocalLightDesc& out);

struct VsmFilterDesc {
    u32 filter = kFilterPcf;       ///< VsmShadowFilter
    u32 pcfRadius = 1;             ///< texels (<= kMaxFilterRadius)
    f32 normalOffset = 1.5f;       ///< texels
    f32 depthBias = 1.f;           ///< texels
    f32 sunTanAngle = 0.01f;       ///< PCSS, directional: tan(angular radius)
    u32 pcssMaxRadius = kMaxFilterRadius;
};

struct VsmShadowFrameDesc {
    const VirtualShadowMap* vsm = nullptr; ///< began this frame (null: no directional shadow, no dirty tracking)
    u32 directionalSlot = kNoSlot;         ///< light slot the VSM shadows
    u32 scene = 0;                         ///< GpuScene::headerHandle()
    u32 instanceCount = 0;                 ///< GpuScene::instanceHighWater()
    const VsmLocalLightDesc* locals = nullptr;
    u32 localCount = 0;                    ///< <= kMaxLocalLights (more are ignored)
    VsmFilterDesc filter{};
    bool forceLocal = false;               ///< re-render every local page this frame
};

struct VsmShadowGraphRefs {
    rg::TextureRef local; ///< local atlas (R32_UINT)
    rg::BufferRef work;   ///< raster work buffer
};

struct VsmShadowStats {
    u32 passes = 0;             ///< passes added this frame
    u32 localLights = 0;        ///< shadowed local lights this frame
    u32 localPages = 0;         ///< atlas pages assigned
    u32 forcedLights = 0;       ///< local lights the host forced this frame
    u32 droppedLights = 0;      ///< local lights without atlas space / invalid
};

class VsmShadows {
public:
    VsmShadows() = default;
    ~VsmShadows();
    VsmShadows(const VsmShadows&) = delete;
    VsmShadows& operator=(const VsmShadows&) = delete;

    /// Fails (false, nothing created) without a capable device (queryVsmCapabilities), without a built
    /// kernel of the requested language, with an invalid atlas size, or in the stub backend.
    bool init(const VsmShadowsDesc& desc);
    void destroy();
    bool valid() const { return m_initialized; }

    /// Writes this frame's VsmShadowConstants (ring slot of `frameSerial`): the VSM's frame constants
    /// address (call after VirtualShadowMap::beginFrame), the filter, the local lights and their pages.
    bool beginFrame(u64 frameSerial, const VsmShadowFrameDesc& frame);
    VsmShadowGraphRefs importInto(rg::Graph& graph);

    /// vsm.raster_reset, vsm.raster (when `vsm` is given and beginFrame had one; after its vsm.clear) and
    /// the local passes (when local lights exist).
    void addRaster(rg::Graph& graph, const VsmShadowGraphRefs& refs, const VsmGraphRefs* vsm, const gpu_scene::GpuSceneGraphRefs& scene);
    /// A command-less pass ("vsm.shadow_use", never culled) declaring StorageRead of the pool, the page
    /// table and the local atlas at `stages`: add it right before the passes that sample (light.shade:
    /// kStageCompute; the forward pass: kStageFragment).
    void addSamplingUse(rg::Graph& graph, const VsmShadowGraphRefs& refs, const VsmGraphRefs* vsm, u8 stages);
    /// The same uses on a pass the caller builds.
    static void declareSampling(rg::PassBuilder& pass, const VsmShadowGraphRefs& refs, const VsmGraphRefs* vsm, u8 stages);
    /// vsm.probe: out[i] = VsmProbeOutput of in[i] (VsmProbeInput), `count` points.
    void addProbe(rg::Graph& graph, const VsmShadowGraphRefs& refs, const VsmGraphRefs* vsm, rg::BufferRef in, u64 inAddress,
                  rg::BufferRef out, u64 outAddress, u32 count);

    // --- inspection ------------------------------------------------------------------------------
    const char* kernelLanguage() const { return m_language; }
    const VsmShadowConstants& constants() const { return m_constants; }
    u64 shadowConstantsAddress() const { return m_frameAddress; }
    const Texture& localImage() const { return m_local.image; }
    const Buffer& workBuffer() const { return m_work.buffer; }
    u32 localPagesX() const { return m_desc.localPagesX; }
    u32 localPagesY() const { return m_desc.localPagesY; }
    const VsmShadowStats& stats() const { return m_stats; }

private:
    struct Image {
        Texture image{};
        BindlessSlotHandle slot{};
        u32 handle = 0;
        u32 layout = 0;
        u8 queue = rg::kNoQueue;
    };
    struct OwnedBuffer {
        Buffer buffer{};
        BindlessSlotHandle slot{};
        u32 handle = 0;
        u8 queue = rg::kNoQueue;
    };
    enum Kernel : u32 { kRaster = 0, kLocalDirty, kLocalList, kLocalClear, kLocalRaster, kProbe, kKernelCount };
    enum RecordKind : u8 { kDispatch = 0, kDispatchIndirectVsm, kDispatchIndirectLocal, kReset };
    struct PassRecord {
        VsmShadows* self = nullptr;
        VsmRasterPush push{};
        RecordKind kind = kDispatch;
        u32 kernel = 0;
        u32 groups[3] = {1u, 1u, 1u};
        void* indirect = nullptr; ///< VkBuffer of kDispatchIndirectVsm
        u64 indirectOffset = 0;
    };
    static constexpr u32 kMaxPasses = 16u;

    bool createPipelines();
    PassRecord* nextRecord(RecordKind kind, u32 kernel);
    static void recordPass(const rg::PassContext& context, void* user);

    VsmShadowsDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    Image m_local{};
    OwnedBuffer m_work{};
    Buffer m_frameRing{};
    u64 m_frameAddress = 0;
    VsmShadowConstants m_constants{};
    const VirtualShadowMap* m_vsm = nullptr;
    // Local-light cache state: last frame's lights (by list position) and their pages.
    VsmLocalLight m_prevLocal[kMaxLocalLights] = {};
    u32 m_prevLocalCount = 0;
    bool m_hasPrev = false;
    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;
    void* m_layoutHandle = nullptr; ///< VkPipelineLayout (bindless set + 32-byte push, compute)
    void* m_pipelines[kKernelCount] = {};
    VsmShadowStats m_stats{};
};

} // namespace fuse::renderer::vsm
