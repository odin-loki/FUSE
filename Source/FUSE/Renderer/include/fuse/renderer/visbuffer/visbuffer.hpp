#pragma once

// WP-1.4 visibility buffer (renderer plan Phase 1: "a 64-bit target storing depth plus instance and
// triangle ID, written with atomics or a raster pass"; execution doc WP-1.4).
//
// Two targets, one pixel format family (vis_format.hpp):
//   Raster (T0 default)  R32G32_UINT colour attachment (instance, triangle) + D32 depth, LESS,
//                        dynamic rendering. The depth attachment is the depth the Hi-Z reads.
//   Atomic64             one 64-bit word per pixel written with atomicMin from the fragment stage,
//                        no attachments: an R64_UINT storage image (VK_EXT_shader_image_atomic_int64)
//                        or a u64 storage buffer (shaderBufferInt64Atomics, the T0 fallback). An
//                        export pass turns it into the same R32G32 image + an R32F depth (far end of
//                        the depth quantum) for the Hi-Z and for readers. The WP-5.x software
//                        rasteriser writes the same words into the same target.
//
// Geometry: the WP-1.3 culler's indirect-count draws over the WP-1.1 scene index buffer (one bound
// index buffer, GpuMesh draw ranges, gpu_scene_types.hpp "Index layout"); the vertex shader pulls
// VPOS through BDA (no vertex buffers), firstInstance is the instance slot and gl_PrimitiveID the
// mesh triangle. Everything is a render-graph v2 pass (no manual barriers). Per frame:
//
//   vis.beginFrame(serial, viewProj, scene.headerHandle());       // after culler.beginFrame
//   VisGraphRefs v = vis.importInto(graph);
//   vis.addCulledFrame(graph, v, sceneRefs, sceneHandle, culler, cullRefs);
//       // cull.reset + cull.phase1, vis.phase1 (clears), [vis.export], cull.hiz,
//       // cull.phase2, vis.phase2, [vis.export], cull.hiz (next frame's phase 1)
//   // or the pieces: addDraw(..., Phase1, clear = true), addExport(), culler.addHizBuild(depth ref,
//   // vis.depthSampledHandle()), ...
//   vis.addDecode(graph, v, sceneRefs, outBuffer, outAddress);    // optional full-frame decode
//
// Requirements: bufferDeviceAddress, drawIndirectCount, dynamicRendering and the geometryShader
// feature (SPIR-V Geometry capability for PrimitiveId in the fragment stage; no geometry stage);
// Atomic64 also needs fragmentStoresAndAtomics + shaderInt64 + shaderBufferInt64Atomics (buffer) or
// shaderImageInt64Atomics + R64_UINT storage-image atomics (image). queryVisCapabilities() reports
// them; init() fails cleanly when the requested mode is unsupported.
//
// Steady-state frames make no heap allocations (fixed pass records; targets change only in resize()).

#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/visbuffer/vis_format.hpp>
#include <fuse/renderer/visbuffer/vis_types.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::visbuffer {

enum class VisMode : u8 {
    Raster = 0,   ///< R32G32_UINT + D32 (T0 default)
    Atomic64 = 1, ///< 64-bit atomicMin target + export
};

enum class VisAtomicTarget : u8 {
    Auto = 0, ///< image when supported, else buffer
    Image,    ///< R64_UINT storage image (VK_EXT_shader_image_atomic_int64)
    Buffer,   ///< u64 storage buffer (shaderBufferInt64Atomics)
};

/// Kernel sources (both are embedded when their toolchain exists).
enum class VisKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct VisCapabilities {
    bool raster = false;       ///< Raster mode usable
    bool atomicBuffer = false; ///< Atomic64 with the buffer target usable
    bool atomicImage = false;  ///< Atomic64 with the image target usable
    const char* rasterReason = "no device";  ///< why raster is not usable ("ok" when it is)
    const char* atomicReason = "no device";  ///< why neither atomic target is usable ("ok" when one is)
};

