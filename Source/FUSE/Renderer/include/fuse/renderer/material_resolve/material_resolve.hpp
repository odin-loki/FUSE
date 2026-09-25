#pragma once

// WP-1.5 material resolve (renderer plan Phase 1: "material resolve pass reconstructing attributes from
// the visibility buffer and shading per material tile (material classification plus indirect
// dispatch)"; execution doc WP-1.5).
//
// Input: the WP-1.4 visibility image (R32G32_UINT instance / triangle, either VisBuffer mode) and the
// WP-1.1 GPU scene. Output: the existing deferred G-buffer layout (deferred/gbuffer.hpp,
// GBufferAttachment RT0..RT5 with GBufferLayout's formats, packed by write_gbuffer of
// shaders/common/gbuffer.glsl, so the lighting passes read it unchanged) + an R32_UINT material id.
//
// Per pixel, a full-screen fragment pass (no depth, no hardware derivatives) reconstructs the
// triangle through the GPU scene (instance -> mesh -> scene index buffer -> VPOS / VNRM / VTAN / VUV0),
// the perspective-correct barycentrics and their analytic screen-space derivatives
// (resolve_kernel.hpp), interpolates UV0 (+ d/dx, d/dy), the world normal and tangent and the
// velocity, evaluates the Material::GPUMaterial row with textureGrad (bindless textures, one sampler)
// and packs the G-buffer. Two paths, identical output:
//
//   Binned   "resolve.reset" + "resolve.classify" (8 x 8 tiles into 4 nested feature bins: empty,
//            flat, textured, normal-mapped, + the layered bin) + "resolve.gbuffer": one indirect draw of
//            tile quads per bin (vkCmdDrawIndirectCount, count 1: see ResolveBinLayout), each with a
//            pipeline specialised to that bin's features.
//   Uber     "resolve.gbuffer": one full-screen triangle, every feature, per-pixel branches (fallback).
//
//   resolve.beginFrame(serial, {viewProj, prevViewProj, scene.headerHandle(), vb.visStorageHandle(), sampler});
//   ResolveGraphRefs r = resolve.importInto(graph);
//   resolve.addResolve(graph, r, visRefs.vis, sceneRefs, ResolvePath::Binned);   // after the VB passes
//
// Layered materials (asset W0.7, material_layers/): rows marked with gpu_scene::kGpuMaterialLayered
// (set_gpu_material_layered) are classified into a fifth bin, kBinLayered, whose pipeline evaluates the
// layered material (ml_common's ml_evaluate: triplanar / stochastic tiling / detail / height-blended and
// wet layers) at the reconstructed world position with the analytic derivatives, sampling the
// MaterialLayers resolve table's mip-mapped bindless images with textureGrad. Set
// ResolveFrameDesc::layered = MaterialLayers::resolveTableHandle() and pass the table's graph ref:
//   resolve.addResolve(graph, r, visRefs.vis, sceneRefs, ResolvePath::Binned, layers.importInto(graph).resolveTable);
// Non-layered pixels are shaded by exactly the code they were before (bit-identical in every bin).
//
// Also: addAttributeDump ("resolve.attributes", the reconstructed attributes per pixel for parity
// gates) and, with MaterialResolveDesc::forward, addForward ("resolve.forward"): a forward G-buffer
// raster of the same scene through the culler's draws, with the same material code but rasteriser
// interpolation and hardware derivatives: the oracle for the resolve (and a path for devices without
// the visibility buffer). Every access is declared on the render graph (no manual barriers).
//
// Requirements: the visibility buffer's raster capabilities (queryVisCapabilities), 7 colour
// attachments (8 for the forward reference). Steady-state frames make no heap allocations.

#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/material_resolve/resolve_types.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::material_resolve {

/// Kernel sources (both are embedded when their toolchain exists). The forward reference is GLSL only.
enum class ResolveKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

enum class ResolvePath : u8 {
    Binned = 0, ///< tile classification + one indirect draw per bin
    Uber = 1,   ///< single full-screen pass
};

struct ResolveCapabilities {
    bool resolve = false;
    bool forward = false;
    const char* reason = "no device"; ///< why the resolve is not usable ("ok" when it is)
};

ResolveCapabilities queryResolveCapabilities(const VulkanDevice* device);

struct MaterialResolveDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    u32 width = 0; ///< initial extent (resize() changes it)
    u32 height = 0;
    ResolveKernelLanguage language = ResolveKernelLanguage::Auto;
    /// Also create the forward reference pipeline and its depth + ids targets.
    bool forward = false;
    /// Frame-constant ring slots (frames the GPU may still read while the CPU writes the next).
    u32 framesInFlight = 3;
    const char* name = "material_resolve";
};

struct ResolveFrameDesc {
    f32 viewProj[16] = {};     ///< this frame's view-projection (the visibility buffer's)
    f32 prevViewProj[16] = {}; ///< last frame's (velocity); equal to viewProj on the first frame
    u32 scene = 0;             ///< GpuScene::headerHandle()
    u32 vis = 0;               ///< VisBuffer::visStorageHandle() (unused by the forward reference)
    u32 sampler = 0;           ///< bindless sampler handle for the material textures (REPEAT for layered rows)
    u32 layered = 0;           ///< MaterialLayers::resolveTableHandle() (layered rows); 0 = none (default surface)
};

/// Render-graph handles of the targets for one frame (importInto()).
struct ResolveGraphRefs {
    rg::TextureRef gbuffer[kResolveColorAttachments - 1u]; ///< GBufferAttachment order (RT0..RT5)
    rg::TextureRef materialId; ///< R32_UINT
    rg::BufferRef bins;        ///< ResolveBinLayout
    rg::TextureRef forwardIds;   ///< forward reference: R32G32_UINT (instance, triangle)
    rg::TextureRef forwardDepth; ///< forward reference: D32
};

