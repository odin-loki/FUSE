#pragma once

// WP-4.2 FSR 3.1 temporal upscaler: host-side records, conventions and the clean-room port of the SDK's
// per-dispatch constant setup. Vulkan-free (builds and is gated in the stub tree).
//
// Upstream: AMD FidelityFX SDK v1.1.4 (tag v1.1.4, commit c6efa6bf7f2027b3ec94f28578bb5965eabb9e55, MIT),
// FSR 3.1 upscaler effect version 3.1.4. The GPU side is the vendored Vulkan GLSL passes
// (Engine/lib/fidelityfx/shaders/vk/fsr3upscaler/*.glsl + include/FidelityFX/gpu/{fsr3upscaler,spd}/*),
// compiled verbatim by cmake/rp_wp42.cmake. The host side (this file, fsr3_host.cpp, fsr3_gpu.cpp) replaces
// sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp + the FidelityFX Vulkan backend, which are NOT vendored:
// the constant setup below reproduces fsr3upscalerDispatch() / setupDeviceDepthToViewSpaceDepthParams()
// expression by expression; resources, clears, ping-pong and pass order follow fsr3upscalerCreate() /
// fsr3upscalerDispatch().
//
// Conventions (FUSE side = upscale/upscale_inputs.hpp, temporal/temporal_types.hpp):
//   jitter   FUSE `jitter_px` is the offset of every render SAMPLE from its pixel centre (render pixel (i, j)
//            samples the unjittered scene at (i + 0.5 + jx, j + 0.5 + jy), +y down). The FSR 3.1 shaders use the
//            opposite quantity: `Jitter()` is the displacement of the image content (upsample.h: "the
//            un-jittered position of the sample at offset 0,0" = iPx + 0.5 - Jitter(); luma_instability.h:
//            uvJittered = uv + Jitter() / RenderSize()). Hence FSR jitterOffset = -jitter_px
//            (fsr3_jitter_offset). The projection matrix for that sample convention moves every point by
//            -jitter_px: in Vulkan clip space (NDC y down) clip.xy -= 2 jitter / size * clip.w, i.e. the NDC
//            offset (-2 jx / w, -2 jy / h) = upscaleJitterNdc = IJitterProvider::offset_ndc (projection_jitter_ndc
//            is kept as an alias), identical to temporal::jitter_view_proj. (Until the WP-4.2 follow-up,
//            upscaleJitterNdc returned +2 jx / w and moved points by +jitter in x; it was fixed at the source.)
//            FSR's jitterOffset = -jitter_px is NOT a compensation for that: it is the SDK's own sign (content
//            displacement vs sample offset) and is independent of how the projection is jittered. The
//            fuse_rp_fsr3_jitter gate pins every provider / consumer against each other and
//            fuse_rp_fsr3_vk_quality shows the wrong FSR sign measurably blurs the output.
//   motion   FUSE motion = uv_cur - uv_prev (unjittered). FSR wants the UV offset to the previous position
//            (prev - cur) after `MotionVectorScale()`: the convert pass stores the FUSE UV motion unchanged
//            and the host sets fMotionVectorScale = (-1, -1) (= SDK motionVectorScale -renderSize / renderSize).
//   depth    FUSE depth is linear view depth (> 0, <= 0 = sky). The convert pass writes reverse-Z device depth
//            of a finite [near, far] perspective (fsr3_device_depth), sky = 0; the passes are compiled with
//            FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=1 and fDeviceToViewDepth maps it back exactly
//            (fsr3_device_to_view_depth, SDK "inverted, non-infinite" branch).
//   colour   linear HDR (FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=1); `exposure` is written into the 1x1 exposure
//            texture the passes read (colour * exposure ~ display-referred, FSR "exposure" semantics).

#include <fuse/math/vec.hpp>
#include <fuse/renderer/upscale/upscale_inputs.hpp>
#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::fsr3 {

// ---- Upstream pin ---------------------------------------------------------------------------------------
inline constexpr const char* kFidelityFxSdkVersion = "1.1.4";
inline constexpr const char* kFidelityFxSdkCommit = "c6efa6bf7f2027b3ec94f28578bb5965eabb9e55";
inline constexpr u32 kFsr3UpscalerVersionMajor = 3u;
inline constexpr u32 kFsr3UpscalerVersionMinor = 1u;
inline constexpr u32 kFsr3UpscalerVersionPatch = 4u;

// ---- GPU records (std140 uniform blocks of the vendored callbacks_glsl.h) ------------------------------------

