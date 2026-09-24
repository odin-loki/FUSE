#pragma once

// WP-2.3 forward transparency (renderer plan Phase 2: "forward pass for transparents reading the same
// cluster lists"; execution doc WP-2.3).
//
// Transparent instances are GpuScene instances flagged kInstanceTransparent (with kInstanceValid |
// kInstanceVisible); the WP-1.3 culler skips them, so the visibility buffer / resolve / deferred shade
// only see opaque geometry. Per frame:
//
//   CPU (beginFrame)   collect the transparent instances from the scene's CPU mirror, frustum-test their
//                      bounding spheres and sort them back to front (forward_reference.hpp: why a CPU
//                      sort); write the frame constants + the sorted draw table into a host-visible ring.
//   forward.copy       the clustered lighting's lit image (RGBA16F) -> the forward colour target.
//   forward.draw       dynamic rendering: colour target LOAD + blend, the visibility buffer's D32 depth
//                      read-only (depth test LESS against the opaque depth, no depth writes, early
//                      fragment tests), one vkCmdDrawIndexed per sorted instance (scene index buffer,
//                      firstInstance = draw index, per-draw front face for mirrored transforms), back-face
//                      culling by default.
//
// Fragment shading (shaders/forward/fw_forward.{frag,_fs.slang}): the material evaluation of the
// WP-1.5 resolve (fuse_mr_shade: same material rows, textures with hardware derivatives), packed and
// quantised like the deferred G-buffer (write_gbuffer + the RGBA16F / RGBA8 attachment formats), then
// decoded and lit with the WP-2.1 deferred shade's own functions (shaders/lighting/lc_common: position
// from the fragment's device depth, cluster lookup, fuse_lc_light over the directional list and the
// fragment's cluster list, read from the ClusteredLighting buffers of the same frame). So a transparent
// surface at opacity 1 shades exactly like its opaque twin in the deferred path, up to the rasteriser's
// attribute interpolation. Output: premultiplied (radiance * opacity, opacity), blend ONE /
// ONE_MINUS_SRC_ALPHA on colour and alpha (forward_reference.hpp blend_over).
//
//   fwd.setOpacity(handle, 0.4f);
//   fwd.beginFrame(serial, {viewProj, &scene, &lighting, sampler});   // after lighting.beginFrame
//   ForwardGraphRefs f = fwd.importInto(graph);
//   fwd.addForward(graph, f, sceneRefs, lightingRefs, visRefs.depth);  // after lighting.addShade
//
// Requirements: the visibility buffer in Raster mode (its D32 depth is the depth attachment), the
// clustered lighting (its frame constants + lists), 1 RGBA16F colour attachment with blending.
// Steady-state frames make no heap allocations (fixed ring, draw arrays sized at init; the colour
// target follows the lit image's extent).

#include <fuse/renderer/forward/forward_reference.hpp>
#include <fuse/renderer/forward/forward_types.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/lighting/gpu/clustered_lighting.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::forward {

enum class ForwardKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

enum class ForwardCullMode : u8 {
    None = 0, ///< both faces (open / double-sided transparent meshes)
    Back,     ///< front faces only: one layer per closed convex mesh (default)
    Front,
};

struct ForwardCapabilities {
    bool forward = false;
    const char* reason = "no device"; ///< "ok" when usable
};

ForwardCapabilities queryForwardCapabilities(const VulkanDevice* device);

struct ForwardTransparencyDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    u32 width = 0; ///< initial colour-target extent (follows the lit image at beginFrame)
    u32 height = 0;
    /// Transparent draws per frame; beyond it the nearest `maxDraws` instances are drawn.
    u32 maxDraws = 1024;
    /// Opacity table pre-size (instance slots); setOpacity beyond it grows the table.
    u32 instanceCapacity = 1024;
    ForwardCullMode cull = ForwardCullMode::Back;
    /// Winding of front faces in framebuffer space for a transform with a positive determinant
    /// (mirrored instances get the opposite).
    bool frontFaceCounterClockwise = true;
    bool frustumCull = true;
    ForwardKernelLanguage language = ForwardKernelLanguage::Auto;
    u32 framesInFlight = 3; ///< frame-constant ring slots
    const char* name = "forward_transparency";
};

struct ForwardFrameDesc {
    f32 viewProj[16] = {}; ///< the visibility buffer's view-projection (column-major, forward depth)
    /// CPU mirror of the scene (the transparent set, transforms, mesh draw ranges) + header handle.
    const gpu_scene::GpuScene* scene = nullptr;
    /// This frame's clustered lighting, after its beginFrame (cluster lists, camera, lit image extent).
    const lighting_gpu::ClusteredLighting* lighting = nullptr;
    u32 sampler = 0; ///< bindless sampler handle for the material textures
    /// Optional parity dump: BDA of ForwardDumpTexel[maxDraws * width * height] (layer = draw index).
    u64 dumpAddress = 0;
};

