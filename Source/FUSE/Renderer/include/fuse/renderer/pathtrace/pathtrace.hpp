#pragma once

// WP-7.3 path-tracing mode (renderer plan Phase 7 "an optional real-time path tracing mode (T3) with DLSS Ray
// Reconstruction or NRD"; execution doc WP-7.3): settings, the frame constants both the GPU tracer
// (PathTracerGpu, pt_gpu.hpp) and the tests consume, the emitter map that ties traced emissive triangles to their
// WP-7.1 light-tree emitters, the shader-binding-table layout of the T3 ray-tracing pipeline, the capability gate
// and the reconstruction choice (DLSS Ray Reconstruction / NRD / in-tree SVGF through the existing plugin
// registries). Vulkan-free; builds in the stub backend.
//
// Integrator (identical in the RT pipeline and the ray-query kernel, pt_common.{glsl,slang}; CPU reference
// pt_reference.hpp): unidirectional path tracing from a pinhole camera (box-filtered pixel jitter), GPU-scene
// materials (pt_bsdf.hpp: Lambert + GGX conductor mix), next-event estimation with the light tree at every vertex
// (area, point, spot and directional lights), BSDF sampling, power-heuristic MIS between the two for the tree's
// area emitters (the emitter map finds the emitter a BSDF ray hit and lt_pmf its selection probability at the
// previous vertex), Russian roulette from rrStartBounce on, a constant sky for escaping rays, progressive
// accumulation (sum and sum of squares per pixel, so the output carries its own standard error).
//
//   PtSettings s;                                  // ptSanitize() clamps every field
//   std::vector<u32> map;
//   buildPtEmitterMap(scene, refs.data(), count, map);   // refs: ptEmitterRefsFromTree(tree, adapterRefs, refs)
//   PtReconstruction r = selectPtReconstruction(&UpscalerRegistry::instance(), &DenoiserRegistry::instance(), {});

#include <fuse/renderer/denoise/denoiser.hpp>
#include <fuse/renderer/light_tree/light_tree.hpp>
#include <fuse/renderer/pathtrace/pt_types.hpp>
#include <fuse/renderer/upscale/upscaler.hpp>
#include <fuse/renderer/vk/render_tier.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::gpu_scene {
class GpuScene;
}

