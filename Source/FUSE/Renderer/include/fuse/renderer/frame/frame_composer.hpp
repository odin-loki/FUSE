#pragma once
// Frame composer (docs/unification/RENDERER-EXECUTION.md, "Frame integration"): records one complete deferred
// frame on render graph v2 from the finished packages' public APIs, every stage optional behind FrameSettings:
//
//   GPU scene (WP-1.1, caller-owned) -> instance cull + Hi-Z (WP-1.3) -> visibility buffer (WP-1.4, jittered)
//   -> material resolve into the G-buffer (WP-1.5) -> motion vectors (WP-4.1)
//   -> VSM (WP-3.1 marking / allocation, WP-3.2 raster)            [vsm]
//   -> T2: TLAS (WP-6.0) -> RT shadows (WP-6.2) -> SVGF (WP-6.4) -> frame.shadow_pack   [rtShadows, denoise]
//   -> DDGI update (WP-6.1: T0 global SDF / T2 ray query)          [ddgi]
//   -> clustered lighting (WP-2.1 / 2.2: VSM visibility, RT visibility, DDGI indirect)
//   -> GTAO / SSR / SSGI (WP-6.3)                                  [ssfx]
//   -> atmosphere LUTs (WP-8.2) -> frame.sky (sky on background pixels, aerial perspective on geometry)
//   -> froxel fog inject / temporal / integrate / apply (WP-8.1)   [fog]
//   -> frame.gather -> volumetric clouds (WP-8.3)                  [clouds]
//   -> gaussian splats (WP-9.2)                                    [splats]
//   -> frame.resolve (render-resolution scene colour)
//   -> TAAU (WP-4.1) or FSR 3.1 (WP-4.2)                           [upscaler]
//   -> GPU post stack (WP-4.5)                                     [post]
//   -> output (RGBA16F, display resolution)
//
// Tier-driven: T0 never touches ray query (DDGI traces the global SDF, no TLAS / RT effects); T2 builds the
// TLAS, traces DDGI with ray query and adds RT shadows + the denoiser. No stage records a hand barrier: every
// access is declared, the composer's own passes included.
//
// Frame protocol (the caller owns the GpuScene, the UploadQueue and the executor):
//   scene.beginFrame(serial); composer.beginSceneFrame(serial);  ... scene edits ...
//   scene.commit(); composer.commitScene(); upload.flush();
//   composer.beginFrame(desc, settings);  graph.reset();  outputs = composer.addFrame(graph);  execute(graph)
//   ... once the frame retired: composer.collectRetired(serial); scene.collectRetired(serial); bindless...
#include <fuse/renderer/atmosphere/atmosphere_gpu.hpp>
#include <fuse/renderer/atmosphere/atmosphere_luts.hpp>
#include <fuse/renderer/clouds/volumetric_clouds.hpp>
#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/denoise/svgf_denoiser.hpp>
#include <fuse/renderer/frame/frame_types.hpp>
#include <fuse/renderer/gi/gpu/ddgi_gpu.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/gsplat/gsplat.hpp>
#include <fuse/renderer/lighting/gpu/clustered_lighting.hpp>
#include <fuse/renderer/material_resolve/material_resolve.hpp>
#include <fuse/renderer/postprocess/gpu/post_stack_gpu.hpp>
#include <fuse/renderer/rt/acceleration_structures.hpp>
#include <fuse/renderer/rt_effects/rt_effects.hpp>
#include <fuse/renderer/shadow/vsm/virtual_shadow_map.hpp>
#include <fuse/renderer/shadow/vsm_raster/vsm_shadows.hpp>
#include <fuse/renderer/ssfx_gpu/ssfx_gpu.hpp>
#include <fuse/renderer/temporal/taau_gpu.hpp>
#include <fuse/renderer/temporal/temporal_motion.hpp>
#include <fuse/renderer/upscale_backends/fsr3/fsr3_gpu.hpp>
#include <fuse/renderer/visbuffer/visbuffer.hpp>
#include <fuse/renderer/volumetric/gpu/froxel_fog.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {
class GpuAllocator;
class UploadQueue;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::frame {

enum class FrameTier : u8 {
    T0 = 0, ///< compute + raster only (no ray query)
    T2 = 2, ///< + TLAS, RT shadows, denoiser, ray-query DDGI
};

enum class FrameUpscaler : u8 {
    None = 0, ///< post (or the output) runs at render resolution
    Taau,     ///< WP-4.1 TaauGpu
    Fsr3,     ///< WP-4.2 Fsr3Gpu
};

enum class FrameKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL (the composer's own kernel; packages use their own Auto)
    Slang,
    Glsl,
};

/// Stages of the frame (FrameStats::ran bits, FrameComposer::available()).
enum FrameStage : u32 {
    kStageScene = 1u << 0,      ///< cull + visibility buffer + material resolve + motion (always)
    kStageVsm = 1u << 1,
    kStageTlas = 1u << 2,
    kStageRtShadows = 1u << 3,
    kStageDenoise = 1u << 4,
    kStageDdgi = 1u << 5,
    kStageLighting = 1u << 6,   ///< always
    kStageSsfx = 1u << 7,
    kStageAtmosphere = 1u << 8, ///< the LUTs (for sky, aerial perspective or clouds)
    kStageSky = 1u << 9,        ///< frame.sky (sky and / or aerial perspective)
    kStageFog = 1u << 10,
    kStageClouds = 1u << 11,
    kStageSplats = 1u << 12,
    kStageResolve = 1u << 13,   ///< always
    kStageTaau = 1u << 14,
    kStageFsr3 = 1u << 15,
    kStagePost = 1u << 16,
};

struct FrameComposerDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    UploadQueue* upload = nullptr;     ///< T2: the acceleration structures' uploads
    gpu_scene::GpuScene* scene = nullptr;
    FrameTier tier = FrameTier::T0;    ///< T2 fails init below the T2 gate
    u32 renderWidth = 0;
    u32 renderHeight = 0;
    u32 displayWidth = 0;              ///< >= render (0 = render)
    u32 displayHeight = 0;
    u32 instanceCapacity = 256;
    u32 meshCapacity = 16;
    u32 lightCapacity = 64;
    ClusterDesc clusters{};
    vsm::VsmClipmapDesc vsmClipmap{};
    u32 vsmPoolPagesX = 16;
    u32 vsmPoolPagesY = 16;
    u32 vsmLocalPagesX = 8;
    u32 vsmLocalPagesY = 2;
    DDGIDesc ddgiVolume{};
    DdgiCpuConfig ddgiConfig{};
    u32 ddgiSdfCapacity = 64;
    u32 maxSplats = 4096;
    u32 maxSplatEntries = 1u << 16;
    taau_kernel::Settings taau{};
    fsr3::Fsr3Settings fsr3{};
    FrameKernelLanguage language = FrameKernelLanguage::Auto;
    u32 framesInFlight = 3;
};

