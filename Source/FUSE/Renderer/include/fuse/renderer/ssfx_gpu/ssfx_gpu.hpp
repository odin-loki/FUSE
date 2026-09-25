#pragma once

// WP-6.3 screen-space fallback on Vulkan (renderer plan Phase 6 "SSR and SSAO/GTAO for T0, and as a
// supplement on higher tiers"; execution doc WP-6.3). Compute passes on render graph v2 that run the B5
// screen-space effects on the GPU over the WP-1.5 G-buffer and the WP-2.1 lit image, and compose them into a
// new lit image. The single-source CPU kernels (fuse::ssfx::{gtao,ssr,ssgi}_kernel) are the oracles; each
// GLSL / Slang kernel is their line-for-line twin.
//
//   ssfx.beginFrame(serial, settings, camera, ambient, {&rt0, &rt1, &rt2, &rt4, &lighting.outputImage()});
//   SsfxGraphRefs refs = ssfx.importInto(graph);
//   ssfx.addPasses(graph, refs, {rt0Ref, rt1Ref, rt2Ref, rt4Ref, litRef});   // after light.shade
//   // refs.output: the composed RGBA16F image (the post stack's input, WP-4.5 PostFrameImages::hdr)
//
// Passes (all compute, 8 x 8 tiles, all declared on the graph, no manual barriers; per-pixel data in one
// persistent work buffer reached through BDA):
//   ssfx.prepare   G-buffer + lit image -> linear depth / roughness / metallic / material AO, view normal,
//                  radiance, albedo, diffuse albedo (ssfx_gpu::prepare_pixel)
//   ssfx.gtao      GTAO visibility (fuse::ssfx::gtao_kernel, new; HBAO stays the independent oracle)
//   ssfx.ssr       mirror reflections of the lit image, roughness gate, gloss fade (fuse::ssfx::ssr_kernel)
//   ssfx.ssgi      one pass per bounce, radiance ping-pong (fuse::ssfx::ssgi_kernel / computeSsgiCpu)
//   ssfx.compose   AO re-weights the ambient term, + SSR x confidence x Fresnel, + SSGI (compose_pixel)
// Disabled effects add no pass. Kernels: Slang primary (-fp-mode precise) with GLSL twins (`precise`),
// embedded by cmake/rp_wp63.cmake. Steady-state frames make no heap allocation (the work buffer and the output
// follow the extent; the input bindings change only when the images do).

#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/ssfx_gpu/ssfx_gpu_reference.hpp>
#include <fuse/renderer/ssfx_gpu/ssfx_gpu_types.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::ssfx_gpu {

enum class SsfxKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct SsfxCapabilities {
    bool ssfx = false;
    const char* reason = "no device"; ///< "ok" when usable
};

SsfxCapabilities querySsfxCapabilities(const VulkanDevice* device);

/// Byte layout of the work buffer for an extent (every section 256-aligned).
struct SsfxBufferLayout {
    u64 prepared = 0; ///< f32x4
    u64 normals = 0;  ///< f32x4
    u64 radiance = 0; ///< f32x4
    u64 albedo = 0;   ///< f32x4
    u64 diffuse = 0;  ///< f32x4
    u64 ao = 0;       ///< f32
    u64 ssr = 0;      ///< f32x4
    u64 gi = 0;       ///< f32x4
    u64 bounce[2] = {0, 0}; ///< f32x4
    u64 workBytes = 0;

    static SsfxBufferLayout compute(u32 width, u32 height);
};

struct SsfxGpuDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    SsfxKernelLanguage language = SsfxKernelLanguage::Auto;
    u32 framesInFlight = 3; ///< frame-constant ring slots
};

/// This frame's inputs (RT4's extent sets the effect extent; every image must have it).
struct SsfxFrameImages {
    const Texture* normalAo = nullptr;  ///< RT0 RGBA16F (signed-octahedral world normal, material AO)
    const Texture* albedo = nullptr;    ///< RT1 RGBA8
    const Texture* roughMetal = nullptr;///< RT2 RGBA8 (roughness, metallic)
    const Texture* depth = nullptr;     ///< RT4 R32F device depth
    const Texture* lit = nullptr;       ///< the WP-2.1 lit image (RGBA16F)
    /// Optional f32x4-per-pixel buffer (BDA) receiving the composed radiance before RGBA16F rounding;
    /// declare it with SsfxGraphInputs::dump.
    u64 dumpAddress = 0;
    /// Optional sky-radiance source of the sky fallback (SsfxGpuSettings::skyFallback): the WP-8.2
    /// AtmosphereGpu::frameAddress() of this frame (AtParams + LUTs); declare it with SsfxGraphInputs::sky. 0 = no
    /// fallback (misses stay 0 as without the setting).
    u64 skyAddress = 0;
};

/// The graph refs of the images of SsfxFrameImages (as imported by their owners, e.g.
/// MaterialResolve::importInto's gbuffer[] and ClusteredLighting::importInto's output).
struct SsfxGraphInputs {
    rg::TextureRef normalAo;
    rg::TextureRef albedo;
    rg::TextureRef roughMetal;
    rg::TextureRef depth;
    rg::TextureRef lit;
    rg::BufferRef dump;
    rg::BufferRef sky; ///< the buffer behind SsfxFrameImages::skyAddress (AtmosphereGraphRefs::luts): ssfx.ssr / .ssgi read it
};

struct SsfxGraphRefs {
    rg::BufferRef work;
    rg::TextureRef output; ///< composed RGBA16F image
};