/// cbFSR3Upscaler (Fsr3UpscalerConstants in ffx_fsr3upscaler_private.h). 148 bytes, std140-compatible.
struct Fsr3UpscalerConstants {
    i32 renderSize[2] = {0, 0};
    i32 previousFrameRenderSize[2] = {0, 0};
    i32 upscaleSize[2] = {0, 0};
    i32 previousFrameUpscaleSize[2] = {0, 0};
    i32 maxRenderSize[2] = {0, 0};
    i32 maxUpscaleSize[2] = {0, 0};
    f32 deviceToViewDepth[4] = {0.f, 0.f, 0.f, 0.f};
    f32 jitterOffset[2] = {0.f, 0.f};
    f32 previousFrameJitterOffset[2] = {0.f, 0.f};
    f32 motionVectorScale[2] = {0.f, 0.f};
    f32 downscaleFactor[2] = {0.f, 0.f};
    f32 motionVectorJitterCancellation[2] = {0.f, 0.f};
    f32 tanHalfFOV = 0.f;
    f32 jitterPhaseCount = 0.f;
    f32 deltaTime = 0.f;
    f32 deltaPreExposure = 0.f;
    f32 viewSpaceToMetersFactor = 0.f;
    f32 frameIndex = 0.f;
    f32 velocityFactor = 0.f;
    f32 reactivenessScale = 0.f;
    f32 shadingChangeScale = 0.f;
    f32 accumulationAddedPerFrame = 0.f;
    f32 minDisocclusionAccumulation = 0.f;
};
static_assert(sizeof(Fsr3UpscalerConstants) == 148u && offsetof(Fsr3UpscalerConstants, deviceToViewDepth) == 48u &&
                  offsetof(Fsr3UpscalerConstants, jitterOffset) == 64u &&
                  offsetof(Fsr3UpscalerConstants, motionVectorJitterCancellation) == 96u &&
                  offsetof(Fsr3UpscalerConstants, tanHalfFOV) == 104u && offsetof(Fsr3UpscalerConstants, deltaTime) == 112u &&
                  offsetof(Fsr3UpscalerConstants, velocityFactor) == 128u &&
                  offsetof(Fsr3UpscalerConstants, minDisocclusionAccumulation) == 144u,
              "Fsr3UpscalerConstants must match cbFSR3Upscaler (std140)");

/// cbSPD (Fsr3UpscalerSpdConstants). 24 bytes.
struct Fsr3SpdConstants {
    u32 mips = 0;
    u32 numWorkGroups = 0;
    u32 workGroupOffset[2] = {0u, 0u};
    u32 renderSize[2] = {0u, 0u};
};
static_assert(sizeof(Fsr3SpdConstants) == 24u && offsetof(Fsr3SpdConstants, renderSize) == 16u, "cbSPD layout");

/// cbRCAS (Fsr3UpscalerRcasConstants). 16 bytes.
struct Fsr3RcasConstants {
    u32 rcasConfig[4] = {0u, 0u, 0u, 0u};
};
static_assert(sizeof(Fsr3RcasConstants) == 16u, "cbRCAS layout");

/// Push constants of the FUSE-authored "fsr3.convert" pass (fsr3_convert.{comp,slang}). 32 bytes.
struct Fsr3ConvertPush {
    u32 renderW = 0;
    u32 renderH = 0;
    f32 nearPlane = 0.f;
    f32 farPlane = 0.f;
    f32 exposure = 1.f;
    u32 pad0 = 0;
    u32 pad1 = 0;
    u32 pad2 = 0;
};
static_assert(sizeof(Fsr3ConvertPush) == 32u && offsetof(Fsr3ConvertPush, exposure) == 16u, "Fsr3ConvertPush layout");

// ---- Passes ---------------------------------------------------------------------------------------------

/// Every compute pass of the backend, in dispatch order (SDK fsr3upscalerDispatch). Convert is FUSE's input
/// adapter (buffers -> the textures the SDK passes read); AccumulateSharpen replaces Accumulate when RCAS runs.
enum class Fsr3Pass : u8 {
    Convert = 0,
    PrepareInputs,
    LumaPyramid,
    ShadingChangePyramid,
    ShadingChange,
    PrepareReactivity,
    LumaInstability,
    Accumulate,
    AccumulateSharpen,
    Rcas,
    Count,
};
inline constexpr u32 kFsr3PassCount = static_cast<u32>(Fsr3Pass::Count);