struct ResolveStats {
    u32 targetRebuilds = 0;
    u32 retired = 0;
    u32 resolvePasses = 0; ///< resolve.gbuffer passes added this frame
    u32 forwardPasses = 0;
};

class MaterialResolve {
public:
    MaterialResolve() = default;
    ~MaterialResolve();
    MaterialResolve(const MaterialResolve&) = delete;
    MaterialResolve& operator=(const MaterialResolve&) = delete;

    /// Fails (returns false, nothing created) when the device lacks what the resolve needs, when no
    /// kernel of the requested language is built, or in the stub backend.
    bool init(const MaterialResolveDesc& desc);
    /// The caller must have retired every frame that used the targets.
    void destroy();
    bool valid() const { return m_initialized; }

    /// (Re)creates the targets and the bin buffer; the old ones retire at the current frame serial.
    bool resize(u32 width, u32 height);
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    u32 tilesX() const { return (m_width + kTileSize - 1u) / kTileSize; }
    u32 tilesY() const { return (m_height + kTileSize - 1u) / kTileSize; }

    /// Writes this frame's constants into the ring slot of `frameSerial`.
    bool beginFrame(u64 frameSerial, const ResolveFrameDesc& frame);
    ResolveGraphRefs importInto(rg::Graph& graph);

    /// The resolve of `vis` (the visibility image the frame's `vis` handle names) into the G-buffer.
    /// `layeredTable`: the layered-material table buffer (MaterialLayerRefs::resolveTable) the frame's
    /// `layered` handle names, declared as read by the resolve; invalid when the frame has none.
    void addResolve(rg::Graph& graph, const ResolveGraphRefs& refs, rg::TextureRef vis,
                    const gpu_scene::GpuSceneGraphRefs& scene, ResolvePath path, rg::BufferRef layeredTable = {});
    /// "resolve.attributes": ResolveAttributeTexel per pixel into `out` (>= width * height * 112 bytes).
    void addAttributeDump(rg::Graph& graph, const ResolveGraphRefs& refs, rg::TextureRef vis,
                          const gpu_scene::GpuSceneGraphRefs& scene, rg::BufferRef out, u64 outAddress);
    /// "resolve.forward": the forward reference over the culler's draws of `phase` (needs `forward`).
    /// `clear`: the first forward draw of the frame (loadOp CLEAR).
    void addForward(rg::Graph& graph, const ResolveGraphRefs& refs, const gpu_scene::GpuSceneGraphRefs& scene,
                    const culling::InstanceCuller& culler, const culling::CullGraphRefs& cullRefs, culling::CullPhase phase,
                    bool clear);

    /// Destroys targets retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- inspection ------------------------------------------------------------------------------
    const char* kernelLanguage() const { return m_language; }
    const Texture& gbufferImage(u32 attachment) const { return m_targets.gbuffer[attachment]; }
    const Texture& materialIdImage() const { return m_targets.materialId; }
    const Texture& forwardIdsImage() const { return m_targets.forwardIds; }
    const Buffer& binBuffer() const { return m_targets.bins; }
    const ResolveStats& stats() const { return m_stats; }

private:
    struct Targets {
        Texture gbuffer[kResolveColorAttachments - 1u]{};
        Texture materialId{};
        Texture forwardIds{};
        Texture forwardDepth{};
        Buffer bins{};
        BindlessSlotHandle binsSlot{};
        u32 layouts[kForwardColorAttachments + 1u] = {};
        u8 queues[kForwardColorAttachments + 2u] = {};
    };
    struct Retired {
        Targets targets{};
        u64 serial = 0;
    };
    struct PassRecord {
        MaterialResolve* self = nullptr;
        ResolvePush push{};
        ResolvePath path = ResolvePath::Binned;
        ResolveGraphRefs refs{};
        const culling::InstanceCuller* culler = nullptr;
        culling::CullPhase phase = culling::CullPhase::Phase1;
        rg::BufferRef indices;
        bool clear = false;
    };
    static constexpr u32 kMaxPasses = 8u;

    bool createPipelines();
    bool createTargets(Targets& t, u32 width, u32 height);
    void destroyTargets(Targets& t);
    PassRecord* nextRecord();
    static void recordReset(const rg::PassContext& context, void* user);
    static void recordClassify(const rg::PassContext& context, void* user);
    static void recordResolve(const rg::PassContext& context, void* user);
    static void recordAttributes(const rg::PassContext& context, void* user);
    static void recordForward(const rg::PassContext& context, void* user);

    MaterialResolveDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    u32 m_width = 0;
    u32 m_height = 0;
    Targets m_targets{};
    std::vector<Retired> m_retired;
    Buffer m_frameRing{};
    u64 m_frameAddress = 0; ///< this frame's ResolveFrameConstants

    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;

    void* m_graphicsLayout = nullptr; ///< VkPipelineLayout (bindless set + 16-byte push, vertex|fragment)
    void* m_computeLayout = nullptr;  ///< VkPipelineLayout (bindless set + 16-byte push, compute)
    void* m_resolvePipelines[kBinLayered + 1u] = {}; ///< per bin id: feature bins, uber (kBinUber), layered
    void* m_classifyPipeline = nullptr;
    void* m_attributesPipeline = nullptr;
    void* m_forwardPipeline = nullptr;

    ResolveStats m_stats{};
};

} // namespace fuse::renderer::material_resolve