/// Work-buffer section an addCopy reads (gates / debugging).
enum class SsfxCopySource : u8 {
    Prepared = 0, ///< f32x4
    Normals,      ///< f32x4
    Radiance,     ///< f32x4
    Albedo,       ///< f32x4
    Diffuse,      ///< f32x4
    Ao,           ///< f32
    Ssr,          ///< f32x4
    Gi,           ///< f32x4
};

struct SsfxStats {
    u32 workRebuilds = 0;
    u32 outputRebuilds = 0;
    u32 inputBinds = 0;
    u32 retired = 0;
    u32 passes = 0; ///< passes added this frame
    bool aoRan = false;
    bool ssrRan = false;
    u32 ssgiBounces = 0;
};

class SsfxGpu {
public:
    SsfxGpu() = default;
    ~SsfxGpu();
    SsfxGpu(const SsfxGpu&) = delete;
    SsfxGpu& operator=(const SsfxGpu&) = delete;

    /// Fails (false, nothing created) without a capable device, without a built kernel of the requested
    /// language, or in the stub backend.
    bool init(const SsfxGpuDesc& desc);
    /// The caller must have retired every frame that used the buffers.
    void destroy();
    bool valid() const { return m_initialized; }

    /// Resolves and writes this frame's constants (ring slot of `frameSerial`); follows the extent (work
    /// buffer + output) and the input images (bindless slots). `ambient` is the lighting's
    /// (LightingFrameDesc::ambient). False when an input is missing, the extents differ, the camera is
    /// invalid or a (re)allocation failed.
    bool beginFrame(u64 frameSerial, const SsfxGpuSettings& settings, const SsfxCameraDesc& camera,
                    const f32 (&ambient)[3], const SsfxFrameImages& images);

    SsfxGraphRefs importInto(rg::Graph& graph);
    /// ssfx.prepare, ssfx.gtao, ssfx.ssr, ssfx.ssgi (per bounce), ssfx.compose.
    void addPasses(rg::Graph& graph, const SsfxGraphRefs& refs, const SsfxGraphInputs& inputs);
    /// Copies a work-buffer section into `dst` at `dstOffset` (after addPasses).
    void addCopy(rg::Graph& graph, const SsfxGraphRefs& refs, SsfxCopySource what, rg::BufferRef dst, u64 dstOffset);
    /// Bytes addCopy(what) copies for the current extent.
    u64 copyBytes(SsfxCopySource what) const;

    /// Destroys buffers / images / bindless slots retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- inspection --------------------------------------------------------------------------------------
    const char* kernelLanguage() const { return m_language; }
    const Texture& outputImage() const { return m_output.image; }
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    const SsfxBufferLayout& layout() const { return m_layout; }
    const SsfxStats& stats() const { return m_stats; }
    const SsfxFrameConstants& constants() const { return m_constants; }
    const Buffer& workBuffer() const { return m_work; }
    /// BDA of the GTAO visibility section (f32 per pixel, valid after the gtao pass of this frame) for
    /// consumers that apply AO themselves (declare a StorageRead of SsfxGraphRefs::work).
    u64 aoAddress() const { return m_work.deviceAddress != 0u ? m_work.deviceAddress + m_layout.ao : 0u; }

private:
    struct Output {
        Texture image{};
        BindlessSlotHandle slot{};
        u32 layout = 0;
        u8 queue = rg::kNoQueue;
    };
    static constexpr u32 kInputCount = 5u; ///< RT4, RT0, RT1, RT2, lit
    struct Retired {
        Buffer buffer{};
        Texture image{};
        BindlessSlotHandle slots[1u + kInputCount]{};
        u64 serial = 0;
    };
    struct PassRecord {
        SsfxGpu* self = nullptr;
        SsfxPush push{};
        u32 kernel = 0;
        u32 groups[3] = {1u, 1u, 1u};
        u64 copySrc = 0;
        u64 copyDst = 0;
        u64 copyBytes = 0;
        rg::BufferRef copyFrom{};
        rg::BufferRef copyTo{};
    };
    enum Kernel : u32 { kPrepare = 0, kGtao, kSsr, kSsgi, kCompose, kKernelCount };
    static constexpr u32 kMaxPasses = 32u;

    bool createPipelines();
    bool createWork(u32 width, u32 height);
    bool createOutput(u32 width, u32 height);
    bool bindInput(u32 index, const Texture* image);
    void retire(Retired r);
    void destroyRetired(Retired& r);
    PassRecord* nextRecord(u32 kernel);
    static void recordDispatch(const rg::PassContext& context, void* user);
    static void recordCopy(const rg::PassContext& context, void* user);

    SsfxGpuDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    u32 m_width = 0;
    u32 m_height = 0;
    SsfxBufferLayout m_layout{};
    Buffer m_work{};
    u8 m_workQueue = rg::kNoQueue;
    Output m_output{};
    std::vector<Retired> m_retired;
    Buffer m_frameRing{};
    u64 m_frameAddress = 0;
    SsfxFrameConstants m_constants{};
    bool m_hasDump = false;

    void* m_inputImages[kInputCount] = {};
    BindlessSlotHandle m_inputSlots[kInputCount]{};
    u32 m_inputHandles[kInputCount] = {};

    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;

    void* m_layoutHandle = nullptr; ///< VkPipelineLayout (bindless set + 32-byte push, compute)
    void* m_pipelines[kKernelCount] = {};

    SsfxStats m_stats{};
};

} // namespace fuse::renderer::ssfx_gpu
