#pragma once

// WP-6.2 ray-traced shadows and reflections (renderer plan Phase 6b, tier T2; execution doc WP-6.2).
//
// Two ray-query compute passes over the WP-1.5 G-buffer against the WP-6.0 TLAS:
//
//   rt.shadows       8 x 8 tiles, thread per pixel: for each shadowed light (<= kRtfxMaxShadowLights)
//                    `samples` rays from the surface (hard: directional / point / spot, 1 ray; soft:
//                    rectangle / disk area lights sampled by area, sun disks by solid angle), Owen-scrambled
//                    Sobol per (pixel, light) continued across frames; writes this frame's visibility and the
//                    mean occluder distance (the denoiser's hit distance) per light
//   rt.reflections   8 x 8 tiles, thread per pixel: `reflectionSamples` GGX visible-normal rays (roughness
//                    from RT2; a mirror below mirrorRoughness), each hit shaded at first bounce (hit triangle
//                    from the GPU scene, material emission + Lambert from the ambient and up to
//                    kRtfxMaxHitLights directional lights, optional shadow rays); writes the mean reflected
//                    radiance + mean hit distance
//
// Both read RT0 / RT2 / RT4 as bindless sampled images (registered when the G-buffer's images change) and the
// TLAS by device address, declare AccelerationStructureRead on the TLAS (so the graph orders them after
// rt.tlas.build), SampledRead on the G-buffer, StorageWrite on the outputs. No manual barriers.
//
// Consumers: the lighting (WP-2.1 light.shade) takes LightingFrameDesc::rtShadows = shadowViewAddress() and
// replaces each RT-shadowed light's VSM visibility by the pixel's RT visibility (declare the read with
// addSamplingUse(graph, refs, rg::kStageCompute) before light.shade); the denoiser (WP-6.4) reads the
// output sections (RtEffectsOutputLayout) with the WP-4.1 motion vectors.
//
//   fx.init({device, allocator, bindless});                     // false below the T2 gate (reason())
//   fx.beginFrame(serial, frameDesc);                            // constants into the ring, G-buffer binding
//   RtEffectsGraphRefs r = fx.importInto(graph);
//   fx.addShadows(graph, r, rtRefs.tlas, gbufferRefs);           // after rt.importInto / resolve
//   fx.addReflections(graph, r, rtRefs.tlas, gbufferRefs, sceneRefs);
//   fx.addSamplingUse(graph, r, rg::kStageCompute);              // before light.shade
//
// Kernels: Slang primary (-fp-mode precise) with GLSL twins, embedded by cmake/rp_wp62.cmake. Steady-state
// frames make no heap allocations (the output buffer grows on resize only; fixed pass records).

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/material_resolve/material_resolve.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/rt/rt_types.hpp>
#include <fuse/renderer/rt_effects/rt_effects_types.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::rt_effects {

enum class RtEffectsKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct RtEffectsDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    RtEffectsKernelLanguage language = RtEffectsKernelLanguage::Auto;
    u32 framesInFlight = 3; ///< frame-constant ring slots
    u32 width = 0;          ///< initial output extent (follows the G-buffer at beginFrame)
    u32 height = 0;
    const char* name = "rt_effects";
};

/// One shadowed light: its GpuScene slot and row (packShadowLight resolves the sampling record).
struct RtShadowLightDesc {
    u32 slot = 0xFFFFFFFFu;
    gpu_scene::GpuLight light{};
    u32 samples = 1; ///< rays per pixel per frame for soft kinds (clamped to 1..kRtfxMaxSamples)
};

/// A directional light that lights reflection hits.
struct RtHitLightDesc {
    u32 slot = 0xFFFFFFFFu;
    gpu_scene::GpuLight light{};
    bool shadowed = true; ///< trace a shadow ray from the hit toward it
};