/// Render-graph pass name ("fsr3.prepare_inputs", ...).
const char* fsr3_pass_name(Fsr3Pass pass);

// ---- Internal resources (SDK resource identifiers, collapsed to what the eight passes bind) -------------------

enum class Fsr3Resource : u8 {
    None = 0,
    InputColor,           ///< caller's jittered colour (r_input_color_jittered)
    InputDepth,           ///< converted reverse-Z device depth, R32F render
    InputMotion,          ///< converted UV motion, RG32F render
    InputExposure,        ///< 1x1 RG32F written by fsr3.convert
    InputReactive,        ///< caller's mask or the 1x1 default (0)
    InputTransparency,    ///< caller's mask or the 1x1 default (0)
    FrameInfo,            ///< 1x1 RGBA32F
    ReconstructedPrevNearestDepth, ///< R32_UINT render
    DilatedMotion,        ///< RG16F render
    DilatedDepth,         ///< R32F render
    Accumulation,         ///< R8 render, ping-pong (SRV = read, UAV = write)
    InternalUpscaled,     ///< RGBA16F display, ping-pong
    RcasInput,            ///< this frame's InternalUpscaled UAV image, read by RCAS
    LumaHistory,          ///< RGBA16F render, ping-pong
    CurrentLuma,          ///< R16F render (Luma1 / Luma2 by parity)
    PreviousLuma,
    Intermediate,         ///< R16F render: farthest depth, later luma instability (SDK INTERMEDIATE_FP16x1)
    FarthestDepthMip1,    ///< R16F render / 2
    ShadingChange,        ///< R8 render / 2
    SpdMips,              ///< RG16F render / 2, full chain (UAV: one view per mip 0..5)
    SpdAtomic,            ///< R32_UINT 1x1
    NewLocks,             ///< R8 display
    DilatedReactiveMasks, ///< RGBA8 render
    LanczosLut,           ///< R16_SNORM 128 x 1 (LUT reprojection permutation only; not built, rejected at init)
    UpscaledOutput,       ///< RGBA16F display (the backend's output image)
    ConstantsMain,        ///< cbFSR3Upscaler
    ConstantsSpd,         ///< cbSPD
    ConstantsRcas,        ///< cbRCAS
    SamplerPoint,         ///< s_PointClamp (immutable)
    SamplerLinear,        ///< s_LinearClamp (immutable)
    SourceDepth,          ///< caller's f32 linear depth buffer (fsr3.convert)
    SourceMotion,         ///< caller's f32x2 UV motion buffer (fsr3.convert)
    Count,
};

/// How a shader binding uses its resource.
enum class Fsr3BindingKind : u8 { Sampled = 0, Storage, Uniform, Sampler, StorageBuffer };

/// Resolved shader binding name -> resource. `mip` selects the storage view of SpdMips (rw_spd_mip0..5).
struct Fsr3BindingTarget {
    Fsr3Resource resource = Fsr3Resource::None;
    Fsr3BindingKind kind = Fsr3BindingKind::Sampled;
    u32 mip = 0;
};

/// The SDK's name tables (srvTextureBindingTable / uavTextureBindingTable / constantBufferBindingTable) plus
/// the samplers and the convert pass's names. False for an unknown name.
bool fsr3_resolve_binding(const char* name, Fsr3BindingTarget& out);

// ---- Settings (ffxFsr3UpscalerSetConstant keys, SDK defaults) -------------------------------------------------
struct Fsr3Settings {
    f32 velocity_factor = 1.f;
    f32 reactiveness_scale = 1.f;
    f32 shading_change_scale = 1.f;
    f32 accumulation_added_per_frame = 1.f / 3.f;
    f32 min_disocclusion_accumulation = -1.f / 3.f;
    f32 view_space_to_meters = 1.f; ///< FfxFsr3UpscalerDispatchDescription::viewSpaceToMetersFactor
};

// ---- Conventions ------------------------------------------------------------------------------------------