namespace fuse::renderer::pathtrace {

struct PtSettings {
    PtStrategy strategy = PtStrategy::Mis;
    u32 maxBounces = 6;            ///< scattering events per path (0..kPtMaxBounces)
    bool russianRoulette = true;
    u32 rrStartBounce = 3;         ///< first bounce that may terminate by roulette
    u32 samplesPerFrame = 1;       ///< 1..kPtMaxSamplesPerFrame paths per pixel per frame
    bool accumulate = true;        ///< progressive accumulation across frames (reset() / a camera change restart)
    u32 maxAccumulatedSamples = 0; ///< stop adding once a pixel holds this many samples (0: unlimited)
    bool guides = true;            ///< write the denoiser / RR inputs (signal, depth, normal, albedo)
    f32 clampRadiance = 0.f;       ///< > 0: clamp each path's luminance (biased; firefly control)
    f32 sky[3] = {0.f, 0.f, 0.f};  ///< radiance of escaping rays
    f32 rayTMin = 0.f;
    f32 normalBias = 1.0e-4f;
    f32 viewBias = 1.0e-5f;
    f32 farDistance = 1.0e4f;
    f32 minRoughness = 0.05f;
    u32 cullMask = 0x3u;           ///< rt::kRtMaskVisible | rt::kRtMaskShadow (ANDed with rt::kRtMaskAll)
};

/// Every field clamped to its documented range.
PtSettings ptSanitize(const PtSettings& s);

/// Pinhole camera of the frame (Vulkan clip; forward or reversed z: rays go through the view-projection inverse
/// at clip z = 0.5 from `position`).
struct PtCamera {
    f32 viewProj[16] = {};             ///< column-major
    f32 position[3] = {0.f, 0.f, 0.f};
    f32 forward[3] = {0.f, 0.f, -1.f}; ///< view axis (linear depth; normalised on use)
};

struct PtFrameParams {
    u32 width = 0;
    u32 height = 0;
    u32 frameIndex = 0;
    u32 seed = 0x5EEDu;
    u32 sampleBase = 0;    ///< samples already accumulated (RNG sample index offset)
    u32 lightCount = 0;
    bool lightTree = false;///< a light-tree header is supplied (else NEE and emitter hits are off)
    bool emitterMap = false;
    u32 emitterMapSlots = 0;
};

/// Fills `out` (addresses 0). False for a zero extent or a singular view-projection.
bool buildPtFrameConstants(const PtSettings& settings, const PtCamera& camera, const PtFrameParams& params,
                           PtFrameConstants& out);

/// Column-major 4x4 inverse in double (false when singular).
bool ptInvert(const f32 in[16], f64 out[16]);

// --- emitter map ------------------------------------------------------------------------------------------

/// One traced emissive triangle that is also light-tree emitter `emitter`.
struct PtEmitterRef {
    u32 instance = kPtInvalid; ///< GPU-scene instance slot
    u32 triangle = 0;          ///< mesh triangle (BLAS primitive, MTRI order)
    u32 emitter = kPtInvalid;  ///< LightTreeEmitter index
};

/// The emitter map words (pt_types.hpp) for `instanceSlots` slots whose meshes have triangleCounts[slot] triangles
/// (0: none). False (out cleared) when a ref names a slot / triangle out of range or two refs the same triangle.
bool buildPtEmitterMap(const u32* triangleCounts, u32 instanceSlots, const PtEmitterRef* refs, u32 count, std::vector<u32>& out);
/// Same, with the triangle counts of the scene's live instances (GpuScene CPU mirror; works in CPU-only mode).
bool buildPtEmitterMap(const gpu_scene::GpuScene& scene, const PtEmitterRef* refs, u32 count, std::vector<u32>& out);
/// Refs of every triangle emitter of `tree` whose source indexes `adapterRefs` (the WP-7.1 adapters'
/// EmissiveTriangleRef list, appendEmissiveTriangles / appendSceneEmissiveTriangles). Returns the count.
u32 ptEmitterRefsFromTree(const light_tree::LightTree& tree, const std::vector<light_tree::EmissiveTriangleRef>& adapterRefs,
                          std::vector<PtEmitterRef>& out);
/// Emitter-map lookup (the kernels' pt_emitter_of): emitter index or kPtInvalid.
u32 ptEmitterLookup(const u32* words, u32 wordCount, u32 slots, u32 instance, u32 triangle);

// --- T3 ray-tracing pipeline: shader binding table ----------------------------------------------------------

/// Shader groups of the path-tracing pipeline, in pipeline group order.
enum PtShaderGroup : u32 {
    kPtGroupRaygen = 0,
    kPtGroupMiss = 1,      ///< every ray type: payload t = -1 (radiance: escaped; shadow: visible)
    kPtGroupHit = 2,       ///< radiance rays (sbtRecordOffset 0): closest hit (surface interaction + material
                           ///< dispatch) + any hit (skips the triangle the ray starts on)
    kPtGroupShadowHit = 3, ///< shadow rays (sbtRecordOffset 1): any hit only; the ray ends at the first
                           ///< accepted hit (TERMINATE_ON_FIRST_HIT | SKIP_CLOSEST_HIT)
    kPtGroupCount = 4,
};
inline constexpr u32 kPtMissRecords = 1u;
inline constexpr u32 kPtHitRecords = 2u;  ///< one per ray type (sbtRecordStride 2: one geometry per BLAS)

struct PtSbtRegion {
    u64 offset = 0; ///< from the SBT buffer's base address (multiple of the base alignment)
    u64 stride = 0;
    u64 size = 0;
};

struct PtSbtLayout {
    PtSbtRegion raygen{};   ///< stride == size (VUID-vkCmdTraceRaysKHR-size-04023)
    PtSbtRegion miss{};
    PtSbtRegion hit{};
    u64 bytes = 0;          ///< buffer size
    u32 handleSize = 0;
    u32 recordStride = 0;   ///< handle size rounded up to the handle alignment
    bool valid = false;
};

/// Region layout from VkPhysicalDeviceRayTracingPipelinePropertiesKHR (handle size / alignment, base alignment,
/// max stride). Invalid (valid = false) for zero or non-power-of-two alignments or a stride above maxStride.
PtSbtLayout computePtSbtLayout(u32 handleSize, u32 handleAlignment, u32 baseAlignment, u32 maxStride = 4096u,
                               u32 missRecords = kPtMissRecords, u32 hitRecords = kPtHitRecords);

// --- capability gate ------------------------------------------------------------------------------------------

struct PtCapabilities {
    bool rayQuery = false;          ///< T2: the ray-query compute integrator can run
    bool rayTracingPipeline = false;///< T3: VK_KHR_ray_tracing_pipeline enabled (VulkanDevice enables it only
                                    ///< when the tier cap is >= T3, WP-0.1)
    const char* reason = "no device";         ///< why rayQuery is false, or "ok"
    const char* pipelineReason = "no device"; ///< why rayTracingPipeline is false, or "ok"
};

/// The rule over the caps a device was created with (pure; unit-tested with synthetic caps).
PtCapabilities evaluatePtCapabilities(const RendererCaps& caps);

// --- reconstruction (DLSS RR / NRD / SVGF) through the existing plugin registries ---------------------------

/// Registry id of NVIDIA DLSS Ray Reconstruction in the upscaler registry (plugins/nvidia nv_backends.hpp
/// kDlssRrName; spelled here so the path tracer does not link the plugin).
inline constexpr const char* kPtRayReconstructionBackend = "dlss_rr";

enum class PtReconstructionKind : u8 {
    Accumulate = 0,        ///< progressive accumulation only (reference / screenshots)
    RayReconstruction = 1, ///< a temporal upscaler registered as kPtRayReconstructionBackend (denoise + upscale)
    Denoiser = 2,          ///< an IDenoiser from the denoiser registry (NRD RELAX / REBLUR plugin, or in-tree SVGF)
};

struct PtReconstructionRequest {
    bool allowRayReconstruction = true;
    bool allowDenoiser = true;
    denoise::DenoiserMethod denoiserMethod = denoise::DenoiserMethod::Relax; ///< preferred IDenoiser method
    bool allowInTreeFallback = true; ///< SVGF when no plugin serves the method
    bool haveHitDistance = false;    ///< the NRD radiance + hit-distance packing is produced (not yet: open)
    bool haveNativeFrame = true;     ///< the renderer can bind native textures (plugins)
};

struct PtReconstruction {
    PtReconstructionKind kind = PtReconstructionKind::Accumulate;
    const char* backend = "";        ///< registry id ("dlss_rr", "nrd", "svgf"), "" for Accumulate
    denoise::DenoiserMethod method = denoise::DenoiserMethod::Svgf;
    bool fallback = false;           ///< not the first choice
    const char* reason = "";
};

/// DLSS RR when a non-stub temporal Vulkan backend named kPtRayReconstructionBackend is registered, else the
/// denoiser registry's selection for the GI signal (preferred method, fallback to the in-tree SVGF), else
/// accumulation. Either registry may be null.
PtReconstruction selectPtReconstruction(const upscale::UpscalerRegistry* upscalers, const denoise::DenoiserRegistry* denoisers,
                                        const PtReconstructionRequest& request);

} // namespace fuse::renderer::pathtrace
