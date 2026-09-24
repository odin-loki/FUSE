#pragma once

// WP-4.5 GPU post stack: the records shared by the C++ host code and the compute kernels
// (shaders/post/pp_common.{glsl,slang} declare the same fields in the same order; the layout gate
// fuse_rp_post_gpu_layout checks the offsets). Vulkan-free: builds in the stub backend.
//
// Storage: every image-sized intermediate is f32x4 per texel in one device-local work buffer reached
// through buffer device addresses, so each pass computes in the CPU oracle's f32 and the parity gates
// compare like with like. The input (the WP-2.1 lit image / WP-2.3 forward composite, RGBA16F) and the
// optional linear-depth (R32F, metres) and velocity (RG16F / RG32F, pixels per frame) images are read
// through bindless sampled-image handles; the output is an RGBA16F storage image (bindless) plus an
// optional f32x4 dump.

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::post_gpu {

/// Workgroup edge of the image kernels (8 x 8 threads, one texel each).
inline constexpr u32 kPostTile = 8u;
/// Histogram: one 256-thread workgroup per 64 x 64 pixel tile writes one partial row.
inline constexpr u32 kPostHistTile = 64u;
inline constexpr u32 kPostHistMaxBins = 256u;
/// Pyramid levels the work buffer reserves (a 65536 x 65536 image halves to 1 x 1 in 17 levels).
inline constexpr u32 kPostMaxBloomLevels = 17u;

/// PostFrameConstants::flags
enum PostFlag : u32 {
    kPostFlagSrgb = 1u << 0,             ///< finalize with the sRGB OETF (else saturate only)
    kPostFlagAutoExposure = 1u << 1,     ///< exposure scale from the GPU adaptation state
    kPostFlagAutoEma = 1u << 2,          ///< AutoExposureParams::use_ema_adaptation
    kPostFlagExposureReset = 1u << 3,    ///< reset the adaptation state to resetEv before updating
    kPostFlagCurve = 1u << 4,            ///< pre-tonemap curve (TonemapCurveParams)
    kPostFlagGradeLiftContrast = 1u << 5,///< PostStack grade: clamp, (c + lift - 0.5) * contrast + 0.5
    kPostFlagGradeSaturation = 1u << 6,  ///< PostStack grade: saturation around Rec.709 luminance
    kPostFlagGradeGammaGain = 1u << 7,   ///< PostStack grade: pow(max(c, 0), 1 / gamma) * gain
    kPostFlagLut = 1u << 8,              ///< Look grade: tetrahedral 3D LUT in the sRGB-encoded domain
    kPostFlagVignetteStack = 1u << 9,    ///< renderer::vignette_factor
    kPostFlagVignetteLook = 1u << 10,    ///< look::kernels::vignette_gain + tint
    kPostFlagGrainStack = 1u << 11,      ///< renderer::apply_film_grain
    kPostFlagGrainLook = 1u << 12,       ///< look::kernels::FilmGrainKernel (luminance response)
    kPostFlagMbDepth = 1u << 13,         ///< motion blur has a linear-depth input
    kPostFlagDofNearBlur = 1u << 14,     ///< DOFParams::near_blur
    kPostFlagGradeClamp = 1u << 15,      ///< PostStack grade's Neutral clamp (set with any grade stage)
};

/// PostFrameConstants::toneMapper (renderer::ToneMapper values).
enum PostToneMapper : u32 {
    kPostToneAces = 0u,
    kPostToneFilmic = 1u,
    kPostToneReinhard = 2u,
    kPostToneNeutral = 3u,
    kPostToneAgx = 4u,
};

/// PostFrameConstants::curveKind (renderer::TonemapCurveKind values).
enum PostCurveKind : u32 { kPostCurveFilmic = 0u, kPostCurveReinhard = 1u, kPostCurveAces = 2u };

/// Per-frame constants, one host-visible ring slot per frame in flight, read through BDA.
struct PostFrameConstants {
    // --- addresses (0 = absent) ------------------------------------------------------------------
    u64 histPartials = 0;  ///< u32 [histGroups][kPostHistMaxBins]
    u64 exposureState = 0; ///< PostExposureState (persistent across frames)
    u64 lut = 0;           ///< f32x4 [lutSize^3], red fastest (look::Lut3D order)
    u64 dump = 0;          ///< f32x4 per pixel: the display result before RGBA16F rounding
    u64 cocs = 0;          ///< f32x2 per pixel: (CoC radius^2, splat weight)
    u64 tileMax = 0;       ///< f32x2 per motion-blur tile
    u64 neighborMax = 0;   ///< f32x2 per motion-blur tile
    u64 reserved0 = 0;
    // --- extent and bindless handles -------------------------------------------------------------
    u32 width = 0;
    u32 height = 0;
    u32 inputHdr = 0;      ///< sampled image handle (RGBA16F lit image)
    u32 inputDepth = 0;    ///< sampled image handle (R32F linear depth, metres), 0 = none
    u32 inputVelocity = 0; ///< sampled image handle (RG velocity, pixels / frame), 0 = none
    u32 output = 0;        ///< storage image handle (RGBA16F)
    u32 flags = 0;         ///< PostFlag
    u32 toneMapper = 0;    ///< PostToneMapper
    // --- exposure (renderer::AutoExposure / LuminanceHistogram) --------------------------------
    f32 exposureScale = 1.f; ///< 2^exposureEv (host, std::pow like renderer::apply_exposure_ev)
    f32 exposureEv = 0.f;    ///< manual EV + mid-grey calibration (auto EV is subtracted on the GPU)
    f32 histMinLog = -8.f;
    f32 histMaxLog = 8.f;
    u32 histBins = 64u;
    u32 histGroupsX = 0;
    u32 histGroups = 0;
    f32 percentile = 0.5f;
    f32 minEv = -8.f;
    f32 maxEv = 8.f;
    f32 targetLuminance = 0.18f;
    f32 meteringBias = 0.f;
    f32 speedUp = 3.f;
    f32 speedDown = 1.f;
    f32 emaUp = 0.15f;
    f32 emaDown = 0.05f;
    f32 deltaSeconds = 0.f;
    f32 resetEv = 0.f;
    u32 adaptValid = 0;    ///< auto_exposure_can_adapt without the enabled bit (params valid, dt > 0)
    u32 histValid = 0;     ///< luminance_histogram_params_valid
    // --- bloom ---------------------------------------------------------------------------------------
    f32 bloomThreshold = 1.f;
    f32 bloomKnee = 0.5f;
    f32 bloomIntensity = 0.f;
    f32 bloomScatter = 0.7f;
    f32 bloomTint[4] = {1.f, 1.f, 1.f, 0.f};
    // --- depth of field ----------------------------------------------------------------------------
    f32 dofFocalDistance = 10.f;
    f32 dofFocalLength = 50.f;
    f32 dofFStop = 2.8f;
    f32 dofSensorWidth = 36.f;
    f32 dofMaxRadius = 32.f;
    u32 dofReach = 32u; ///< floor(dofMaxRadius): the gather window half extent
    u32 reserved1 = 0;
    u32 reserved2 = 0;
    // --- motion blur -------------------------------------------------------------------------------
    u32 mbSamples = 16u;
    f32 mbShutter = 0.5f; ///< clamp(shutter_angle, 0, 360) / 360
    f32 mbMaxBlur = 32.f; ///< max(max_blur_px, 0)
    f32 mbSoftDepth = 0.5f;
    u32 mbTile = 16u;
    u32 mbTilesX = 0;
    u32 mbTilesY = 0;
    u32 reserved3 = 0;
    // --- tone-map curve (renderer::TonemapCurveParams, host-resolved) ------------------------------
    u32 curveKind = 0; ///< PostCurveKind
    f32 filmicA = 0.22f;
    f32 filmicB = 0.30f;
    f32 filmicC = 0.10f;
    f32 filmicD = 0.20f;
    f32 filmicE = 0.01f;
    f32 filmicF = 0.30f;
    f32 filmicWhiteScale = 1.f;
    f32 curveGamma = 1.f;
    f32 reinhardWhite = 4.f;  ///< max(white_point, 1e-4)
    f32 reinhardScale = 1.f;  ///< 2^exposure_bias
    f32 acesContrast = 1.f;
    f32 acesShoulder = 1.f;
    u32 reserved4 = 0;
    u32 reserved5 = 0;
    u32 lutSize = 0;
    // --- grade / vignette / grain ------------------------------------------------------------------
    f32 lift[4] = {0.f, 0.f, 0.f, 0.f};
    f32 invGamma[4] = {1.f, 1.f, 1.f, 0.f}; ///< 1 / gamma per channel (host, as grade_linear)
    f32 gain[4] = {1.f, 1.f, 1.f, 0.f};
    f32 vignetteTint[4] = {0.f, 0.f, 0.f, 0.f};
    f32 contrast = 1.f;
    f32 saturation = 1.f;
    f32 vignette = 0.f;
    f32 vignetteFalloff = 2.f;
    f32 vignetteRoundness = 0.f;
    f32 grain = 0.f;
    f32 grainResponse = 0.f;
    u32 seedLo = 0;
    u32 seedHi = 0;
    u32 reserved6 = 0;
    u32 reserved7 = 0;
    u32 reserved8 = 0;
};
static_assert(sizeof(PostFrameConstants) == 448, "PostFrameConstants layout (pp_common.glsl / .slang)");
static_assert(offsetof(PostFrameConstants, width) == 64, "PostFrameConstants::width");
static_assert(offsetof(PostFrameConstants, exposureScale) == 96, "PostFrameConstants::exposureScale");
static_assert(offsetof(PostFrameConstants, bloomThreshold) == 176, "PostFrameConstants::bloomThreshold");
static_assert(offsetof(PostFrameConstants, dofFocalDistance) == 208, "PostFrameConstants::dofFocalDistance");
static_assert(offsetof(PostFrameConstants, mbSamples) == 240, "PostFrameConstants::mbSamples");
static_assert(offsetof(PostFrameConstants, curveKind) == 272, "PostFrameConstants::curveKind");
static_assert(offsetof(PostFrameConstants, lift) == 336, "PostFrameConstants::lift");
static_assert(offsetof(PostFrameConstants, contrast) == 400, "PostFrameConstants::contrast");
static_assert(offsetof(PostFrameConstants, seedLo) == 428, "PostFrameConstants::seedLo");

/// Persistent adaptation state + the last frame's histogram (renderer::AutoExposureState + bins).
struct PostExposureState {
    f32 currentEv = 0.f;
    f32 measuredLuminance = 0.f;
    f32 smoothedLuminance = 0.f;
    f32 meteredLuminance = 0.f; ///< percentile luminance of the last histogram (0 = empty / invalid)
    u32 sampleCount = 0;
    u32 frames = 0;             ///< adaptation passes run since the last reset
    u32 reserved0 = 0;
    u32 reserved1 = 0;
    u32 bins[kPostHistMaxBins] = {};
};
static_assert(sizeof(PostExposureState) == 32 + 4 * kPostHistMaxBins, "PostExposureState layout");

/// Push constants (64 bytes, compute stage). `mode` selects the kernel's sub-pass; src / dst / aux are
/// f32x4 texel addresses (0 = unused) and the extents describe them.
struct PostPush {
    u64 frame = 0; ///< BDA of PostFrameConstants
    u64 src = 0;
    u64 dst = 0;
    u64 aux = 0;
    u32 mode = 0;
    u32 srcW = 0;
    u32 srcH = 0;
    u32 dstW = 0;
    u32 dstH = 0;
    u32 reserved[3] = {0u, 0u, 0u};
};
static_assert(sizeof(PostPush) == 64, "PostPush layout");

/// Sub-pass selectors (PostPush::mode).
enum PostBloomMode : u32 {
    kBloomPrefilter = 0u, ///< src (full) -> dst: soft-knee threshold
    kBloomDownH = 1u,     ///< src (sw x sh) -> dst (ceil(sw / 2) x sh)
    kBloomDownV = 2u,     ///< src (w x sh) -> dst (w x ceil(sh / 2))
    kBloomUpH = 3u,       ///< src (sw x sh) -> dst (dw x sh)
    kBloomUpV = 4u,       ///< src (dw x sh) -> dst (dw x dh), combined with aux (down level) when aux != 0
    kBloomComposite = 5u, ///< dst += aux * intensity * tint (in place)
};
enum PostDofMode : u32 { kDofCoc = 0u, kDofGather = 1u };
enum PostMotionBlurMode : u32 { kMbTileMax = 0u, kMbNeighborMax = 1u, kMbGather = 2u };
enum PostExposureMode : u32 { kExposureHistogram = 0u, kExposureAdapt = 1u };

} // namespace fuse::renderer::post_gpu