/// FSR jitterOffset (content displacement, SDK sign) for a FUSE sample jitter: -jitter_px.
inline math::Vec2 fsr3_jitter_offset(math::Vec2 fuseJitterPx) { return math::Vec2(-fuseJitterPx.x, -fuseJitterPx.y); }
/// NDC translation of a Vulkan projection (NDC y down) that realises the FUSE sample jitter:
/// (-2 jx / w, -2 jy / h) — upscaleJitterNdc (alias kept for WP-4.2 callers), same as temporal::jitter_view_proj.
inline math::Vec2 projection_jitter_ndc(math::Vec2 jitterPx, u32 renderWidth, u32 renderHeight) {
    return upscaleJitterNdc(jitterPx, renderWidth, renderHeight);
}
/// ffxFsr3UpscalerGetJitterPhaseCount: int(8 * (display / render)^2) (truncation; the renderer's
/// upscaleJitterPhaseCount rounds up, e.g. 23 vs 24 at 1.7x — only the lock / accumulation constant uses this).
i32 fsr3_jitter_phase_count(i32 renderWidth, i32 displayWidth);
/// ffxFsr3UpscalerGetJitterOffset: (Halton(i + 1, 2) - 0.5, Halton(i + 1, 3) - 0.5), i = index % phaseCount.
math::Vec2 fsr3_sdk_jitter_offset(i32 index, i32 phaseCount);

/// Reverse-Z device depth of linear view depth `linear` for a finite [near, far] perspective:
/// z = n (f - L) / (L (f - n)), clamped to [0, 1]; sky (L <= 0) -> 0 (the far plane).
f32 fsr3_device_depth(f32 linear, f32 nearPlane, f32 farPlane);
/// setupDeviceDepthToViewSpaceDepthParams (SDK), for the given depth mode and the render aspect.
void fsr3_device_to_view_depth(f32 nearPlane, f32 farPlane, bool inverted, bool infinite, u32 renderWidth, u32 renderHeight,
                               f32 fovYRad, f32 out[4]);
/// GetViewSpaceDepth (ffx_fsr3upscaler_common.h): d2v[1] / (z - d2v[0]).
inline f32 fsr3_view_depth(f32 deviceDepth, const f32 d2v[4]) { return d2v[1] / (deviceDepth - d2v[0]); }

// ---- Host state machine ---------------------------------------------------------------------------------

/// One frame's host inputs (the parts of FfxFsr3UpscalerDispatchDescription the passes consume).
struct Fsr3FrameParams {
    UpscaleResolution resolution{}; ///< render <= max render, display <= max display
    math::Vec2 jitter_px{};         ///< FUSE sample jitter (converted with fsr3_jitter_offset)
    f32 exposure = 1.f;             ///< written into the exposure texture
    f32 pre_exposure = 1.f;         ///< FfxFsr3UpscalerDispatchDescription::preExposure (0 -> 1)
    f32 frame_time_s = 1.f / 60.f;  ///< frameTimeDelta (SDK takes ms, clamps seconds to [0, 1])
    bool reset = false;
    f32 near_plane = 0.05f;
    f32 far_plane = 1000.f;
    f32 vertical_fov_rad = 1.f;
    bool sharpen = false;
    f32 sharpness = 0.f; ///< [0, 1], RCAS strength (SDK: sharpenessRemapped = -2 s + 2 stops)
};

/// Persistent host state (the SDK context's constants / firstExecution / resourceFrameIndex / pre-exposure).
struct Fsr3HostState {
    Fsr3UpscalerConstants constants{};
    bool firstExecution = true;
    u32 resourceFrameIndex = 0; ///< mod 16 (FSR3UPSCALER_MAX_QUEUED_FRAMES)
    f32 preExposure = 0.f;
    f32 previousFramePreExposure = 0.f;
};

/// Everything one dispatch needs, derived from the state + params.
struct Fsr3FrameSetup {
    Fsr3UpscalerConstants constants{};
    Fsr3SpdConstants spd{};
    Fsr3RcasConstants rcas{};
    bool firstExecution = false;    ///< clear accumulation / luma ping-pong images
    bool resetAccumulation = false; ///< reset || firstExecution
    bool oddFrame = false;          ///< ping-pong parity (resourceFrameIndex & 1)
    u32 groups[kFsr3PassCount][2] = {}; ///< dispatch size per pass
};

/// Resets `state` to a fresh context (fsr3upscalerCreate defaults) for the given maximum sizes.
void fsr3_init_state(Fsr3HostState& state, const Fsr3Settings& settings, u32 maxRenderW, u32 maxRenderH, u32 maxDisplayW,
                     u32 maxDisplayH);
/// fsr3upscalerDispatch's constant / dispatch-size computation for one frame; advances `state`
/// (jitter / size history, frame index, pre-exposure, resourceFrameIndex). False (state untouched) on invalid input.
bool fsr3_setup_frame(Fsr3HostState& state, const Fsr3Settings& settings, const Fsr3FrameParams& params, Fsr3FrameSetup& out);

} // namespace fuse::renderer::fsr3