/// Camera of the frame (unjittered; the composer jitters the visibility buffer when an upscaler runs).
struct FrameCamera {
    f32 eye[3] = {0.f, 1.5f, 5.f};
    f32 target[3] = {0.f, 1.f, 0.f};
    f32 fovY = 1.0f;
    f32 nearPlane = 0.1f;
    f32 farPlane = 200.f;
};

/// The sun: a directional light of the GpuScene (its slot is shadowed by the VSM / RT shadows) plus the
/// radiometric terms of the sky / DDGI / clouds (the scene light keeps its own colour x intensity).
struct FrameSun {
    u32 slot = 0xFFFFFFFFu;               ///< GpuScene light slot (Directional) or none
    f32 toSun[3] = {0.3f, 0.8f, 0.2f};    ///< surface -> sun (normalised by the composer)
    f32 illuminance[3] = {1.f, 1.f, 1.f}; ///< sky / clouds sun illuminance, DDGI sun irradiance
};

struct FrameSettings {
    bool vsm = true;
    vsm::VsmFilterDesc vsmFilter{};
    bool ddgi = true;
    f32 ddgiSkyRadiance[3] = {0.05f, 0.07f, 0.1f};
    bool rtShadows = true; ///< T2 only
    bool denoise = true;   ///< T2 RT shadows through SVGF (else light.shade reads the raw 1-spp visibility)
    denoise::SvgfSettings denoiser{};  ///< the signal is forced to Shadow
    bool ssfx = true;
    ssfx_gpu::SsfxGpuSettings ssfxSettings{};
    bool sky = true;
    bool aerialPerspective = true;
    bool sunDisk = true;
    atmosphere::AtmosphereLutSettings atmosphere{};
    bool fog = true;
    volumetric_gpu::FroxelFogSettings fogSettings{};
    bool clouds = true;
    clouds::CloudSettings cloudSettings{}; ///< resolution.outWidth / outHeight follow the render extent
    bool splats = false;
    gsplat::GsSettings splatSettings{};
    FrameUpscaler upscaler = FrameUpscaler::Taau;
    bool post = true;
    post_gpu::PostGpuSettings postSettings{};
    f32 ambient[3] = {0.03f, 0.035f, 0.045f};
};

struct FrameDesc {
    u64 serial = 0;          ///< frame serial (ring slots, retirement)
    u32 frameIndex = 0;      ///< jitter / temporal sequence index
    FrameCamera camera{};
    FrameSun sun{};
    bool resetHistory = false; ///< camera cut: temporal stages start over
    f32 deltaSeconds = 1.f / 60.f;
};