struct ForwardGraphRefs {
    rg::TextureRef color; ///< RGBA16F: lit image + blended transparents
};

struct ForwardStats {
    u32 draws = 0;      ///< this frame
    u32 candidates = 0; ///< transparent instances seen this frame
    u32 culled = 0;     ///< outside the frustum this frame
    u32 dropped = 0;    ///< beyond maxDraws this frame
    u32 targetRebuilds = 0;
    u32 retired = 0;
    u32 passes = 0; ///< forward.draw passes added this frame
};

class ForwardTransparency {
public:
    ForwardTransparency() = default;
    ~ForwardTransparency();
    ForwardTransparency(const ForwardTransparency&) = delete;
    ForwardTransparency& operator=(const ForwardTransparency&) = delete;

    /// Fails (false, nothing created) without a capable device, without a built kernel of the
    /// requested language, or in the stub backend.
    bool init(const ForwardTransparencyDesc& desc);
    /// The caller must have retired every frame that used the target.
    void destroy();
    bool valid() const { return m_initialized; }

    /// Opacity of a transparent instance (clamped to [0, 1] when drawn); applies while the handle's
    /// generation owns the slot. Instances without an opacity draw at 1. Allocates only when the slot
    /// is beyond the table (see ForwardTransparencyDesc::instanceCapacity).
    bool setOpacity(gpu_scene::InstanceHandle handle, f32 opacity);
    void clearOpacity(gpu_scene::InstanceHandle handle);

    /// Collects + sorts the transparent draws, writes this frame's constants (ring slot of
    /// `frameSerial`), follows the lit image's extent.
    bool beginFrame(u64 frameSerial, const ForwardFrameDesc& frame);
    ForwardGraphRefs importInto(rg::Graph& graph);

    /// forward.copy + forward.draw. `lightingRefs` = the ClusteredLighting's importInto() of this frame
    /// (after its addShade), `depth` = the visibility buffer's D32 depth (VisGraphRefs::depth, Raster
    /// mode). `dump`, when valid, must be the buffer at beginFrame's dumpAddress.
    void addForward(rg::Graph& graph, const ForwardGraphRefs& refs, const gpu_scene::GpuSceneGraphRefs& scene,
                    const lighting_gpu::LightingGraphRefs& lightingRefs, rg::TextureRef depth, rg::BufferRef dump = {});

    /// Destroys targets retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- inspection ------------------------------------------------------------------------------
    const char* kernelLanguage() const { return m_language; }
    const Texture& colorImage() const { return m_color.image; }
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    u32 drawCount() const { return m_drawCount; }
    /// This frame's draws in draw order (back to front).
    const SortedDraw* draws() const { return m_sorted.data(); }
    u32 maxDraws() const { return m_desc.maxDraws; }
    const ForwardStats& stats() const { return m_stats; }

private:
    struct Target {
        Texture image{};
        u32 layout = 0;
        u8 queue = rg::kNoQueue;
    };
    struct Retired {
        Texture image{};
        u64 serial = 0;
    };
    struct CopyRecord {
        rg::TextureRef src;
        rg::TextureRef dst;
        u32 width = 0;
        u32 height = 0;
    };
    struct DrawRecord {
        ForwardTransparency* self = nullptr;
        ForwardPush push{};
        rg::TextureRef color;
        rg::TextureRef depth;
        rg::BufferRef indices;
    };
    struct CpuDraw {
        u32 indexCount = 0;
        u32 firstIndex = 0;
        s32 vertexOffset = 0;
        bool mirrored = false;
    };
    static constexpr u32 kMaxPasses = 4u;

    bool createPipeline();
    bool createTarget(Target& t, u32 width, u32 height);
    static void recordCopy(const rg::PassContext& context, void* user);
    static void recordDraw(const rg::PassContext& context, void* user);

    ForwardTransparencyDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    u32 m_width = 0;
    u32 m_height = 0;
    Target m_color{};
    std::vector<Retired> m_retired;
    Buffer m_frameRing{};
    u64 m_slotStride = 0;
    u64 m_frameAddress = 0;
    u64 m_dumpAddress = 0;
    std::vector<OpacityEntry> m_opacity;
    std::vector<SortedDraw> m_sorted; ///< maxDraws entries
    std::vector<CpuDraw> m_cpuDraws;  ///< maxDraws entries
    u32 m_drawCount = 0;

    CopyRecord m_copies[kMaxPasses] = {};
    DrawRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;

    void* m_layoutHandle = nullptr; ///< VkPipelineLayout (bindless set + 16-byte push, vertex | fragment)
    void* m_pipeline = nullptr;

    ForwardStats m_stats{};
};

} // namespace fuse::renderer::forward