struct RtEffectsFrameDesc {
    f32 viewProj[16] = {};              ///< column-major, Vulkan clip, forward z/w (the G-buffer's projection)
    f32 cameraPosition[3] = {0.f, 0.f, 0.f};
    u32 scene = 0;                      ///< GpuScene::headerHandle()
    const material_resolve::MaterialResolve* gbuffer = nullptr;
    u64 tlasAddress = 0;                ///< AccelerationStructures::tlasAddress()
    RtShadowLightDesc shadowLights[kRtfxMaxShadowLights]{};
    u32 shadowLightCount = 0;
    RtHitLightDesc hitLights[kRtfxMaxHitLights]{};
    u32 hitLightCount = 0;
    u32 reflectionSamples = 1;          ///< 0 = addReflections records nothing
    u32 frameIndex = 0;                 ///< sequence index (increment every frame to converge)
    u32 seed = 0x5EEDu;
    f32 ambient[3] = {0.f, 0.f, 0.f};   ///< ambient radiance at reflection hits
    f32 sky[3] = {0.f, 0.f, 0.f};       ///< radiance of reflection misses
    /// Optional WP-8.2 sky for reflection misses: AtmosphereGpu::frameAddress() of this frame (0 = the constant
    /// `sky`). Sets kRtfxFlagAtmosphereSky; declare the LUTs with RtEffectsGraphRefs::atmosphere.
    u64 atmosphereAddress = 0;
    f32 normalBias = 0.01f;
    f32 viewBias = 1.0e-3f;
    f32 farDistance = 1.0e4f;
    f32 mirrorRoughness = 0.03f;
    bool hitShadows = true;
    u32 shadowCullMask = rt::kRtMaskShadow;      ///< ANDed with rt::kRtMaskAll (never the dead-slot bit)
    u32 reflectionCullMask = rt::kRtMaskVisible; ///< ANDed with rt::kRtMaskAll
};

struct RtEffectsGraphRefs {
    rg::BufferRef output; ///< visibility / hit distance / reflection sections (RtEffectsOutputLayout)
    /// Set by the caller (AtmosphereGraphRefs::luts) when RtEffectsFrameDesc::atmosphereAddress != 0:
    /// rt.reflections declares it StorageRead (misses sample the sky-view LUT).
    rg::BufferRef atmosphere;
};

/// Optional debug dump of the first sample's ray + hit (RtfxRayRecord): shadows [light][pixel], reflections [pixel].
struct RtEffectsDump {
    rg::BufferRef buffer;
    u64 address = 0;
};

struct RtEffectsStats {
    u32 outputRebuilds = 0;
    u32 gbufferBinds = 0;
    u32 shadowPasses = 0;     ///< this frame
    u32 reflectionPasses = 0; ///< this frame
    u32 shadowRays = 0;       ///< this frame's upper bound (pixels x sum of samples)
    u32 reflectionRays = 0;
};

/// Resolves a GpuLight row into its shadow sampling record. False for free slots / unknown types.
/// Directional: sun disk when cosOuter < ltc's punctual threshold (setSunAngularRadius), else hard;
/// point / spot: hard; rectangle (4) / disk (5): centre, half axes from ltc::area_axes.
bool packShadowLight(u32 slot, const gpu_scene::GpuLight& light, u32 samples, RtfxShadowLight& out);
/// Directional light -> hit light (false for other types).
bool packHitLight(u32 slot, const gpu_scene::GpuLight& light, bool shadowed, RtfxHitLight& out);
/// The constants beginFrame writes for `desc` at `width` x `height` (outputs / handles left 0): the CPU
/// reference and the gates use it to reproduce the kernels' inputs.
bool buildFrameConstants(const RtEffectsFrameDesc& desc, u32 width, u32 height, RtfxFrameConstants& out);

class RtEffects {
public:
    static constexpr u32 kMaxPasses = 8;

    RtEffects() = default;
    ~RtEffects();
    RtEffects(const RtEffects&) = delete;
    RtEffects& operator=(const RtEffects&) = delete;