/// What addFrame recorded (graph refs for readback / chaining; invalid when the stage did not run).
struct FrameGraphOutputs {
    gpu_scene::GpuSceneGraphRefs scene{};
    material_resolve::ResolveGraphRefs gbuffer{};
    lighting_gpu::LightingGraphRefs lighting{};
    rg::TextureRef lit;        ///< WP-2.1 lit image
    rg::TextureRef ssfx;       ///< WP-6.3 composed image
    rg::TextureRef sky;        ///< frame.sky output
    rg::TextureRef fog;        ///< WP-8.1 applied image
    rg::BufferRef splats;      ///< WP-9.2 image buffer (output section: GsplatRenderer copy source Output)
    rg::TextureRef sceneColor; ///< frame.resolve output (render resolution, the upscaler input)
    rg::TextureRef upscaled;   ///< TAAU / FSR 3 output (display resolution)
    rg::TextureRef output;     ///< the frame's final image (post, else upscaled, else sceneColor)
    rg::BufferRef motion;      ///< WP-4.1 motion (f32x2) / depth (f32) buffers
    rg::BufferRef motionDepth;
    rg::BufferRef background;  ///< frame.gather outputs (f32x4 / f32 per pixel)
    rg::BufferRef distance;
    rg::BufferRef clouds;      ///< WP-8.3 frame buffer (result section at FrameComposer::cloudsResultOffset())
    rg::BufferRef denoised;    ///< WP-6.4 output (f32x4 per pixel) and the frame.shadow_pack plane (f32 per pixel)
    rg::BufferRef visibility;
};

struct FrameStats {
    u32 ran = 0;          ///< FrameStage bits of the last addFrame
    u32 composerPasses = 0; ///< frame.* passes of the last addFrame
    u32 imageRebuilds = 0;  ///< composer-owned images / buffers (re)created
    u32 sampledBinds = 0;   ///< sampled-handle registrations (steady state: 0 per frame)
};

class FrameComposer {
public:
    FrameComposer() = default;
    ~FrameComposer();
    FrameComposer(const FrameComposer&) = delete;
    FrameComposer& operator=(const FrameComposer&) = delete;

    /// Initialises every package the tier allows. The mandatory ones (cull, visibility buffer, resolve,
    /// motion, lighting) fail init; optional ones only clear their available() bit (reason() names the first).
    bool init(const FrameComposerDesc& desc);
    /// The caller must have retired every frame that used the resources.
    void destroy();
    bool valid() const { return m_initialized; }
    const char* reason() const { return m_reason; }
    FrameTier tier() const { return m_desc.tier; }
    /// FrameStage bits of the stages that can run.
    u32 available() const { return m_available; }
    const char* kernelLanguage() const { return m_language; }

    /// T0 DDGI scene (analytic SDF primitives + Lambertian rows, gi_gpu::sdfSceneFromBoxes).
    bool setSdfScene(const compute::SdfObject* objects, u32 objectCount, const gi_gpu::DdgiSurface* surfaces,
                     u32 surfaceCount);
    /// WP-9.2 splat asset (no frame using the splat buffer may be in flight).
    bool setSplats(const gsplat::GsSplat* splats, u32 count, u32 shDegree);

    /// T2: AccelerationStructures::beginFrame (after scene.beginFrame). No-op at T0.
    void beginSceneFrame(u64 serial);
    /// T2: AccelerationStructures::commit (after scene.commit, before upload.flush). True at T0.
    bool commitScene();

    /// Writes every enabled stage's frame constants (in dependency order). False when a stage failed.
    bool beginFrame(const FrameDesc& desc, const FrameSettings& settings);
    /// Records the frame (after beginFrame). The graph must execute before the next beginFrame.
    FrameGraphOutputs addFrame(rg::Graph& graph);
    /// Destroys every package's resources retired at serials <= completedSerial.
    void collectRetired(u64 completedSerial);

    // --- inspection ------------------------------------------------------------------------------
    const FrameStats& stats() const { return m_stats; }
    u32 renderWidth() const { return m_desc.renderWidth; }
    u32 renderHeight() const { return m_desc.renderHeight; }
    /// Extent of FrameGraphOutputs::output for the last beginFrame.
    u32 outputWidth() const { return m_outputWidth; }
    u32 outputHeight() const { return m_outputHeight; }
    const Texture& sceneColorImage() const { return m_resolveImage.image; }
    const material_resolve::MaterialResolve& gbuffer() const { return m_resolve; }
    const gsplat::GsplatRenderer& splatRenderer() const { return m_gsplat; }
    const FrameConstants& constants() const { return m_constants; }
    /// Byte offset of the clouds result inside FrameGraphOutputs::clouds (this frame).
    u64 cloudsResultOffset() const;
    /// Byte offset of the splat output inside FrameGraphOutputs::splats (this frame).
    u64 splatOutputOffset() const;
    /// View-projection the visibility buffer used this frame (jittered when an upscaler runs).
    const f32* drawViewProj() const { return m_drawViewProj; }

private:
    struct OwnedImage {
        Texture image{};
        BindlessSlotHandle storage{};
        u32 storageHandle = 0;
        u32 layout = 0;
        u8 queue = rg::kNoQueue;
    };
    struct OwnedBuffer {
        Buffer buffer{};
        u8 queue = rg::kNoQueue;
    };
    struct SampledSlot {
        void* image = nullptr;
        BindlessSlotHandle slot{};
        u32 handle = 0;
    };
    struct PassRecord {
        FrameComposer* self = nullptr;
        FramePush push{};
        u32 groups[2] = {1u, 1u};
    };
    static constexpr u32 kSampledSlots = 12u;
    static constexpr u32 kMaxRecords = 8u;

