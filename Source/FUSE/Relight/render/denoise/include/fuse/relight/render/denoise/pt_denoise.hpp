// FUSE Relight RL-5.5: the path tracer's denoiser (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.5, RL-5.5 row) - the renderer's
// in-tree radiance denoiser (renderer/denoise/radiance_denoiser.hpp: separate demodulated diffuse / specular channels,
// hit-distance pre-blur, A-SVGF gradients, specular virtual motion, history fix, firefly suppression, variance-guided
// a-trous) fed by RL-5.1's outputs, plus the A-SVGF gradient producer on the path tracer
// (shaders/rl_dn_gradient_core.h: re-shading the previous frame's paths at one pixel per 3 x 3 stratum).
//
//   inputs   PathTracerGpu's output sections by device address: kPtOutDiffuse / kPtOutSpecular (demodulated rgb + the
//            continuation's hit distance), kPtOutNormal (normal + roughness), kPtOutDepth, kPtOutMotion (previous -
//            current UV: motionScale +1), kPtOutInstance; the cameras from the compiled scene's parameter words
//   passes   "relight.denoise.gradient" (after "relight.pt.trace", same scene resources) then the rdn.* passes; the
//            output (f32x4 (rgb, variance) per pixel, diffuse and specular) is host-visible and declared HostRead - the
//            RL-5.7 post composite remodulates it (PtPostInput::denoisedDiffuse / denoisedSpecular)
//   seeds    denoising needs a fresh random-number stream per frame: PtPostHook::frameSeed gives the path tracer's
//            frame seed
//
// Options (Remix name first; the relight.* ones are FUSE's):
//   rtx.useDenoiser                 Remix switch (default on); the denoiser runs when relight.post.enable,
//                                   rtx.useDenoiser and relight.denoise.enable are all set
//   relight.denoise.enable          FUSE master switch (default off until the RL-5.7 Wine gates expect denoised
//                                   composites; FUSE_RELIGHT_DENOISE)
//   relight.denoise.backend         "" / "rdn": in-tree; "nrd": the WP-6.4b plugin through the DenoiserRegistry (falls
//                                   back to the in-tree denoiser with the reason recorded - see selectPtDenoiser);
//                                   a selected "dlss_rr" upscaler replaces the denoiser (Ray Reconstruction)
//   relight.denoise.gradients       A-SVGF (the producer pass + the gradient-driven history)
//   relight.denoise.virtualMotion   specular hit-distance reprojection
//   relight.denoise.historyFix / preblur / fireflyRatio / fireflySigma / atrousIterations
#pragma once

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/render/pathtrace/pt_gpu.hpp>
#include <fuse/relight/render/pathtrace/pt_reference.hpp>
#include <fuse/relight/render/pathtrace/pt_scene.hpp>
#include <fuse/renderer/denoise/denoiser.hpp>
#include <fuse/renderer/denoise/radiance_denoise.hpp>
#include <fuse/renderer/denoise/radiance_denoiser.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::render::denoise {

namespace rdn = renderer::denoise;

struct RemixDenoiseOptions {
    FUSE_RELIGHT_OPTION("rtx", bool, useDenoiser, true,
                        "Denoise the path tracer's output (Remix switch; FUSE also needs relight.denoise.enable).");
};

struct DenoiseOptions {
    FUSE_RELIGHT_OPTION_ENV("relight.denoise", bool, enable, false, "FUSE_RELIGHT_DENOISE",
                            "Run the RL-5.5 denoiser on the path tracer's output inside the RL-5.7 post pipeline.");
    FUSE_RELIGHT_OPTION_ENV("relight.denoise", std::string, backend, "", "FUSE_RELIGHT_DENOISE_BACKEND",
                            "Denoiser backend: \"\" / rdn (in-tree), nrd (plugin, falls back when unavailable).");
    FUSE_RELIGHT_OPTION_ENV("relight.denoise", bool, gradients, true, "FUSE_RELIGHT_DENOISE_GRADIENTS",
                            "A-SVGF temporal gradients (producer pass on the path tracer + gradient-driven history).");
    FUSE_RELIGHT_OPTION_ENV("relight.denoise", bool, virtualMotion, true, "FUSE_RELIGHT_DENOISE_VIRTUAL_MOTION",
                            "Specular reprojection by the virtual (hit-distance) motion of reflections.");
    FUSE_RELIGHT_OPTION("relight.denoise", bool, historyFix, true,
                        "Fill short histories (disocclusions) from the history-fix pyramid.");
    FUSE_RELIGHT_OPTION("relight.denoise", bool, preblur, true, "Hit-distance-scaled pre-blur of the inputs.");
    FUSE_RELIGHT_OPTION("relight.denoise", float, fireflyRatio, 0.f,
                        "Spatial firefly bound: x the brightest 3 x 3 neighbour (0: off).");
    FUSE_RELIGHT_OPTION("relight.denoise", float, fireflySigma, 0.f,
                        "Temporal anti-firefly: clamp at m1 + k sigma of the history (0: off).");
    FUSE_RELIGHT_OPTION("relight.denoise", int, atrousIterations, 5, "A-trous iterations (1..5).");
};

