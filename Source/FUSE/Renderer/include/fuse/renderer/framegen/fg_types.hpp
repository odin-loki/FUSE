#pragma once

// WP-4.4 FSR 3.1 frame generation: host-side records, conventions, the dispatch plan and the clean-room port of the
// SDK's per-dispatch constant setup. Vulkan-free (built and gated in the stub tree).
//
// Upstream: AMD FidelityFX SDK v1.1.4 (tag v1.1.4, commit c6efa6bf7f2027b3ec94f28578bb5965eabb9e55, MIT), optical flow
// effect 1.1.2 and frame interpolation effect 1.1.3 (the FSR 3.1.4 frame generation). The GPU side is the vendored
// Vulkan GLSL passes (Engine/lib/fidelityfx/shaders/vk/{opticalflow,frameinterpolation}/*.glsl + include/FidelityFX/
// gpu/{opticalflow,frameinterpolation,spd}/*), compiled verbatim by cmake/rp_wp44.cmake. The host side (this file,
// fg_host.cpp, fg_gpu.cpp) replaces sdk/src/components/{opticalflow,frameinterpolation}/*.cpp, the ffx-api frame
// generation provider (ffx_provider_framegeneration.cpp: OF context at display size, block size 8, optical flow scale
// 1 / display size) and the FidelityFX Vulkan backend, none of which is vendored. The frame-interpolation swapchain
// (pacing thread, UI composition) is replaced by FUSE's present-side code: the pacing model is
// present/latency/present_timing.hpp, the UI composition the FUSE pass "fg.ui_composite".
//
// Conventions (FUSE side = temporal/temporal_types.hpp, upscale/upscale_inputs.hpp):
//   interpolation source  the HUD-less, display-referred colour of the frame (sRGB-encoded values, transfer function
//            0 = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB), display resolution, any sampled format with at least RGB;
//            the previous frame's copy is kept internally (same format).
//   depth    FUSE linear view depth per render pixel (<= 0 = sky), converted by "fg.convert" to reverse-Z device depth
//            (fsr3::fsr3_device_depth; passes built with FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=1).
//   motion   FUSE UV motion (current - previous, unjittered, render resolution); the FI shaders want the UV offset to
//            the previous position: motionVectorScale = -renderSize (cbFI.fMotionVectorScale = -1, like WP-4.2),
//            FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=1, JITTERED_MOTION_VECTORS=0.
//   jitter   FSR sign (content displacement) = -jitter_px (fsr3::fsr3_jitter_offset); unused by this permutation.
//   UI       separate premultiplied RGBA display-resolution texture, composited over both the interpolated and the
//            real frame by "fg.ui_composite" (out = src * (1 - ui.a) + ui.rgb): the interpolation never sees the UI.
//   output   the interpolated frame lands halfway between the previous and the current interpolation source
//            (FSR 3.1 interpolates at t = 0.5: game-MV and optical-flow samples at +/- mv / 2).

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::framegen {

// ---- Upstream pin ---------------------------------------------------------------------------------------------
inline constexpr const char* kFidelityFxSdkVersion = "1.1.4";
inline constexpr const char* kFidelityFxSdkCommit = "c6efa6bf7f2027b3ec94f28578bb5965eabb9e55";
inline constexpr u32 kOpticalFlowVersion[3] = {1u, 1u, 2u};        ///< FFX_OPTICALFLOW_VERSION_*
inline constexpr u32 kFrameInterpolationVersion[3] = {1u, 1u, 3u}; ///< FFX_FRAMEINTERPOLATION_VERSION_*

// ---- GPU records (std140 uniform blocks of the vendored callbacks_glsl.h) -------------------------------------

/// cbFI (FrameInterpolationConstants in ffx_frameinterpolation_private.h). 176 bytes.
struct FgFiConstants {
    i32 renderSize[2] = {0, 0};
    i32 displaySize[2] = {0, 0};
    f32 displaySizeRcp[2] = {0.f, 0.f};
    f32 cameraNear = 0.f;
    f32 cameraFar = 0.f;
    i32 upscalerTargetSize[2] = {0, 0};
    i32 mode = 0;
    i32 reset = 0;
    f32 deviceToViewDepth[4] = {0.f, 0.f, 0.f, 0.f};
    f32 deltaTime = 0.f;
    i32 hudLessAttachedFactor = 0;
    i32 distortionFieldSize[2] = {0, 0};
    f32 opticalFlowScale[2] = {0.f, 0.f};
    i32 opticalFlowBlockSize = 0;
    u32 dispatchFlags = 0;
    i32 maxRenderSize[2] = {0, 0};
    i32 opticalFlowHalfResMode = 0;
    i32 numInstances = 0;
    i32 interpolationRectBase[2] = {0, 0};
    i32 interpolationRectSize[2] = {0, 0};
    f32 debugBarColor[3] = {0.f, 0.f, 0.f};
    u32 backBufferTransferFunction = 0;
    f32 minMaxLuminance[2] = {0.f, 0.f};
    f32 tanHalfFov = 0.f;
    f32 pad1 = 0.f;
    f32 jitter[2] = {0.f, 0.f};
    f32 motionVectorScale[2] = {0.f, 0.f};
};
static_assert(sizeof(FgFiConstants) == 176u && offsetof(FgFiConstants, deviceToViewDepth) == 48u &&
                  offsetof(FgFiConstants, opticalFlowScale) == 80u && offsetof(FgFiConstants, maxRenderSize) == 96u &&
                  offsetof(FgFiConstants, interpolationRectBase) == 112u && offsetof(FgFiConstants, debugBarColor) == 128u &&
                  offsetof(FgFiConstants, backBufferTransferFunction) == 140u && offsetof(FgFiConstants, tanHalfFov) == 152u &&
                  offsetof(FgFiConstants, jitter) == 160u && offsetof(FgFiConstants, motionVectorScale) == 168u,
              "FgFiConstants must match cbFI (std140)");

/// cbInpaintingPyramid (InpaintingPyramidConstants). 16 bytes.
struct FgInpaintingPyramidConstants {
    u32 mips = 0;
    u32 numWorkGroups = 0;
    u32 workGroupOffset[2] = {0u, 0u};
};
static_assert(sizeof(FgInpaintingPyramidConstants) == 16u, "cbInpaintingPyramid layout");

/// cbOF (OpticalflowConstants in ffx_opticalflow_private.h). 32 bytes.
struct FgOfConstants {
    i32 inputLumaResolution[2] = {0, 0};
    u32 opticalFlowPyramidLevel = 0;
    u32 opticalFlowPyramidLevelCount = 0;
    i32 frameIndex = 0;
    u32 backbufferTransferFunction = 0;
    f32 minMaxLuminance[2] = {0.f, 0.f};
};
static_assert(sizeof(FgOfConstants) == 32u && offsetof(FgOfConstants, frameIndex) == 16u &&
                  offsetof(FgOfConstants, minMaxLuminance) == 24u,
              "FgOfConstants must match cbOF (std140)");

/// cbOF_SPD (OpticalFlowSpdConstants in ffx_opticalflow.cpp). 32 bytes.
struct FgOfSpdConstants {
    u32 mips = 0;
    u32 numWorkGroups = 0;
    u32 workGroupOffset[2] = {0u, 0u};
    u32 numWorkGroupsOpticalFlowInputPyramid = 0;
    u32 pad0 = 0, pad1 = 0, pad2 = 0;
};
static_assert(sizeof(FgOfSpdConstants) == 32u, "cbOF_SPD layout");

/// Push constants of the FUSE-authored "fg.convert" (fg_convert.{slang,comp}). 16 bytes.
struct FgConvertPush {
    u32 renderW = 0;
    u32 renderH = 0;
    f32 nearPlane = 0.f;
    f32 farPlane = 0.f;
};
static_assert(sizeof(FgConvertPush) == 16u, "FgConvertPush layout");

/// Push constants of the FUSE-authored "fg.ui_composite" (fg_ui_composite.{slang,comp}). 16 bytes.
struct FgCompositePush {
    u32 width = 0;
    u32 height = 0;
    u32 hasUi = 0;
    u32 pad = 0;
};
static_assert(sizeof(FgCompositePush) == 16u, "FgCompositePush layout");

/// CPU twins of the FUSE passes (the Lavapipe gates compare the GPU with these bit for bit).
f32 fg_device_depth(f32 linearDepth, f32 nearPlane, f32 farPlane); ///< = fsr3::fsr3_device_depth
math::Vec4 fg_composite(const math::Vec4& source, const math::Vec4& ui, bool hasUi);

// ---- Passes ---------------------------------------------------------------------------------------------------

enum class FgPass : u8 {
    Convert = 0,             ///< FUSE: linear depth / UV motion buffers -> the FI input textures
    OfPrepareLuma,
    OfLuminancePyramid,
    OfScdHistogram,
    OfScdDivergence,
    OfSearch,                ///< vendored search v5 (wave32 / wave64 devices only)
    OfSearchPortable,        ///< FUSE: the same search with workgroup-memory reductions (any subgroup size)
    OfFilter,
    OfScale,
    FiReconstructAndDilate,
    FiSetup,
    FiReconstructPrevDepth,
    FiGameMotionVectorField,
    FiGameVectorFieldInpaintingPyramid,
    FiOpticalFlowVectorField,
    FiDisocclusionMask,
    FiInterpolation,
    FiInpaintingPyramid,
    FiInpainting,
    UiComposite,             ///< FUSE: UI over the interpolated / real frame
    Count,
};
inline constexpr u32 kFgPassCount = static_cast<u32>(FgPass::Count);
const char* fg_pass_name(FgPass pass);            ///< render-graph pass name ("fg.of.search", ...)
bool fg_pass_is_optical_flow(FgPass pass);        ///< binding names resolve against the optical-flow tables
bool fg_pass_is_vendored(FgPass pass);

// ---- Resources ------------------------------------------------------------------------------------------------

/// Logical resources the shaders bind (SDK resource identifiers, collapsed; per-dispatch aliases resolved by the plan).
enum class FgResource : u8 {
    None = 0,
    SourceDepth,        ///< caller's f32 linear depth buffer (fg.convert)
    SourceMotion,       ///< caller's f32x2 UV motion buffer (fg.convert)
    InputDepth,         ///< R32F render: reverse-Z device depth (r_input_depth, fgOutDepth)
    InputMotion,        ///< RG32F render (r_input_motion_vectors, fgOutMotion)
    CurrentSource,      ///< caller's HUD-less colour (r_current_interpolation_source, r_present_backbuffer, r_input_color)
    PreviousSource,     ///< previous frame's copy (r_previous_interpolation_source)
    DilatedDepth,       ///< R32F render
    DilatedMotion,      ///< RG16F render
    ReconPrevDepth,     ///< R32_UINT render
    ReconInterpDepth,   ///< R32_UINT render
    DisocclusionMask,   ///< RG8 render
    GameMvX,            ///< R32_UINT render
    GameMvY,
    OfMvX,              ///< R32_UINT render
    OfMvY,
    OpticalFlowVector,  ///< RG16_SINT OF level-0 size: the OF result (FI r_optical_flow, OF level-0 filter output)
    OpticalFlowScd,     ///< R32_UINT 3 x 1: scene-change detection output
    Output,             ///< RGBA16F display: the interpolated HUD-less frame (r_output / rw_output)
    InpaintingPyramid,  ///< RGBA16F display / 2, full chain (SRV; UAV one view per mip)
    Counters,           ///< 2 x u32 storage buffer
    DistortionField,    ///< RG8 1 x 1 zero (the SDK's default distortion field)
    OfInput,            ///< R8_UINT: per dispatch, the current frame's luma at the dispatch's level
    OfInputLevel,       ///< rw_optical_flow_input_level_N: current luma level N (mip = N)
    OfPreviousInput,    ///< previous frame's luma at the dispatch's level
    OfFlow,             ///< rw_optical_flow (per dispatch)
    OfFlowSrv,          ///< OF r_optical_flow (per dispatch)
    OfFlowPrevious,     ///< r_optical_flow_previous (per dispatch)
    OfFlowNextLevel,    ///< rw_optical_flow_next_level (per dispatch)
    OfScdHistogram,     ///< R32_UINT 2304 x 1
    OfScdPreviousHistogram, ///< R32F 2304 x 1
    OfScdTemp,          ///< R32_UINT 3 x 1
    UiInput,            ///< caller's premultiplied UI (fgCompositeUi)
    CompositeSource,    ///< per dispatch: Output or CurrentSource (fgCompositeSource)
    CompositeTarget,    ///< per dispatch: PresentInterpolated or PresentReal (fgCompositeOut)
    ConstantsFi,
    ConstantsInpaintingPyramid,
    ConstantsOf,
    ConstantsOfSpd,
    SamplerLinear,
    SamplerPoint,
    Count,
};

enum class FgBindingKind : u8 { Sampled = 0, Storage, Uniform, Sampler, StorageBuffer };

struct FgBindingTarget {
    FgResource resource = FgResource::None;
    FgBindingKind kind = FgBindingKind::Sampled;
    u32 mip = 0;
};

/// The SDK's name tables (frame interpolation: srv / uav / cb; optical flow: srv / uav / cb — the two effects use
/// "r_optical_flow" for different resources) plus the samplers and the FUSE passes' names. False for an unknown name.
bool fg_resolve_binding(const char* name, bool opticalFlowPass, FgBindingTarget& out);

// ---- Physical slots (the plan's resource ids; fg_gpu.cpp creates the images) ------------------------------------

inline constexpr u32 kFgOfLevels = 7u; ///< OpticalFlowMaxPyramidLevels / advancedAlgorithmIterations
enum FgSlot : u16 {
    kSlotInputDepth = 0,
    kSlotInputMotion,
    kSlotDilatedDepth,
    kSlotDilatedMotion,
    kSlotReconPrevDepth,
    kSlotReconInterpDepth,
    kSlotGameMvX,
    kSlotGameMvY,
    kSlotOfMvX,
    kSlotOfMvY,
    kSlotDisocclusion,
    kSlotInpaintingPyramid,
    kSlotPreviousSource,
    kSlotDistortion,
    kSlotOutput,
    kSlotPresentInterpolated,
    kSlotPresentReal,
    kSlotOfInput1,                          ///< 7 consecutive levels
    kSlotOfInput2 = kSlotOfInput1 + kFgOfLevels,
    kSlotOfFlow1 = kSlotOfInput2 + kFgOfLevels,
    kSlotOfFlow2 = kSlotOfFlow1 + kFgOfLevels,
    kSlotOfScdHistogram = kSlotOfFlow2 + kFgOfLevels,
    kSlotOfScdPreviousHistogram,
    kSlotOfScdTemp,
    kSlotOfVector,
    kSlotOfScdOutput,
    kFgImageCount,                          ///< internal images
    kSlotCurrentSource = kFgImageCount,     ///< caller's images / buffers
    kSlotUi,
    kSlotCounters,                          ///< internal buffer
    kSlotSourceDepth,
    kSlotSourceMotion,
    kSlotNone,
};

// ---- Constant ring ------------------------------------------------------------------------------------------------

/// Uniform sub-allocations of one frame-in-flight slot (256-byte aligned ranges).
enum class FgCb : u8 { Fi = 0, IpRender, IpDisplay, OfSpd, OfBase, OfLevel0 };
inline constexpr u32 kFgCbAlign = 256u;
inline constexpr u32 kFgCbSlotBytes = (static_cast<u32>(FgCb::OfLevel0) + kFgOfLevels) * kFgCbAlign; ///< 3072
inline constexpr u32 fg_cb_offset(FgCb cb, u32 level = 0u) {
    return (static_cast<u32>(cb) + (cb == FgCb::OfLevel0 ? level : 0u)) * kFgCbAlign;
}

// ---- Dispatch plan ------------------------------------------------------------------------------------------------

/// One compute dispatch of the frame: the pass, its group counts, the per-dispatch aliases and constant ranges.
struct FgDispatch {
    FgPass pass = FgPass::Convert;
    u8 ofLevel = 0;
    FgCb cbOf = FgCb::OfBase;        ///< cbOF range
    FgCb cbIp = FgCb::IpDisplay;     ///< cbInpaintingPyramid range
    u32 groups[3] = {1u, 1u, 1u};
    u16 ofInput = kSlotNone;         ///< OfInput / r_optical_flow_input (and rw_optical_flow_input)
    u16 ofInputLevelBase = kSlotNone; ///< OfInputLevel N -> ofInputLevelBase + N
    u16 ofPreviousInput = kSlotNone;
    u16 ofFlow = kSlotNone;
    u16 ofFlowSrv = kSlotNone;
    u16 ofFlowPrevious = kSlotNone;
    u16 ofFlowNextLevel = kSlotNone;
    u16 compositeSource = kSlotNone;
    u16 compositeTarget = kSlotNone;
};
inline constexpr u32 kFgMaxDispatches = 48u;

// ---- Host state machine -----------------------------------------------------------------------------------------

struct FgSizes {
    u32 displayW = 0, displayH = 0;       ///< interpolation / OF resolution
    u32 maxRenderW = 0, maxRenderH = 0;
    u32 ofW[kFgOfLevels] = {}, ofH[kFgOfLevels] = {};             ///< optical-flow vector texture per level (8x8 blocks)
    u32 ofInputW[kFgOfLevels] = {}, ofInputH[kFgOfLevels] = {};   ///< luma pyramid (w >> level, >= 1)
    u32 pyramidW = 0, pyramidH = 0, pyramidMips = 0;              ///< inpainting pyramid (display / 2, full chain)
};
/// Resource sizes of opticalflowCreate / frameinterpolationCreate for the display size and the maximum render size.
bool fg_compute_sizes(u32 displayW, u32 displayH, u32 maxRenderW, u32 maxRenderH, FgSizes& out);

/// One frame's host inputs (the parts of the prepare / dispatch descriptions the passes consume).
struct FgFrameParams {
    u32 renderW = 0, renderH = 0;      ///< <= the maximum render size
    math::Vec2 jitter_px{};            ///< FUSE sample jitter
    f32 near_plane = 0.1f;
    f32 far_plane = 1000.f;
    f32 vertical_fov_rad = 1.f;
    f32 view_space_to_meters = 1.f;
    f32 frame_time_ms = 16.667f;       ///< frameTimeDelta (the SDK passes milliseconds here)
    bool reset = false;                ///< camera cut / teleport
    u64 frame_id = 0;                  ///< must increase by one per frame (a jump resets, like the SDK)
    bool compose_ui = false;           ///< a UI texture is bound
};

/// Persistent host state (the SDK contexts' constants / counters / resourceFrameIndex).
struct FgHostState {
    FgSizes sizes{};
    bool ofFirstExecution = true;
    i32 ofFrameIndex = 0;
    u32 ofResourceFrameIndex = 0; ///< mod 16 (FFX_OPTICALFLOW_MAX_QUEUED_FRAMES)
    u64 fiDispatchCount = 0;
    u64 previousFrameId = 0;
    u32 debugBarIndex = 0;
    bool portableSearch = true;   ///< plan FgPass::OfSearchPortable instead of the vendored wave32 / wave64 search
};

/// Everything one frame needs, derived from the state + params.
struct FgFrameSetup {
    FgFiConstants fi{};
    FgInpaintingPyramidConstants ipRender{};   ///< game vector field inpainting pyramid (render rect)
    FgInpaintingPyramidConstants ipDisplay{};  ///< inpainting pyramid (display rect)
    FgOfSpdConstants ofSpd{};
    FgOfConstants ofBase{};                    ///< prepare luma / SCD passes
    FgOfConstants ofLevel[kFgOfLevels]{};      ///< search / filter / scale per level
    bool fiReset = false;          ///< cbFI.reset: no preparation passes, the output is the current frame
    bool ofReset = false;          ///< optical flow reset (clears, frameIndex 0)
    bool ofOddFrame = false;       ///< optical flow ping-pong parity
    FgDispatch dispatches[kFgMaxDispatches]{};
    u32 dispatchCount = 0;
};

/// Resets `state` to fresh contexts (opticalflowCreate / frameinterpolationCreate) for the given sizes.
bool fg_init_state(FgHostState& state, u32 displayW, u32 displayH, u32 maxRenderW, u32 maxRenderH, bool portableSearch);
/// ffxFrameInterpolationPrepare + ffxOpticalflowContextDispatch + ffxFrameInterpolationDispatch for one frame:
/// constants, reset logic, ping-pong and the ordered dispatch plan (FUSE passes included). Advances `state`.
/// False (state untouched) on invalid input.
bool fg_setup_frame(FgHostState& state, const FgFrameParams& params, FgFrameSetup& out);

} // namespace fuse::renderer::framegen