    bool createPipeline();
    bool createTargets();
    void destroyTargets();
    bool createImage(OwnedImage& o, u32 width, u32 height, const char* name);
    bool createBuffer(OwnedBuffer& o, u64 bytes, const char* name);
    rg::TextureRef importImage(rg::Graph& graph, OwnedImage& o, const char* name);
    rg::BufferRef importBuffer(rg::Graph& graph, OwnedBuffer& o, const char* name);
    u32 sampledHandle(const Texture& texture);
    PassRecord* nextRecord(u32 mode);
    static void recordDispatch(const rg::PassContext& context, void* user);
    void computeMatrices(const FrameDesc& desc, bool jitter);

    FrameComposerDesc m_desc{};
    bool m_initialized = false;
    const char* m_reason = "not initialised";
    const char* m_language = "none";
    u32 m_available = 0;

    // packages
    culling::InstanceCuller m_culler;
    visbuffer::VisBuffer m_vb;
    material_resolve::MaterialResolve m_resolve;
    temporal::TemporalMotion m_motion;
    vsm::VirtualShadowMap m_vsm;
    vsm::VsmShadows m_vsmShadows;
    rt::AccelerationStructures m_as;
    rt_effects::RtEffects m_rtfx;
    denoise::SvgfDenoiser m_svgf;
    gi_gpu::DdgiGpu m_ddgi;
    lighting_gpu::ClusteredLighting m_lighting;
    ssfx_gpu::SsfxGpu m_ssfx;
    atmosphere::AtmosphereGpu m_atmosphere;
    volumetric_gpu::FroxelFog m_fog;
    clouds::VolumetricClouds m_clouds;
    gsplat::GsplatRenderer m_gsplat;
    temporal::TaauGpu m_taau;
    fsr3::Fsr3Gpu m_fsr3;
    post_gpu::PostStackGpu m_post;

    // composer-owned resources
    void* m_layoutHandle = nullptr; ///< VkPipelineLayout (bindless set + 16-byte push)
    void* m_pipeline = nullptr;
    Buffer m_frameRing{};           ///< per ring slot: [gather / resolve constants][sky constants]
    u64 m_frameAddress = 0;
    u64 m_skyFrameAddress = 0;
    u32 m_skyInColor = 0;
    BindlessSlotHandle m_sampler{}; ///< material sampler (resolve)
    u32 m_samplerHandle = 0;
    Buffer m_viewRing{};            ///< RtfxShadowView ring (the denoised visibility)
    u64 m_viewAddress = 0;
    OwnedImage m_skyImage{};
    OwnedImage m_resolveImage{};
    OwnedBuffer m_background{};     ///< f32x4 (clouds background)
    OwnedBuffer m_distance{};       ///< f32 (clouds scene distance)
    OwnedBuffer m_visibility{};     ///< f32 (packed denoised shadow)
    SampledSlot m_sampled[kSampledSlots]{};
    u32 m_sampledCount = 0;
    PassRecord m_records[kMaxRecords]{};
    u32 m_recordCount = 0;

    // frame state
    FrameDesc m_frame{};
    FrameSettings m_settings{};
    FrameConstants m_constants{};
    u32 m_plan = 0;                 ///< FrameStage bits planned by beginFrame
    bool m_begun = false;
    bool m_hasPrev = false;
    f32 m_viewProj[16] = {};
    f32 m_prevViewProj[16] = {};
    f32 m_drawViewProj[16] = {};
    f32 m_prevDrawViewProj[16] = {};
    f32 m_view[16] = {};
    f32 m_proj[16] = {};
    f32 m_prevView[16] = {};
    f32 m_jitter[2] = {0.f, 0.f};
    const Texture* m_chain[6] = {}; ///< the HDR image each stage reads (beginFrame's plan)
    u32 m_outputWidth = 0;
    u32 m_outputHeight = 0;
    FrameStats m_stats{};
};

} // namespace fuse::renderer::frame