/// Registers the options above (call once; keeps them linked in).
void registerDenoiseOptions();

struct PtDenoiseConfig {
    bool enabled = false; ///< rtx.useDenoiser && relight.denoise.enable
    std::string backend = ""; ///< relight.denoise.backend
    rdn::RdnSettings settings{};

    static PtDenoiseConfig fromOptions();
};

/// The in-tree settings for the path tracer's outputs: RL-5.5 preset, motion previous - current, instance test on.
rdn::RdnSettings ptDenoiseSettings();

struct PtDenoiseSelection {
    std::string backend = "none"; ///< "rdn" (in-tree), "dlss_rr" (the upscaler denoises), "none", or a plugin id
    std::string reason; ///< why backend differs from the request ("" otherwise)
    bool fallback = false;
    bool active = false; ///< the in-tree denoiser runs this frame
};

/// The plugin switch (§5.5 "Plugins"): `upscaler` is the post pipeline's
/// running upscaler id. dlss_rr -> the upscaler replaces the denoiser; backend
/// "nrd" -> the registry's selection for REBLUR (diffuse / specular + hit
/// distance) without a native frame binding: the Relight path tracer does not
/// bind NRD-packed textures, so a plugin that needs one is not selectable and
/// the in-tree denoiser runs with the reason recorded; unknown ids fall back likewise.
PtDenoiseSelection selectPtDenoiser(const PtDenoiseConfig& config, std::string_view upscaler,
                                    const rdn::DenoiserRegistry& registry);

/// The current (previous = false) or previous camera of a compiled scene's parameter words (packParams).
rdn::RdnCamera cameraFromParams(const pathtrace::Word* words, bool previous);

// --- GPU -------------------------------------------------------------------------------------------------------------

/// Push constants of "relight.denoise.gradient" (rl_dn_gradient.{comp,slang}), 128 bytes.
struct RldnPush {
    u64 params = 0;
    u64 instances = 0;
    u64 triangles = 0;
    u64 materials = 0;
    u64 portals = 0;
    u64 lightMap = 0;
    u64 lightTable = 0;
    u64 lightTree = 0;
    u64 tlas = 0;
    u64 lut = 0;
    u64 restirDi = 0;
    u64 records = 0;
    u64 gradient = 0;
    u64 reserved = 0;
    u32 strataW = 0;
    u32 strataH = 0;
    u32 lightCount = 0;
    u32 havePrev = 0;
};
static_assert(sizeof(RldnPush) == 128u, "RldnPush (rl_dn_gradient.comp / .slang)");

enum class PtDenoiseLanguage : u8 { Auto = 0, Slang, Glsl };

struct PtDenoiserDesc {
    renderer::VulkanDevice* device = nullptr;
    renderer::GpuAllocator* allocator = nullptr;
    renderer::BindlessDescriptors* bindless = nullptr;
    PtDenoiseLanguage language = PtDenoiseLanguage::Auto;
    u32 framesInFlight = 3;
    bool hostVisible = false; ///< every denoiser arena mapped (tests); the output always is
};

struct PtDenoiseStats {
    u32 frames = 0;
    u32 gradientPasses = 0;
    bool history = false;
    bool gradients = false;
};

class PtDenoiser {
public:
    PtDenoiser();
    ~PtDenoiser();
    PtDenoiser(const PtDenoiser&) = delete;
    PtDenoiser& operator=(const PtDenoiser&) = delete;

    /// False (reason()) without the requirements (the path tracer's + the radiance denoiser's) or in the stub backend.
    bool init(const PtDenoiserDesc& desc);
    void destroy();
    bool valid() const { return m_initialized; }
    const char* reason() const { return m_reason; }
    const char* kernelLanguage() const;

    void setSettings(const rdn::RdnSettings& settings);
    const rdn::RdnSettings& settings() const { return m_settings; }
    /// The next frame starts without history (and without gradient records).
    void reset();

    /// This frame (after PathTracerGpu::beginFrame with `frame`): parameter words, cameras, denoiser constants.
    bool beginFrame(u64 frameSerial, const pathtrace::PathTracerGpu& gpu, const pathtrace::PtCompiledScene& scene,
                    const pathtrace::PtFrameDesc& frame);
    /// After PathTracerGpu::addTracePass: the gradient pass, the denoiser passes and the output's HostRead.
    bool addPasses(renderer::rg::Graph& graph, const pathtrace::PathTracerGpu& gpu, const pathtrace::PtGraphRefs& refs);
    u32 collectRetired(u64 completedSerial);

    /// f32x4 (rgb, variance) per pixel of the last completed frame (host-visible).
    const f32* denoisedDiffuse() const { return m_dn.mappedOutputD(); }
    const f32* denoisedSpecular() const { return m_dn.mappedOutputS(); }
    const rdn::RadianceDenoiser& denoiser() const { return m_dn; }
    /// The gradient producer's buffers (records then samples, f32x4 per stratum each; host-visible).
    const f32* gradientSamples() const;
    const f32* gradientRecords() const;
    u32 strataW() const { return m_strataW; }
    u32 strataH() const { return m_strataH; }
    const PtDenoiseStats& stats() const { return m_stats; }

private:
    bool createPipeline();
    bool ensureGradientBuffer(u32 strataW, u32 strataH);
    static void recordGradient(const renderer::rg::PassContext& context, void* user);