    /// False (nothing created, reason()) below the T2 gate, without a built kernel or in the stub backend.
    bool init(const RtEffectsDesc& desc);
    /// The caller must have retired every frame that used the buffers.
    void destroy();
    bool valid() const { return m_initialized; }
    const char* reason() const { return m_reason; }
    const char* kernelLanguage() const { return m_language; }

    /// Writes this frame's constants; follows the G-buffer's extent (output rebuilt on resize) and images.
    bool beginFrame(u64 frameSerial, const RtEffectsFrameDesc& desc);
    RtEffectsGraphRefs importInto(rg::Graph& graph);

    /// rt.shadows (nothing without shadowed lights).
    bool addShadows(rg::Graph& graph, const RtEffectsGraphRefs& refs, rg::BufferRef tlas,
                    const material_resolve::ResolveGraphRefs& gbuffer, const RtEffectsDump& dump = {});
    /// rt.reflections (nothing with reflectionSamples 0).
    bool addReflections(rg::Graph& graph, const RtEffectsGraphRefs& refs, rg::BufferRef tlas,
                        const material_resolve::ResolveGraphRefs& gbuffer, const gpu_scene::GpuSceneGraphRefs& scene,
                        const RtEffectsDump& dump = {});
    /// Command-less pass declaring the visibility section StorageRead at `stages` (before light.shade).
    void addSamplingUse(rg::Graph& graph, const RtEffectsGraphRefs& refs, u8 stages);

    /// Destroys buffers / bindless slots retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- results / inspection ------------------------------------------------------------------------
    /// LightingFrameDesc::rtShadows (the RtfxShadowView at the head of this frame's constants).
    u64 shadowViewAddress() const { return m_frameAddress; }
    const RtfxFrameConstants& frameConstants() const { return m_constants; }
    const Buffer& outputBuffer() const { return m_output; }
    const RtEffectsOutputLayout& outputLayout() const { return m_layout; }
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    const RtEffectsStats& stats() const { return m_stats; }

private:
    static constexpr u32 kGBufferInputs = 3u; ///< RT0, RT2, RT4
    static constexpr u32 kMaxRetired = 8u;
    struct Retired {
        Buffer buffer{};
        BindlessSlotHandle slots[kGBufferInputs]{};
        u64 serial = 0;
        bool used = false;
    };
    enum Kernel : u32 { kShadows = 0, kReflections, kKernelCount };
    struct PassRecord {
        RtEffects* self = nullptr;
        RtfxPush push{};
        u32 kernel = 0;
    };

    bool createPipelines(RtEffectsKernelLanguage language);
    bool createOutput(u32 width, u32 height);
    bool bindGBuffer(const material_resolve::MaterialResolve& gbuffer);
    bool retire(const Retired& r);
    static void recordDispatch(const rg::PassContext& context, void* user);

    RtEffectsDesc m_desc{};
    bool m_initialized = false;
    const char* m_reason = "not initialised";
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    u32 m_width = 0;
    u32 m_height = 0;
    RtEffectsOutputLayout m_layout{};
    Buffer m_output{};
    u8 m_outputQueue = rg::kNoQueue;
    Buffer m_frameRing{};
    u64 m_frameAddress = 0;
    RtfxFrameConstants m_constants{};
    const material_resolve::MaterialResolve* m_gbuffer = nullptr;
    void* m_gbufferImages[kGBufferInputs] = {};
    BindlessSlotHandle m_gbufferSlots[kGBufferInputs]{};
    u32 m_gbufferHandles[kGBufferInputs] = {};
    Retired m_retired[kMaxRetired]{};
    PassRecord m_records[kMaxPasses]{};
    u32 m_recordCount = 0;
    void* m_layoutHandle = nullptr; ///< VkPipelineLayout (bindless set + 16-byte push)
    void* m_pipelines[kKernelCount] = {};
    RtEffectsStats m_stats{};
};

} // namespace fuse::renderer::rt_effects