/// What the device (as created) supports for the visibility buffer.
VisCapabilities queryVisCapabilities(const VulkanDevice* device);

struct VisBufferDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    u32 width = 0; ///< initial extent (resize() changes it)
    u32 height = 0;
    VisMode mode = VisMode::Raster;
    VisAtomicTarget atomicTarget = VisAtomicTarget::Auto;
    VisKernelLanguage language = VisKernelLanguage::Auto;
    const char* name = "visbuffer";
};

/// Render-graph handles of the targets for one frame (importInto()).
struct VisGraphRefs {
    rg::TextureRef vis;      ///< R32G32_UINT (both modes; Atomic64: written by the export)
    rg::TextureRef depth;    ///< D32 attachment (Raster) or R32F exported depth (Atomic64)
    rg::TextureRef image64;  ///< Atomic64 image target
    rg::BufferRef buffer64;  ///< Atomic64 buffer target
};

struct VisStats {
    u32 targetRebuilds = 0; ///< resize() (re)creations
    u32 retired = 0;        ///< target sets waiting for collectRetired
    u32 drawPasses = 0;     ///< draw passes added this frame
    bool instanceLimitExceeded = false; ///< Atomic64: the scene has instance slots >= kVis64MaxInstances
};

class VisBuffer {
public:
    VisBuffer() = default;
    ~VisBuffer();
    VisBuffer(const VisBuffer&) = delete;
    VisBuffer& operator=(const VisBuffer&) = delete;

    /// Fails (returns false, nothing created) when the device lacks what the mode needs, when no
    /// kernel of the requested language is built, or in the stub backend.
    bool init(const VisBufferDesc& desc);
    /// The caller must have retired every frame that used the targets.
    void destroy();
    bool valid() const { return m_initialized; }

    /// (Re)creates the targets; the old ones retire at the current frame serial.
    bool resize(u32 width, u32 height);
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }

    /// Starts a frame: the view the draws and the decode use, and the scene root.
    /// `instanceHighWater` (GpuScene::instanceHighWater()) is checked against the atomic id limit.
    bool beginFrame(u64 frameSerial, const f32 viewProj[16], u32 sceneHandle, u32 instanceHighWater = 0);
    VisGraphRefs importInto(rg::Graph& graph);

    /// Draw pass "vis.phase1" / "vis.phase2" for one cull phase. `clear`: the first draw of the frame
    /// (Raster: loadOp CLEAR; Atomic64: a "vis.clear" pass first).
    void addDraw(rg::Graph& graph, const VisGraphRefs& refs, const gpu_scene::GpuSceneGraphRefs& scene,
                 const culling::InstanceCuller& culler, const culling::CullGraphRefs& cullRefs, culling::CullPhase phase,
                 bool clear);
    /// Atomic64: "vis.export" (R32G32 + R32F depth from the words). Raster: no-op.
    void addExport(rg::Graph& graph, const VisGraphRefs& refs);
    /// Atomic64: "vis.clear" (every word <- kVis64Clear), for draw passes recorded by another
    /// component into these targets (WP-5.1 mesh-shader path). Raster: no-op (use loadOp CLEAR).
    void addClear(rg::Graph& graph, const VisGraphRefs& refs);
    /// The whole two-phase frame (see the header comment); the culler must have begun its frame.
    void addCulledFrame(rg::Graph& graph, const VisGraphRefs& refs, const gpu_scene::GpuSceneGraphRefs& scene,
                        u32 sceneHandle, culling::InstanceCuller& culler, const culling::CullGraphRefs& cullRefs);
    /// "vis.decode": VisDecodeTexel per pixel into `out` (>= width * height * 16 bytes, BDA `outAddress`).
    void addDecode(rg::Graph& graph, const VisGraphRefs& refs, const gpu_scene::GpuSceneGraphRefs& scene,
                   rg::BufferRef out, u64 outAddress);

    /// Destroys targets retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- inspection ------------------------------------------------------------------------------
    VisMode mode() const { return m_desc.mode; }
    VisAtomicTarget atomicTarget() const { return m_atomicTarget; } ///< resolved (Image or Buffer)
    const char* kernelLanguage() const { return m_language; }
    /// Bindless sampled handle of the depth the Hi-Z reads (D32 view or R32F export).
    u32 depthSampledHandle() const;
    /// Bindless storage-image handle of the R32G32_UINT visibility image (WP-1.5 material resolve).
    u32 visStorageHandle() const;
    /// Bindless handle of the Atomic64 target (storage image or storage buffer; 0 in Raster mode):
    /// VisRasterPush::target64 for draw passes recorded by another component (WP-5.1).
    u32 target64Handle() const;
    const Texture& visImage() const { return m_targets.vis; }
    const Texture& depthImage() const { return m_targets.depth; }
    const Texture& image64() const { return m_targets.image64; }
    const Buffer& buffer64() const { return m_targets.buffer64; }
    const VisStats& stats() const { return m_stats; }

