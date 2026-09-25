// WP-4.2 FSR 3.1: clean-room host port (constant setup, binding names, conventions). See fsr3_types.hpp.
// Reproduces sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp of FidelityFX SDK v1.1.4 (MIT) expression by
// expression where it computes numbers; the FFX host helpers it calls (FsrRcasCon, ffxSpdSetup) are the vendored
// headers compiled in their FFX_CPU mode.
#include <fuse/renderer/upscale_backends/fsr3/fsr3_types.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

// Vendored FidelityFX host-side helpers (CPU mode of the portable FFX headers; SYSTEM include).
#define FFX_CPU 1
#include <FidelityFX/gpu/ffx_core.h>
#include <FidelityFX/gpu/fsr1/ffx_fsr1.h>
#include <FidelityFX/gpu/spd/ffx_spd.h>

namespace fuse::renderer::fsr3 {

namespace {
constexpr u32 kMaxQueuedFrames = 16u; ///< FSR3UPSCALER_MAX_QUEUED_FRAMES (even)
constexpr i32 kTile = 8;              ///< threadGroupWorkRegionDim
constexpr i32 kTileRcas = 16;         ///< threadGroupWorkRegionDimRCAS

struct NameEntry {
    const char* name;
    Fsr3Resource resource;
    Fsr3BindingKind kind;
    u32 mip;
};

// srvTextureBindingTable / uavTextureBindingTable / constantBufferBindingTable of ffx_fsr3upscaler.cpp, with the
// per-frame aliasing of fsr3upscalerDispatch folded in (FARTHEST_DEPTH and LUMA_INSTABILITY are the
// INTERMEDIATE_FP16x1 surface, RCAS_INPUT is this frame's upscaled-colour UAV image, the SPD mip UAVs are views
// of SPD_MIPS), plus the static samplers of the Vulkan callbacks and the FUSE convert pass.
constexpr NameEntry kNames[] = {
    {"r_input_color_jittered", Fsr3Resource::InputColor, Fsr3BindingKind::Sampled, 0},
    {"r_input_motion_vectors", Fsr3Resource::InputMotion, Fsr3BindingKind::Sampled, 0},
    {"r_input_depth", Fsr3Resource::InputDepth, Fsr3BindingKind::Sampled, 0},
    {"r_input_exposure", Fsr3Resource::InputExposure, Fsr3BindingKind::Sampled, 0},
    {"r_frame_info", Fsr3Resource::FrameInfo, Fsr3BindingKind::Sampled, 0},
    {"r_reactive_mask", Fsr3Resource::InputReactive, Fsr3BindingKind::Sampled, 0},
    {"r_transparency_and_composition_mask", Fsr3Resource::InputTransparency, Fsr3BindingKind::Sampled, 0},
    {"r_reconstructed_previous_nearest_depth", Fsr3Resource::ReconstructedPrevNearestDepth, Fsr3BindingKind::Sampled, 0},
    {"r_dilated_motion_vectors", Fsr3Resource::DilatedMotion, Fsr3BindingKind::Sampled, 0},
    {"r_dilated_depth", Fsr3Resource::DilatedDepth, Fsr3BindingKind::Sampled, 0},
    {"r_internal_upscaled_color", Fsr3Resource::InternalUpscaled, Fsr3BindingKind::Sampled, 0},
    {"r_accumulation", Fsr3Resource::Accumulation, Fsr3BindingKind::Sampled, 0},
    {"r_luma_history", Fsr3Resource::LumaHistory, Fsr3BindingKind::Sampled, 0},
    {"r_rcas_input", Fsr3Resource::RcasInput, Fsr3BindingKind::Sampled, 0},
    {"r_lanczos_lut", Fsr3Resource::LanczosLut, Fsr3BindingKind::Sampled, 0},
    {"r_spd_mips", Fsr3Resource::SpdMips, Fsr3BindingKind::Sampled, 0},
    {"r_dilated_reactive_masks", Fsr3Resource::DilatedReactiveMasks, Fsr3BindingKind::Sampled, 0},
    {"r_new_locks", Fsr3Resource::NewLocks, Fsr3BindingKind::Sampled, 0},
    {"r_farthest_depth", Fsr3Resource::Intermediate, Fsr3BindingKind::Sampled, 0},
    {"r_farthest_depth_mip1", Fsr3Resource::FarthestDepthMip1, Fsr3BindingKind::Sampled, 0},
    {"r_shading_change", Fsr3Resource::ShadingChange, Fsr3BindingKind::Sampled, 0},
    {"r_current_luma", Fsr3Resource::CurrentLuma, Fsr3BindingKind::Sampled, 0},
    {"r_previous_luma", Fsr3Resource::PreviousLuma, Fsr3BindingKind::Sampled, 0},
    {"r_luma_instability", Fsr3Resource::Intermediate, Fsr3BindingKind::Sampled, 0},
    {"rw_reconstructed_previous_nearest_depth", Fsr3Resource::ReconstructedPrevNearestDepth, Fsr3BindingKind::Storage, 0},
    {"rw_dilated_motion_vectors", Fsr3Resource::DilatedMotion, Fsr3BindingKind::Storage, 0},
    {"rw_dilated_depth", Fsr3Resource::DilatedDepth, Fsr3BindingKind::Storage, 0},
    {"rw_internal_upscaled_color", Fsr3Resource::InternalUpscaled, Fsr3BindingKind::Storage, 0},
    {"rw_accumulation", Fsr3Resource::Accumulation, Fsr3BindingKind::Storage, 0},
    {"rw_luma_history", Fsr3Resource::LumaHistory, Fsr3BindingKind::Storage, 0},
    {"rw_upscaled_output", Fsr3Resource::UpscaledOutput, Fsr3BindingKind::Storage, 0},
    {"rw_dilated_reactive_masks", Fsr3Resource::DilatedReactiveMasks, Fsr3BindingKind::Storage, 0},
    {"rw_frame_info", Fsr3Resource::FrameInfo, Fsr3BindingKind::Storage, 0},
    {"rw_spd_global_atomic", Fsr3Resource::SpdAtomic, Fsr3BindingKind::Storage, 0},
    {"rw_new_locks", Fsr3Resource::NewLocks, Fsr3BindingKind::Storage, 0},
    {"rw_shading_change", Fsr3Resource::ShadingChange, Fsr3BindingKind::Storage, 0},
    {"rw_farthest_depth", Fsr3Resource::Intermediate, Fsr3BindingKind::Storage, 0},
    {"rw_farthest_depth_mip1", Fsr3Resource::FarthestDepthMip1, Fsr3BindingKind::Storage, 0},
    {"rw_current_luma", Fsr3Resource::CurrentLuma, Fsr3BindingKind::Storage, 0},
    {"rw_luma_instability", Fsr3Resource::Intermediate, Fsr3BindingKind::Storage, 0},
    {"rw_spd_mip0", Fsr3Resource::SpdMips, Fsr3BindingKind::Storage, 0},
    {"rw_spd_mip1", Fsr3Resource::SpdMips, Fsr3BindingKind::Storage, 1},
    {"rw_spd_mip2", Fsr3Resource::SpdMips, Fsr3BindingKind::Storage, 2},
    {"rw_spd_mip3", Fsr3Resource::SpdMips, Fsr3BindingKind::Storage, 3},
    {"rw_spd_mip4", Fsr3Resource::SpdMips, Fsr3BindingKind::Storage, 4},
    {"rw_spd_mip5", Fsr3Resource::SpdMips, Fsr3BindingKind::Storage, 5},
    {"cbFSR3Upscaler", Fsr3Resource::ConstantsMain, Fsr3BindingKind::Uniform, 0},
    {"cbSPD", Fsr3Resource::ConstantsSpd, Fsr3BindingKind::Uniform, 0},
    {"cbRCAS", Fsr3Resource::ConstantsRcas, Fsr3BindingKind::Uniform, 0},
    {"s_PointClamp", Fsr3Resource::SamplerPoint, Fsr3BindingKind::Sampler, 0},
    {"s_LinearClamp", Fsr3Resource::SamplerLinear, Fsr3BindingKind::Sampler, 0},
    // FUSE convert pass (fsr3_convert.{comp,slang}).
    {"fsr3SourceDepth", Fsr3Resource::SourceDepth, Fsr3BindingKind::StorageBuffer, 0},
    {"fsr3SourceMotion", Fsr3Resource::SourceMotion, Fsr3BindingKind::StorageBuffer, 0},
    {"fsr3OutDepth", Fsr3Resource::InputDepth, Fsr3BindingKind::Storage, 0},
    {"fsr3OutMotion", Fsr3Resource::InputMotion, Fsr3BindingKind::Storage, 0},
    {"fsr3OutExposure", Fsr3Resource::InputExposure, Fsr3BindingKind::Storage, 0},
};

// ffx_fsr3upscaler.cpp: halton()
f32 halton(i32 index, i32 base) {
    f32 f = 1.0f, result = 0.0f;
    for (i32 currentIndex = index; currentIndex > 0;) {
        f /= static_cast<f32>(base);
        result = result + f * static_cast<f32>(currentIndex % base);
        currentIndex = static_cast<i32>(static_cast<u32>(std::floor(static_cast<f32>(currentIndex) / static_cast<f32>(base))));
    }
    return result;
}

i32 groups(i32 items, i32 tile) { return (items + (tile - 1)) / tile; }
} // namespace

const char* fsr3_pass_name(Fsr3Pass pass) {
    switch (pass) {
    case Fsr3Pass::Convert: return "fsr3.convert";
    case Fsr3Pass::PrepareInputs: return "fsr3.prepare_inputs";
    case Fsr3Pass::LumaPyramid: return "fsr3.luma_pyramid";
    case Fsr3Pass::ShadingChangePyramid: return "fsr3.shading_change_pyramid";
    case Fsr3Pass::ShadingChange: return "fsr3.shading_change";
    case Fsr3Pass::PrepareReactivity: return "fsr3.prepare_reactivity";
    case Fsr3Pass::LumaInstability: return "fsr3.luma_instability";
    case Fsr3Pass::Accumulate: return "fsr3.accumulate";
    case Fsr3Pass::AccumulateSharpen: return "fsr3.accumulate_sharpen";
    case Fsr3Pass::Rcas: return "fsr3.rcas";
    case Fsr3Pass::Count: break;
    }
    return "fsr3.unknown";
}

bool fsr3_resolve_binding(const char* name, Fsr3BindingTarget& out) {
    if (name == nullptr) {
        return false;
    }
    for (const NameEntry& e : kNames) {
        if (std::strcmp(e.name, name) == 0) {
            out.resource = e.resource;
            out.kind = e.kind;
            out.mip = e.mip;
            return true;
        }
    }
    return false;
}

i32 fsr3_jitter_phase_count(i32 renderWidth, i32 displayWidth) {
    // ffxFsr3UpscalerGetJitterPhaseCount
    const f32 basePhaseCount = 8.0f;
    return static_cast<i32>(basePhaseCount * std::pow(static_cast<f32>(displayWidth) / static_cast<f32>(renderWidth), 2.0f));
}

math::Vec2 fsr3_sdk_jitter_offset(i32 index, i32 phaseCount) {
    // ffxFsr3UpscalerGetJitterOffset
    if (phaseCount <= 0) {
        return math::Vec2(0.f, 0.f);
    }
    const f32 x = halton((index % phaseCount) + 1, 2) - 0.5f;
    const f32 y = halton((index % phaseCount) + 1, 3) - 0.5f;
    return math::Vec2(x, y);
}

f32 fsr3_device_depth(f32 linear, f32 nearPlane, f32 farPlane) {
    if (!(linear > 0.f)) {
        return 0.f;
    }
    const f32 z = nearPlane * (farPlane - linear) / (linear * (farPlane - nearPlane));
    return std::clamp(z, 0.f, 1.f);
}

void fsr3_device_to_view_depth(f32 nearPlane, f32 farPlane, bool inverted, bool infinite, u32 renderWidth, u32 renderHeight,
                               f32 fovYRad, f32 out[4]) {
    // setupDeviceDepthToViewSpaceDepthParams: near / far order independent, the flags pick the transform.
    f32 fMin = std::min(nearPlane, farPlane);
    f32 fMax = std::max(nearPlane, farPlane);
    if (inverted) {
        const f32 tmp = fMin;
        fMin = fMax;
        fMax = tmp;
    }
    const f32 fQ = fMax / (fMin - fMax);
    const f32 d = -1.0f;
    const f32 matrixElemC[2][2] = {{fQ, -1.0f - FLT_EPSILON}, {fQ, 0.0f + FLT_EPSILON}};
    const f32 matrixElemE[2][2] = {{fQ * fMin, -fMin - FLT_EPSILON}, {fQ * fMin, fMax}};
    out[0] = d * matrixElemC[inverted ? 1 : 0][infinite ? 1 : 0];
    out[1] = matrixElemE[inverted ? 1 : 0][infinite ? 1 : 0];
    const f32 aspect = static_cast<f32>(renderWidth) / static_cast<f32>(renderHeight);
    const f32 cotHalfFovY = std::cos(0.5f * fovYRad) / std::sin(0.5f * fovYRad);
    const f32 a = cotHalfFovY / aspect;
    const f32 b = cotHalfFovY;
    out[2] = (1.0f / a);
    out[3] = (1.0f / b);
}

void fsr3_init_state(Fsr3HostState& state, const Fsr3Settings& settings, u32 maxRenderW, u32 maxRenderH, u32 maxDisplayW,
                     u32 maxDisplayH) {
    // fsr3upscalerCreate: memset, firstExecution, resourceFrameIndex 0, max sizes and the tunable defaults.
    state = Fsr3HostState{};
    state.firstExecution = true;
    state.resourceFrameIndex = 0;
    state.constants.maxUpscaleSize[0] = static_cast<i32>(maxDisplayW);
    state.constants.maxUpscaleSize[1] = static_cast<i32>(maxDisplayH);
    state.constants.maxRenderSize[0] = static_cast<i32>(maxRenderW);
    state.constants.maxRenderSize[1] = static_cast<i32>(maxRenderH);
    state.constants.velocityFactor = settings.velocity_factor;
    state.constants.reactivenessScale = settings.reactiveness_scale;
    state.constants.shadingChangeScale = settings.shading_change_scale;
    state.constants.accumulationAddedPerFrame = settings.accumulation_added_per_frame;
    state.constants.minDisocclusionAccumulation = settings.min_disocclusion_accumulation;
}

bool fsr3_setup_frame(Fsr3HostState& state, const Fsr3Settings& settings, const Fsr3FrameParams& params, Fsr3FrameSetup& out) {
    const UpscaleResolution& r = params.resolution;
    Fsr3UpscalerConstants& c = state.constants;
    if (!r.valid() || static_cast<i32>(r.render_width) > c.maxRenderSize[0] || static_cast<i32>(r.render_height) > c.maxRenderSize[1] ||
        static_cast<i32>(r.display_width) > c.maxUpscaleSize[0] || static_cast<i32>(r.display_height) > c.maxUpscaleSize[1] ||
        !std::isfinite(params.jitter_px.x) || !std::isfinite(params.jitter_px.y) || !(params.near_plane > 0.f) ||
        !(params.far_plane > params.near_plane) || !(params.vertical_fov_rad > 0.f) || !std::isfinite(params.exposure)) {
        return false;
    }
    out = Fsr3FrameSetup{};
    // Tunables (ffxFsr3UpscalerSetConstant) apply from this dispatch on.
    c.velocityFactor = settings.velocity_factor;
    c.reactivenessScale = settings.reactiveness_scale;
    c.shadingChangeScale = settings.shading_change_scale;
    c.accumulationAddedPerFrame = settings.accumulation_added_per_frame;
    c.minDisocclusionAccumulation = settings.min_disocclusion_accumulation;

    const bool isOddFrame = (state.resourceFrameIndex & 1u) != 0u;
    const bool resetAccumulation = params.reset || state.firstExecution;
    out.firstExecution = state.firstExecution;
    out.resetAccumulation = resetAccumulation;
    out.oddFrame = isOddFrame;
    state.firstExecution = false;

    const math::Vec2 jitter = fsr3_jitter_offset(params.jitter_px);
    c.previousFrameJitterOffset[0] = c.jitterOffset[0];
    c.previousFrameJitterOffset[1] = c.jitterOffset[1];
    c.jitterOffset[0] = jitter.x;
    c.jitterOffset[1] = jitter.y;

    c.previousFrameRenderSize[0] = c.renderSize[0];
    c.previousFrameRenderSize[1] = c.renderSize[1];
    c.renderSize[0] = static_cast<i32>(r.render_width);
    c.renderSize[1] = static_cast<i32>(r.render_height);

    // Horizontal FOV for the shader from the vertical one.
    const f32 aspectRatio = static_cast<f32>(r.render_width) / static_cast<f32>(r.render_height);
    const f32 cameraAngleHorizontal = std::atan(std::tan(params.vertical_fov_rad / 2.f) * aspectRatio) * 2.f;
    c.tanHalfFOV = std::tan(cameraAngleHorizontal * 0.5f);
    c.viewSpaceToMetersFactor = (settings.view_space_to_meters > 0.0f) ? settings.view_space_to_meters : 1.0f;

    // FUSE feeds reverse-Z, finite far plane (fsr3_device_depth): the "inverted, non-infinite" transform.
    fsr3_device_to_view_depth(params.near_plane, params.far_plane, true, false, r.render_width, r.render_height,
                              params.vertical_fov_rad, c.deviceToViewDepth);

    c.previousFrameUpscaleSize[0] = c.upscaleSize[0];
    c.previousFrameUpscaleSize[1] = c.upscaleSize[1];
    c.upscaleSize[0] = static_cast<i32>(r.display_width);
    c.upscaleSize[1] = static_cast<i32>(r.display_height);

    c.downscaleFactor[0] = static_cast<f32>(c.renderSize[0]) / static_cast<f32>(c.upscaleSize[0]);
    c.downscaleFactor[1] = static_cast<f32>(c.renderSize[1]) / static_cast<f32>(c.upscaleSize[1]);

    // Pre-exposure.
    c.deltaPreExposure = 1.0f;
    state.previousFramePreExposure = state.preExposure;
    state.preExposure = (params.pre_exposure != 0.0f) ? params.pre_exposure : 1.0f;
    if (state.previousFramePreExposure > 0.0f) {
        c.deltaPreExposure = state.preExposure / state.previousFramePreExposure;
    }

    // Render-resolution motion vectors: SDK motionVectorScale / renderSize with motionVectorScale = -renderSize
    // (FUSE UV motion is current - previous; FSR wants previous - current).
    c.motionVectorScale[0] = -static_cast<f32>(c.renderSize[0]) / static_cast<f32>(c.renderSize[0]);
    c.motionVectorScale[1] = -static_cast<f32>(c.renderSize[1]) / static_cast<f32>(c.renderSize[1]);
    // FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION is off (FUSE motion is unjittered).
    c.motionVectorJitterCancellation[0] = 0.f;
    c.motionVectorJitterCancellation[1] = 0.f;

    // Lock data: the jitter sequence length, moved by at most one per frame.
    const i32 jitterPhaseCount = fsr3_jitter_phase_count(static_cast<i32>(r.render_width), c.upscaleSize[0]);
    if (resetAccumulation || c.jitterPhaseCount == 0.f) {
        c.jitterPhaseCount = static_cast<f32>(jitterPhaseCount);
    } else {
        const i32 jitterPhaseCountDelta = static_cast<i32>(static_cast<f32>(jitterPhaseCount) - c.jitterPhaseCount);
        if (jitterPhaseCountDelta > 0) {
            c.jitterPhaseCount++;
        } else if (jitterPhaseCountDelta < 0) {
            c.jitterPhaseCount--;
        }
    }

    // Delta time in seconds, clamped to [0, 1].
    c.deltaTime = std::max(0.0f, std::min(1.0f, params.frame_time_s));
    if (resetAccumulation) {
        c.frameIndex = 0.0f;
    } else {
        c.frameIndex += 1.0f;
    }

    const i32 dispatchSrcX = groups(c.renderSize[0], kTile);
    const i32 dispatchSrcY = groups(c.renderSize[1], kTile);
    const i32 dispatchDstX = groups(c.upscaleSize[0], kTile);
    const i32 dispatchDstY = groups(c.upscaleSize[1], kTile);
    const i32 dispatchShadingChangePassX = groups(static_cast<i32>(static_cast<f32>(c.renderSize[0]) * 0.5f), kTile);
    const i32 dispatchShadingChangePassY = groups(static_cast<i32>(static_cast<f32>(c.renderSize[1]) * 0.5f), kTile);

    // SPD setup for the luma / shading-change pyramids (vendored ffxSpdSetup).
    FfxUInt32x2 dispatchThreadGroupCountXY = {0u, 0u};
    FfxUInt32x2 workGroupOffset = {0u, 0u};
    FfxUInt32x2 numWorkGroupsAndMips = {0u, 0u};
    FfxUInt32x4 rectInfo = {0u, 0u, r.render_width, r.render_height};
    ffxSpdSetup(dispatchThreadGroupCountXY, workGroupOffset, numWorkGroupsAndMips, rectInfo);
    out.spd.numWorkGroups = numWorkGroupsAndMips[0];
    out.spd.mips = numWorkGroupsAndMips[1];
    out.spd.workGroupOffset[0] = workGroupOffset[0];
    out.spd.workGroupOffset[1] = workGroupOffset[1];
    out.spd.renderSize[0] = r.render_width;
    out.spd.renderSize[1] = r.render_height;

    // RCAS (vendored FsrRcasCon).
    const f32 sharpenessRemapped = (-2.0f * params.sharpness) + 2.0f;
    FfxUInt32x4 rcasConfig = {0u, 0u, 0u, 0u};
    FsrRcasCon(rcasConfig, sharpenessRemapped);
    for (u32 i = 0; i < 4u; ++i) {
        out.rcas.rcasConfig[i] = rcasConfig[i];
    }

    auto set = [&out](Fsr3Pass pass, i32 x, i32 y) {
        out.groups[static_cast<u32>(pass)][0] = static_cast<u32>(std::max(x, 0));
        out.groups[static_cast<u32>(pass)][1] = static_cast<u32>(std::max(y, 0));
    };
    set(Fsr3Pass::Convert, dispatchSrcX, dispatchSrcY);
    set(Fsr3Pass::PrepareInputs, dispatchSrcX, dispatchSrcY);
    set(Fsr3Pass::LumaPyramid, static_cast<i32>(dispatchThreadGroupCountXY[0]), static_cast<i32>(dispatchThreadGroupCountXY[1]));
    set(Fsr3Pass::ShadingChangePyramid, static_cast<i32>(dispatchThreadGroupCountXY[0]),
        static_cast<i32>(dispatchThreadGroupCountXY[1]));
    set(Fsr3Pass::ShadingChange, dispatchShadingChangePassX, dispatchShadingChangePassY);
    set(Fsr3Pass::PrepareReactivity, dispatchSrcX, dispatchSrcY);
    set(Fsr3Pass::LumaInstability, dispatchSrcX, dispatchSrcY);
    set(Fsr3Pass::Accumulate, dispatchDstX, dispatchDstY);
    set(Fsr3Pass::AccumulateSharpen, dispatchDstX, dispatchDstY);
    set(Fsr3Pass::Rcas, groups(c.upscaleSize[0], kTileRcas), groups(c.upscaleSize[1], kTileRcas));

    out.constants = c;
    state.resourceFrameIndex = (state.resourceFrameIndex + 1u) % kMaxQueuedFrames;
    return true;
}

} // namespace fuse::renderer::fsr3