    PtDenoiserDesc m_desc{};
    bool m_initialized = false;
    const char* m_reason = "not initialised";
    const char* m_language = "none";
    rdn::RdnSettings m_settings{};
    rdn::RadianceDenoiser m_dn;
    renderer::Buffer m_ring{}; ///< per slot: this frame's then the previous frame's parameter words
    renderer::Buffer m_gradient{}; ///< records, then samples (host-visible)
    u8 m_gradientQueue = 0xFFu;
    u8 m_ringQueue = 0xFFu;
    u64 m_slotStride = 0;
    u64 m_samplesOffset = 0;
    u32 m_strataW = 0;
    u32 m_strataH = 0;
    pathtrace::Word m_prevWords[pathtrace::kPtParamWords]{};
    u32 m_prevWidth = 0;
    u32 m_prevHeight = 0;
    bool m_havePrev = false;
    struct GradientRecord {
        PtDenoiser* self = nullptr;
        RldnPush push{};
        u32 groupsX = 1;
        u32 groupsY = 1;
    };
    GradientRecord m_record{};
    struct Retired {
        renderer::Buffer buffer{};
        u64 serial = 0;
    };
    std::vector<Retired> m_retired;
    u64 m_serial = 0;
    bool m_frameReady = false;
    void* m_layoutHandle = nullptr; ///< VkPipelineLayout
    void* m_pipeline = nullptr; ///< VkPipeline
    PtDenoiseStats m_stats{};
};

// --- CPU oracle ------------------------------------------------------------------------------------------------------

/// The CPU twin of one PtDenoiser frame (tests): the gradient producer (C++
/// dialect of rl_dn_gradient_core.h over the CPU reference path tracer) and
/// RdnReference on the inputs a PtReferenceImage frame provides (the same packing as PathTracerGpu's sections).
class PtDenoiseCpu {
public:
    void init(u32 width, u32 height, const rdn::RdnSettings& settings);
    void reset();
    /// Renders one path-traced frame of `scene` on the CPU (frameSeed /
    /// sampleBase / spp of `settings`), runs the gradient producer and the
    /// denoiser. The noisy inputs of the frame stay available (noisy*()).
    bool runFrame(const pathtrace::PtCompiledScene& scene, const pathtrace::PtSettings& settings, u32 frameSeed,
                  u32 sampleBase, kernel::Backend backend = kernel::Backend::CpuParallel);
    /// Only the gradient producer (the GPU parity gate replays it on the GPU's
    /// records): `params` = this frame's then the previous frame's kPtParamWords.
    static bool runGradient(const pathtrace::PtCompiledScene& scene, const pathtrace::Word* params, bool havePrev,
                            u32 strataW, u32 strataH, pathtrace::Word* records, pathtrace::Word* gradient,
                            kernel::Backend backend = kernel::Backend::CpuParallel);

    const rdn::RdnReference& reference() const { return m_ref; }
    const rdn::rdnk::float4* denoisedDiffuse() const { return m_ref.outputD(); }
    const rdn::rdnk::float4* denoisedSpecular() const { return m_ref.outputS(); }
    const std::vector<rdn::rdnk::float4>& noisyDiffuse() const { return m_diffuse; }
    const std::vector<rdn::rdnk::float4>& noisySpecular() const { return m_specular; }
    const std::vector<rdn::rdnk::float4>& albedoD() const { return m_albedoD; }
    const std::vector<rdn::rdnk::float4>& albedoS() const { return m_albedoS; }
    const std::vector<f32>& depth() const { return m_depth; }
    const std::vector<rdn::rdnk::float4>& normal() const { return m_normal; }
    const std::vector<rdn::rdnk::float2>& motion() const { return m_motion; }
    const std::vector<u32>& instance() const { return m_instance; }
    const std::vector<pathtrace::Word>& gradient() const { return m_gradient; }
    const pathtrace::PtReferenceImage& image() const { return m_image; }

private:
    u32 m_width = 0;
    u32 m_height = 0;
    rdn::RdnSettings m_settings{};
    rdn::RdnReference m_ref;
    pathtrace::PtReferenceImage m_image;
    std::vector<rdn::rdnk::float4> m_diffuse, m_specular, m_normal, m_albedoD, m_albedoS;
    std::vector<f32> m_depth;
    std::vector<rdn::rdnk::float2> m_motion;
    std::vector<u32> m_instance;
    std::vector<pathtrace::Word> m_records, m_gradient;
    pathtrace::Word m_words[2u * pathtrace::kPtParamWords]{};
    u32 m_prevWidth = 0;
    bool m_havePrev = false;
};

} // namespace fuse::relight::render::denoise