private:
    struct Targets {
        Texture vis{};
        Texture depth{};
        Texture image64{};
        Buffer buffer64{};
        BindlessSlotHandle visStorage{};
        BindlessSlotHandle depthSampled{};
        BindlessSlotHandle depthStorage{};
        BindlessSlotHandle target64{};
        u32 visLayout = 0;
        u32 depthLayout = 0;
        u32 image64Layout = 0;
        u8 visQueue = rg::kNoQueue;
        u8 depthQueue = rg::kNoQueue;
        u8 image64Queue = rg::kNoQueue;
        u8 buffer64Queue = rg::kNoQueue;
    };
    struct Retired {
        Targets targets{};
        u64 serial = 0;
    };
    struct DrawRecord {
        VisBuffer* self = nullptr;
        const culling::InstanceCuller* culler = nullptr;
        culling::CullPhase phase = culling::CullPhase::Phase1;
        rg::TextureRef vis;
        rg::TextureRef depth;
        rg::BufferRef indices; ///< scene index buffer (no draws are recorded without one)
        bool clear = false;
    };
    struct Vis64Record {
        VisBuffer* self = nullptr;
        Vis64Push push{};
    };
    struct DecodeRecord {
        VisBuffer* self = nullptr;
        VisDecodePush push{};
    };
    static constexpr u32 kMaxDraws = 4u;
    static constexpr u32 kMaxVis64Passes = 6u;
    static constexpr u32 kMaxDecodes = 2u;

    bool createPipelines();
    bool createTargets(Targets& t, u32 width, u32 height);
    void destroyTargets(Targets& t);
    static void recordDraw(const rg::PassContext& context, void* user);
    static void recordVis64(const rg::PassContext& context, void* user);
    static void recordDecode(const rg::PassContext& context, void* user);
    void addVis64Pass(rg::Graph& graph, const VisGraphRefs& refs, u32 mode);

    VisBufferDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    VisAtomicTarget m_atomicTarget = VisAtomicTarget::Buffer;
    u64 m_frameSerial = 0;
    u32 m_width = 0;
    u32 m_height = 0;
    Targets m_targets{};
    std::vector<Retired> m_retired;

    VisRasterPush m_rasterPush{};
    DrawRecord m_draws[kMaxDraws] = {};
    u32 m_drawCount = 0;
    Vis64Record m_vis64[kMaxVis64Passes] = {};
    u32 m_vis64Count = 0;
    DecodeRecord m_decodes[kMaxDecodes] = {};
    u32 m_decodeCount = 0;

    void* m_graphicsLayout = nullptr; ///< VkPipelineLayout (bindless set + 96-byte push, vertex|fragment)
    void* m_computeLayout = nullptr;  ///< VkPipelineLayout (bindless set + 96-byte push, compute)
    void* m_rasterPipeline = nullptr; ///< Raster mode draw
    void* m_atomicPipeline = nullptr; ///< Atomic64 draw
    void* m_vis64Pipeline = nullptr;  ///< Atomic64 clear / export
    void* m_decodePipeline = nullptr;

    VisStats m_stats{};
};

} // namespace fuse::renderer::visbuffer
