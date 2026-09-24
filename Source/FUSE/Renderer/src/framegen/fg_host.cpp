// WP-4.4 FSR 3.1 frame generation: clean-room host port (constants, reset logic, ping-pong, dispatch plan, binding
// names). See fg_types.hpp. Reproduces sdk/src/components/opticalflow/ffx_opticalflow.cpp,
// sdk/src/components/frameinterpolation/ffx_frameinterpolation.cpp and ffx-api/src/ffx_provider_framegeneration.cpp of
// FidelityFX SDK v1.1.4 (MIT) expression by expression where they compute numbers; ffxSpdSetup is the vendored header
// compiled in its FFX_CPU mode.
#include <fuse/renderer/framegen/fg_types.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>

// Vendored FidelityFX host-side helper (CPU mode of the portable FFX headers; SYSTEM include).
#define FFX_CPU 1
#include <FidelityFX/gpu/ffx_core.h>
#include <FidelityFX/gpu/spd/ffx_spd.h>

namespace fuse::renderer::framegen {

namespace {

constexpr u32 kOfBlockSize = 8u;          ///< opticalFlowBlockSize (provider and OF dispatch)
constexpr u32 kHistogramsPerDim = 3u;     ///< HistogramsPerDim
constexpr u32 kHistogramShifts = 3u;      ///< HistogramShifts
constexpr u32 kOfMaxQueuedFrames = 16u;   ///< FFX_OPTICALFLOW_MAX_QUEUED_FRAMES
constexpr u32 kSpdLumaPyramidMips = 4u;   ///< ffxSpdSetup(..., 4) of the OF luminance pyramid

/// debugBarColorSequence (ffx_frameinterpolation.cpp).
constexpr f32 kDebugBarColors[7][3] = {{0.0f, 1.0f, 1.0f}, {1.0f, 0.42f, 0.0f}, {0.0f, 0.16f, 1.0f}, {0.74f, 1.0f, 0.0f},
                                       {0.68f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.1f}, {1.0f, 1.0f, 0.48f}};

struct NameEntry {
    const char* name;
    FgResource resource;
    FgBindingKind kind;
    u32 mip;
};

// srvResourceBindingTable / uavResourceBindingTable / cbResourceBindingTable of ffx_frameinterpolation.cpp, with the
// dispatch-time registrations folded in (r_present_backbuffer = the current source when no HUD-less colour is
// attached: FUSE feeds the HUD-less colour as the source and composites the UI itself; r_optical_flow / r_optical_flow_scd
// = the optical flow's shared outputs; the distortion field = the SDK's 1x1 default).
constexpr NameEntry kFiNames[] = {
    {"r_input_depth", FgResource::InputDepth, FgBindingKind::Sampled, 0},
    {"r_input_motion_vectors", FgResource::InputMotion, FgBindingKind::Sampled, 0},
    {"r_input_distortion_field", FgResource::DistortionField, FgBindingKind::Sampled, 0},
    {"r_dilated_depth", FgResource::DilatedDepth, FgBindingKind::Sampled, 0},
    {"r_dilated_motion_vectors", FgResource::DilatedMotion, FgBindingKind::Sampled, 0},
    {"r_reconstructed_depth_previous_frame", FgResource::ReconPrevDepth, FgBindingKind::Sampled, 0},
    {"r_reconstructed_depth_interpolated_frame", FgResource::ReconInterpDepth, FgBindingKind::Sampled, 0},
    {"r_previous_interpolation_source", FgResource::PreviousSource, FgBindingKind::Sampled, 0},
    {"r_current_interpolation_source", FgResource::CurrentSource, FgBindingKind::Sampled, 0},
    {"r_disocclusion_mask", FgResource::DisocclusionMask, FgBindingKind::Sampled, 0},
    {"r_game_motion_vector_field_x", FgResource::GameMvX, FgBindingKind::Sampled, 0},
    {"r_game_motion_vector_field_y", FgResource::GameMvY, FgBindingKind::Sampled, 0},
    {"r_optical_flow_motion_vector_field_x", FgResource::OfMvX, FgBindingKind::Sampled, 0},
    {"r_optical_flow_motion_vector_field_y", FgResource::OfMvY, FgBindingKind::Sampled, 0},
    {"r_optical_flow", FgResource::OpticalFlowVector, FgBindingKind::Sampled, 0},
    {"r_optical_flow_scd", FgResource::OpticalFlowScd, FgBindingKind::Sampled, 0},
    {"r_output", FgResource::Output, FgBindingKind::Sampled, 0},
    {"r_inpainting_pyramid", FgResource::InpaintingPyramid, FgBindingKind::Sampled, 0},
    {"r_present_backbuffer", FgResource::CurrentSource, FgBindingKind::Sampled, 0},
    {"r_counters", FgResource::Counters, FgBindingKind::StorageBuffer, 0},
    {"rw_dilated_depth", FgResource::DilatedDepth, FgBindingKind::Storage, 0},
    {"rw_dilated_motion_vectors", FgResource::DilatedMotion, FgBindingKind::Storage, 0},
    {"rw_reconstructed_depth_previous_frame", FgResource::ReconPrevDepth, FgBindingKind::Storage, 0},
    {"rw_reconstructed_depth_interpolated_frame", FgResource::ReconInterpDepth, FgBindingKind::Storage, 0},
    {"rw_output", FgResource::Output, FgBindingKind::Storage, 0},
    {"rw_disocclusion_mask", FgResource::DisocclusionMask, FgBindingKind::Storage, 0},
    {"rw_game_motion_vector_field_x", FgResource::GameMvX, FgBindingKind::Storage, 0},
    {"rw_game_motion_vector_field_y", FgResource::GameMvY, FgBindingKind::Storage, 0},
    {"rw_optical_flow_motion_vector_field_x", FgResource::OfMvX, FgBindingKind::Storage, 0},
    {"rw_optical_flow_motion_vector_field_y", FgResource::OfMvY, FgBindingKind::Storage, 0},
    {"rw_counters", FgResource::Counters, FgBindingKind::StorageBuffer, 0},
    {"rw_inpainting_pyramid0", FgResource::InpaintingPyramid, FgBindingKind::Storage, 0},
    {"rw_inpainting_pyramid1", FgResource::InpaintingPyramid, FgBindingKind::Storage, 1},
    {"rw_inpainting_pyramid2", FgResource::InpaintingPyramid, FgBindingKind::Storage, 2},
    {"rw_inpainting_pyramid3", FgResource::InpaintingPyramid, FgBindingKind::Storage, 3},
    {"rw_inpainting_pyramid4", FgResource::InpaintingPyramid, FgBindingKind::Storage, 4},
    {"rw_inpainting_pyramid5", FgResource::InpaintingPyramid, FgBindingKind::Storage, 5},
    {"rw_inpainting_pyramid6", FgResource::InpaintingPyramid, FgBindingKind::Storage, 6},
    {"rw_inpainting_pyramid7", FgResource::InpaintingPyramid, FgBindingKind::Storage, 7},
    {"rw_inpainting_pyramid8", FgResource::InpaintingPyramid, FgBindingKind::Storage, 8},
    {"rw_inpainting_pyramid9", FgResource::InpaintingPyramid, FgBindingKind::Storage, 9},
    {"rw_inpainting_pyramid10", FgResource::InpaintingPyramid, FgBindingKind::Storage, 10},
    {"rw_inpainting_pyramid11", FgResource::InpaintingPyramid, FgBindingKind::Storage, 11},
    {"rw_inpainting_pyramid12", FgResource::InpaintingPyramid, FgBindingKind::Storage, 12},
    {"cbFI", FgResource::ConstantsFi, FgBindingKind::Uniform, 0},
    {"cbInpaintingPyramid", FgResource::ConstantsInpaintingPyramid, FgBindingKind::Uniform, 0},
};

// srvBindingNames / uavBindingNames / cbBindingNames of ffx_opticalflow.cpp (bound per dispatch through the plan).
constexpr NameEntry kOfNames[] = {
    {"r_input_color", FgResource::CurrentSource, FgBindingKind::Sampled, 0},
    {"r_optical_flow_input", FgResource::OfInput, FgBindingKind::Sampled, 0},
    {"r_optical_flow_previous_input", FgResource::OfPreviousInput, FgBindingKind::Sampled, 0},
    {"r_optical_flow", FgResource::OfFlowSrv, FgBindingKind::Sampled, 0},
    {"r_optical_flow_previous", FgResource::OfFlowPrevious, FgBindingKind::Sampled, 0},
    {"rw_optical_flow_input", FgResource::OfInput, FgBindingKind::Storage, 0},
    {"rw_optical_flow_input_level_1", FgResource::OfInputLevel, FgBindingKind::Storage, 1},
    {"rw_optical_flow_input_level_2", FgResource::OfInputLevel, FgBindingKind::Storage, 2},
    {"rw_optical_flow_input_level_3", FgResource::OfInputLevel, FgBindingKind::Storage, 3},
    {"rw_optical_flow_input_level_4", FgResource::OfInputLevel, FgBindingKind::Storage, 4},
    {"rw_optical_flow_input_level_5", FgResource::OfInputLevel, FgBindingKind::Storage, 5},
    {"rw_optical_flow_input_level_6", FgResource::OfInputLevel, FgBindingKind::Storage, 6},
    {"rw_optical_flow", FgResource::OfFlow, FgBindingKind::Storage, 0},
    {"rw_optical_flow_next_level", FgResource::OfFlowNextLevel, FgBindingKind::Storage, 0},
    {"rw_optical_flow_scd_histogram", FgResource::OfScdHistogram, FgBindingKind::Storage, 0},
    {"rw_optical_flow_scd_previous_histogram", FgResource::OfScdPreviousHistogram, FgBindingKind::Storage, 0},
    {"rw_optical_flow_scd_temp", FgResource::OfScdTemp, FgBindingKind::Storage, 0},
    {"rw_optical_flow_scd_output", FgResource::OpticalFlowScd, FgBindingKind::Storage, 0},
    {"cbOF", FgResource::ConstantsOf, FgBindingKind::Uniform, 0},
    {"cbOF_SPD", FgResource::ConstantsOfSpd, FgBindingKind::Uniform, 0},
};

// Static samplers of the Vulkan callbacks and the FUSE passes' names.
constexpr NameEntry kCommonNames[] = {
    {"s_LinearClamp", FgResource::SamplerLinear, FgBindingKind::Sampler, 0},
    {"s_PointClamp", FgResource::SamplerPoint, FgBindingKind::Sampler, 0},
    {"fgSourceDepth", FgResource::SourceDepth, FgBindingKind::StorageBuffer, 0},
    {"fgSourceMotion", FgResource::SourceMotion, FgBindingKind::StorageBuffer, 0},
    {"fgOutDepth", FgResource::InputDepth, FgBindingKind::Storage, 0},
    {"fgOutMotion", FgResource::InputMotion, FgBindingKind::Storage, 0},
    {"fgCompositeSource", FgResource::CompositeSource, FgBindingKind::Sampled, 0},
    {"fgCompositeUi", FgResource::UiInput, FgBindingKind::Sampled, 0},
    {"fgCompositeOut", FgResource::CompositeTarget, FgBindingKind::Storage, 0},
};

template <usize N>
bool lookup(const NameEntry (&table)[N], const char* name, FgBindingTarget& out) {
    for (const NameEntry& e : table) {
        if (std::strcmp(e.name, name) == 0) {
            out = FgBindingTarget{e.resource, e.kind, e.mip};
            return true;
        }
    }
    return false;
}

u32 groupsOf(u32 n, u32 d) { return (n + d - 1u) / d; }

void spdSetup(u32 w, u32 h, i32 mips, u32 groups[2], u32 workGroupOffset[2], u32 numWorkGroupsAndMips[2]) {
    FfxUInt32x2 dispatch = {0u, 0u};
    FfxUInt32x2 offset = {0u, 0u};
    FfxUInt32x2 numAndMips = {0u, 0u};
    FfxUInt32x4 rect = {0u, 0u, w, h};
    ffxSpdSetup(dispatch, offset, numAndMips, rect, mips);
    groups[0] = dispatch[0];
    groups[1] = dispatch[1];
    workGroupOffset[0] = offset[0];
    workGroupOffset[1] = offset[1];
    numWorkGroupsAndMips[0] = numAndMips[0];
    numWorkGroupsAndMips[1] = numAndMips[1];
}

/// setupDeviceDepthToViewSpaceDepthParams (ffx_frameinterpolation.cpp) for inverted, non-infinite depth.
void deviceToViewDepth(f32 cameraNear, f32 cameraFar, f32 viewSpaceToMeters, u32 renderW, u32 renderH, f32 fovY, f32 out[4]) {
    f32 fMin = std::min(cameraNear, cameraFar);
    f32 fMax = std::max(cameraNear, cameraFar);
    // bInverted: swap
    const f32 tmp = fMin;
    fMin = fMax;
    fMax = tmp;
    const f32 fQ = fMax / (fMin - fMax);
    const f32 d = -1.0f;
    const f32 c = fQ;         // matrix_elem_c[reversed][non infinite]
    const f32 e = fQ * fMin;  // matrix_elem_e[reversed][non infinite]
    out[0] = d * c;
    out[1] = e * viewSpaceToMeters;
    const f32 aspect = static_cast<f32>(renderW) / static_cast<f32>(renderH);
    const f32 cotHalfFovY = std::cos(0.5f * fovY) / std::sin(0.5f * fovY);
    const f32 a = cotHalfFovY / aspect;
    const f32 b = cotHalfFovY;
    out[2] = 1.0f / a;
    out[3] = 1.0f / b;
}

} // namespace

const char* fg_pass_name(FgPass pass) {
    switch (pass) {
    case FgPass::Convert: return "fg.convert";
    case FgPass::OfPrepareLuma: return "fg.of.prepare_luma";
    case FgPass::OfLuminancePyramid: return "fg.of.luminance_pyramid";
    case FgPass::OfScdHistogram: return "fg.of.scd_histogram";
    case FgPass::OfScdDivergence: return "fg.of.scd_divergence";
    case FgPass::OfSearch: return "fg.of.search";
    case FgPass::OfSearchPortable: return "fg.of.search_portable";
    case FgPass::OfFilter: return "fg.of.filter";
    case FgPass::OfScale: return "fg.of.scale";
    case FgPass::FiReconstructAndDilate: return "fg.fi.reconstruct_and_dilate";
    case FgPass::FiSetup: return "fg.fi.setup";
    case FgPass::FiReconstructPrevDepth: return "fg.fi.reconstruct_previous_depth";
    case FgPass::FiGameMotionVectorField: return "fg.fi.game_motion_vector_field";
    case FgPass::FiGameVectorFieldInpaintingPyramid: return "fg.fi.game_vector_field_inpainting_pyramid";
    case FgPass::FiOpticalFlowVectorField: return "fg.fi.optical_flow_vector_field";
    case FgPass::FiDisocclusionMask: return "fg.fi.disocclusion_mask";
    case FgPass::FiInterpolation: return "fg.fi.interpolation";
    case FgPass::FiInpaintingPyramid: return "fg.fi.inpainting_pyramid";
    case FgPass::FiInpainting: return "fg.fi.inpainting";
    case FgPass::UiComposite: return "fg.ui_composite";
    default: return "fg.?";
    }
}

bool fg_pass_is_optical_flow(FgPass pass) { return pass >= FgPass::OfPrepareLuma && pass <= FgPass::OfScale; }

bool fg_pass_is_vendored(FgPass pass) {
    return pass != FgPass::Convert && pass != FgPass::OfSearchPortable && pass != FgPass::UiComposite && pass < FgPass::Count;
}

bool fg_resolve_binding(const char* name, bool opticalFlowPass, FgBindingTarget& out) {
    out = FgBindingTarget{};
    if (name == nullptr) {
        return false;
    }
    if (opticalFlowPass ? lookup(kOfNames, name, out) : lookup(kFiNames, name, out)) {
        return true;
    }
    return lookup(kCommonNames, name, out);
}

f32 fg_device_depth(f32 linear, f32 nearPlane, f32 farPlane) {
    // Same expression as fg_convert.{slang,comp} and fsr3::fsr3_device_depth (no contraction: `precise` on the GPU).
    if (!(linear > 0.f)) {
        return 0.f;
    }
    const f32 num = nearPlane * (farPlane - linear);
    const f32 den = linear * (farPlane - nearPlane);
    const f32 z = num / den;
    return std::clamp(z, 0.f, 1.f);
}

math::Vec4 fg_composite(const math::Vec4& source, const math::Vec4& ui, bool hasUi) {
    if (!hasUi) {
        return math::Vec4(source.x, source.y, source.z, 1.f);
    }
    const f32 k = 1.f - ui.w;
    // out = src * (1 - a) + ui (premultiplied). Written as separate multiply and add (no FMA on either side).
    const f32 r = source.x * k;
    const f32 g = source.y * k;
    const f32 b = source.z * k;
    return math::Vec4(r + ui.x, g + ui.y, b + ui.z, 1.f);
}

bool fg_compute_sizes(u32 displayW, u32 displayH, u32 maxRenderW, u32 maxRenderH, FgSizes& out) {
    out = FgSizes{};
    // The OF luma pyramid needs level 6 (w >> 6) to exist; the SDK has the same implicit lower bound.
    if (displayW < 64u || displayH < 64u || maxRenderW == 0u || maxRenderH == 0u || displayW > 16384u || displayH > 16384u ||
        maxRenderW > 16384u || maxRenderH > 16384u) {
        return false;
    }
    out.displayW = displayW;
    out.displayH = displayH;
    out.maxRenderW = maxRenderW;
    out.maxRenderH = maxRenderH;
    out.ofW[0] = groupsOf(displayW, kOfBlockSize); // GetOpticalFlowTextureSize
    out.ofH[0] = groupsOf(displayH, kOfBlockSize);
    for (u32 l = 1; l < kFgOfLevels; ++l) {
        out.ofW[l] = (out.ofW[l - 1u] + 1u) / 2u; // FFX_ALIGN_UP(w, 2) / 2
        out.ofH[l] = (out.ofH[l - 1u] + 1u) / 2u;
    }
    for (u32 l = 0; l < kFgOfLevels; ++l) {
        out.ofInputW[l] = displayW >> l;
        out.ofInputH[l] = displayH >> l;
    }
    out.pyramidW = displayW / 2u;
    out.pyramidH = displayH / 2u;
    u32 m = std::max(out.pyramidW, out.pyramidH);
    out.pyramidMips = 1u;
    while (m > 1u) {
        m >>= 1u;
        ++out.pyramidMips;
    }
    return true;
}

bool fg_init_state(FgHostState& state, u32 displayW, u32 displayH, u32 maxRenderW, u32 maxRenderH, bool portableSearch) {
    state = FgHostState{};
    state.portableSearch = portableSearch;
    return fg_compute_sizes(displayW, displayH, maxRenderW, maxRenderH, state.sizes);
}

bool fg_setup_frame(FgHostState& state, const FgFrameParams& p, FgFrameSetup& out) {
    const FgSizes& z = state.sizes;
    if (z.displayW == 0u || p.renderW == 0u || p.renderH == 0u || p.renderW > z.maxRenderW || p.renderH > z.maxRenderH ||
        !std::isfinite(p.near_plane) || !std::isfinite(p.far_plane) || !(p.near_plane > 0.f) || !(p.far_plane > p.near_plane) ||
        !std::isfinite(p.vertical_fov_rad) || !(p.vertical_fov_rad > 0.f) || !(p.vertical_fov_rad < 3.1f) ||
        !std::isfinite(p.frame_time_ms) || !std::isfinite(p.jitter_px.x) || !std::isfinite(p.jitter_px.y)) {
        return false;
    }
    out = FgFrameSetup{};
    const u32 W = z.displayW, H = z.displayH, rw = p.renderW, rh = p.renderH;

    // ---- Frame interpolation constants (frameinterpolationCreate defaults + Prepare + Dispatch) ----------------
    FgFiConstants& c = out.fi;
    c.maxRenderSize[0] = static_cast<i32>(z.maxRenderW);
    c.maxRenderSize[1] = static_cast<i32>(z.maxRenderH);
    // Prepare: render size, jitter (FSR sign), motion-vector scale (low-res MVs: scale / renderSize).
    c.renderSize[0] = static_cast<i32>(rw);
    c.renderSize[1] = static_cast<i32>(rh);
    c.jitter[0] = -p.jitter_px.x;
    c.jitter[1] = -p.jitter_px.y;
    const f32 mvScaleX = -static_cast<f32>(rw); // FUSE UV motion (cur - prev) -> SDK convention (prev - cur)
    const f32 mvScaleY = -static_cast<f32>(rh);
    c.motionVectorScale[0] = mvScaleX / static_cast<f32>(c.renderSize[0]);
    c.motionVectorScale[1] = mvScaleY / static_cast<f32>(c.renderSize[1]);
    // Dispatch.
    const bool bReset = state.fiDispatchCount == 0u || p.reset;
    const bool decreased = p.frame_id < state.previousFrameId;
    const bool skipped = (p.frame_id - state.previousFrameId) > 1u; // u64 wrap like the SDK
    const bool disjoint = decreased || skipped;
    state.previousFrameId = p.frame_id;
    ++state.fiDispatchCount;
    c.displaySize[0] = static_cast<i32>(W);
    c.displaySize[1] = static_cast<i32>(H);
    c.displaySizeRcp[0] = 1.0f / static_cast<f32>(W);
    c.displaySizeRcp[1] = 1.0f / static_cast<f32>(H);
    c.upscalerTargetSize[0] = static_cast<i32>(W); // interpolationRect = the whole back buffer
    c.upscalerTargetSize[1] = static_cast<i32>(H);
    c.mode = 0;
    c.reset = (bReset || disjoint) ? 1 : 0;
    c.deltaTime = p.frame_time_ms;
    c.hudLessAttachedFactor = 0; // the source IS the HUD-less colour; the UI never enters the interpolation
    c.opticalFlowScale[0] = 1.f / static_cast<f32>(W);
    c.opticalFlowScale[1] = 1.f / static_cast<f32>(H);
    c.opticalFlowBlockSize = static_cast<i32>(kOfBlockSize);
    c.dispatchFlags = 0u;
    c.cameraNear = p.near_plane;
    c.cameraFar = p.far_plane;
    c.interpolationRectBase[0] = 0;
    c.interpolationRectBase[1] = 0;
    c.interpolationRectSize[0] = static_cast<i32>(W);
    c.interpolationRectSize[1] = static_cast<i32>(H);
    std::memcpy(c.debugBarColor, kDebugBarColors[state.debugBarIndex], sizeof(c.debugBarColor));
    state.debugBarIndex = (state.debugBarIndex + 1u) % 7u;
    c.backBufferTransferFunction = 0u; // FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB
    c.minMaxLuminance[0] = 0.f;
    c.minMaxLuminance[1] = 0.f;
    const f32 aspectRatio = static_cast<f32>(rw) / static_cast<f32>(rh);
    const f32 cameraAngleHorizontal = std::atan(std::tan(p.vertical_fov_rad / 2.f) * aspectRatio) * 2.f;
    c.tanHalfFov = std::tan(cameraAngleHorizontal * 0.5f);
    c.distortionFieldSize[0] = 1;
    c.distortionFieldSize[1] = 1;
    const f32 meters = p.view_space_to_meters > 0.f ? p.view_space_to_meters : 1.f;
    deviceToViewDepth(p.near_plane, p.far_plane, meters, rw, rh, p.vertical_fov_rad, c.deviceToViewDepth);
    out.fiReset = c.reset != 0;

    // Inpainting pyramids (scheduleDispatchGameVectorFieldInpaintingPyramid: render rect; inpainting: display rect).
    u32 gRender[2], gDisplay[2], tmpOff[2], tmpNum[2];
    spdSetup(rw, rh, -1, gRender, tmpOff, tmpNum);
    out.ipRender.numWorkGroups = tmpNum[0];
    out.ipRender.mips = tmpNum[1];
    out.ipRender.workGroupOffset[0] = tmpOff[0];
    out.ipRender.workGroupOffset[1] = tmpOff[1];
    spdSetup(W, H, -1, gDisplay, tmpOff, tmpNum);
    out.ipDisplay.numWorkGroups = tmpNum[0];
    out.ipDisplay.mips = tmpNum[1];
    out.ipDisplay.workGroupOffset[0] = tmpOff[0];
    out.ipDisplay.workGroupOffset[1] = tmpOff[1];

    // ---- Optical flow (ffxOpticalflowContextDispatch, resolution = display size) --------------------------------
    const bool firstEver = state.ofFirstExecution;
    out.ofReset = p.reset || state.ofFirstExecution;
    state.ofFirstExecution = false;
    state.ofFrameIndex = out.ofReset ? 0 : state.ofFrameIndex + 1;
    FgOfConstants of{};
    of.inputLumaResolution[0] = static_cast<i32>(W);
    of.inputLumaResolution[1] = static_cast<i32>(H);
    of.frameIndex = state.ofFrameIndex;
    of.backbufferTransferFunction = 0u;
    of.minMaxLuminance[0] = 0.f;
    of.minMaxLuminance[1] = 0.f;
    // The SDK stages the constants for the prepare / SCD passes with the level fields the previous dispatch left
    // (0 and 7 after the level loop; 0 and 0 before the first dispatch).
    of.opticalFlowPyramidLevel = 0u;
    of.opticalFlowPyramidLevelCount = firstEver ? 0u : kFgOfLevels;
    out.ofBase = of;
    for (u32 l = 0; l < kFgOfLevels; ++l) {
        out.ofLevel[l] = of;
        out.ofLevel[l].opticalFlowPyramidLevel = l;
        out.ofLevel[l].opticalFlowPyramidLevelCount = kFgOfLevels;
    }
    u32 gPyramid[2];
    spdSetup(W, H, static_cast<i32>(kSpdLumaPyramidMips), gPyramid, tmpOff, tmpNum);
    out.ofSpd.numWorkGroups = tmpNum[0];
    out.ofSpd.mips = tmpNum[1];
    out.ofSpd.workGroupOffset[0] = tmpOff[0];
    out.ofSpd.workGroupOffset[1] = tmpOff[1];
    out.ofSpd.numWorkGroupsOpticalFlowInputPyramid = tmpNum[0];
    const bool odd = (state.ofResourceFrameIndex & 1u) != 0u;
    out.ofOddFrame = odd;
    state.ofResourceFrameIndex = (state.ofResourceFrameIndex + 1u) % kOfMaxQueuedFrames;
    const u16 inA = static_cast<u16>(odd ? kSlotOfInput2 : kSlotOfInput1);
    const u16 inB = static_cast<u16>(odd ? kSlotOfInput1 : kSlotOfInput2);

    // ---- Plan -----------------------------------------------------------------------------------------------------
    u32& n = out.dispatchCount;
    auto add = [&](FgPass pass, u32 gx, u32 gy, u32 gz = 1u) -> FgDispatch& {
        FgDispatch& d = out.dispatches[n++];
        d = FgDispatch{};
        d.pass = pass;
        d.groups[0] = std::max(gx, 1u);
        d.groups[1] = std::max(gy, 1u);
        d.groups[2] = std::max(gz, 1u);
        return d;
    };
    const u32 renderGx = groupsOf(rw, 8u), renderGy = groupsOf(rh, 8u);
    const u32 displayGx = groupsOf(W, 8u), displayGy = groupsOf(H, 8u);

    add(FgPass::Convert, renderGx, renderGy);
    // ffxFrameInterpolationPrepare
    add(FgPass::FiReconstructAndDilate, renderGx, renderGy);
    // ffxOpticalflowContextDispatch
    {
        FgDispatch& d = add(FgPass::OfPrepareLuma, groupsOf(groupsOf(W, 2u), 16u), groupsOf(groupsOf(H, 2u), 16u));
        d.ofInput = inA;
    }
    {
        FgDispatch& d = add(FgPass::OfLuminancePyramid, gPyramid[0], gPyramid[1]);
        d.ofInput = inA;
        d.ofInputLevelBase = inA;
    }
    {
        const u32 strataWidth = (W / 4u) / kHistogramsPerDim;
        FgDispatch& d = add(FgPass::OfScdHistogram, groupsOf(strataWidth, 32u), 16u, kHistogramsPerDim * kHistogramsPerDim);
        d.ofInput = inA;
    }
    add(FgPass::OfScdDivergence, kHistogramsPerDim * kHistogramsPerDim, kHistogramShifts);
    for (i32 level = static_cast<i32>(kFgOfLevels) - 1; level >= 0; --level) {
        const u32 l = static_cast<u32>(level);
        const bool oddLevel = (l & 1u) != 0u;
        const u16 flowA = static_cast<u16>(((odd != oddLevel) ? kSlotOfFlow2 : kSlotOfFlow1) + l);
        const u16 flowB = static_cast<u16>(((odd != oddLevel) ? kSlotOfFlow1 : kSlotOfFlow2) + l);
        const u32 inW = std::max(W >> l, 1u), inH = std::max(H >> l, 1u);
        {
            const u32 threadPixels = 4u, tgY = 16u, tgSize = 64u;
            FgDispatch& d = add(state.portableSearch ? FgPass::OfSearchPortable : FgPass::OfSearch,
                                (groupsOf(inW, threadPixels) * tgY + (tgSize - 1u)) / tgSize, groupsOf(inH, tgY));
            d.ofLevel = static_cast<u8>(l);
            d.cbOf = FgCb::OfLevel0;
            d.ofInput = static_cast<u16>(inA + l);
            d.ofPreviousInput = static_cast<u16>(inB + l);
            d.ofFlow = flowA;
            d.ofFlowPrevious = flowB;
        }
        {
            FgDispatch& d = add(FgPass::OfFilter, groupsOf(z.ofW[l], 16u), groupsOf(z.ofH[l], 4u));
            d.ofLevel = static_cast<u8>(l);
            d.cbOf = FgCb::OfLevel0;
            d.ofInput = static_cast<u16>(inA + l);
            d.ofPreviousInput = static_cast<u16>(inB + l);
            d.ofFlowPrevious = flowA;
            d.ofFlow = l == 0u ? static_cast<u16>(kSlotOfVector) : flowB;
        }
        if (l > 0u) {
            FgDispatch& d = add(FgPass::OfScale, groupsOf(z.ofW[l - 1u], 4u), groupsOf(z.ofH[l - 1u], 4u));
            d.ofLevel = static_cast<u8>(l);
            d.cbOf = FgCb::OfLevel0;
            d.ofInput = static_cast<u16>(inA + l);
            d.ofPreviousInput = static_cast<u16>(inB + l);
            d.ofFlowSrv = flowB;
            d.ofFlowNextLevel = static_cast<u16>(flowB - 1u);
        }
    }
    // ffxFrameInterpolationDispatch
    add(FgPass::FiSetup, renderGx, renderGy);
    if (!out.fiReset) {
        add(FgPass::FiReconstructPrevDepth, renderGx, renderGy);
        add(FgPass::FiGameMotionVectorField, renderGx, renderGy);
        FgDispatch& d = add(FgPass::FiGameVectorFieldInpaintingPyramid, gRender[0], gRender[1]);
        d.cbIp = FgCb::IpRender;
        const u32 ofGx = static_cast<u32>(static_cast<f32>(W) / static_cast<f32>(kOfBlockSize) + 7.f) / 8u;
        const u32 ofGy = static_cast<u32>(static_cast<f32>(H) / static_cast<f32>(kOfBlockSize) + 7.f) / 8u;
        add(FgPass::FiOpticalFlowVectorField, ofGx, ofGy);
        add(FgPass::FiDisocclusionMask, renderGx, renderGy);
    }
    add(FgPass::FiInterpolation, displayGx, displayGy);
    {
        FgDispatch& d = add(FgPass::FiInpaintingPyramid, gDisplay[0], gDisplay[1]);
        d.cbIp = FgCb::IpDisplay;
    }
    add(FgPass::FiInpainting, displayGx, displayGy);
    // FUSE: UI over the interpolated frame and over the real frame.
    {
        FgDispatch& d = add(FgPass::UiComposite, displayGx, displayGy);
        d.compositeSource = kSlotOutput;
        d.compositeTarget = kSlotPresentInterpolated;
    }
    {
        FgDispatch& d = add(FgPass::UiComposite, displayGx, displayGy);
        d.compositeSource = kSlotCurrentSource;
        d.compositeTarget = kSlotPresentReal;
    }
    return true;
}

void fg_of_search_cpu(const u8* current, const u8* previous, u32 w, u32 h, u32 level, bool usePrediction, const std::int16_t* prediction,
                      bool sceneChanged, std::int16_t* flow) {
    const u32 flowW = (w + 7u) / 8u, flowH = (h + 7u) / 8u;
    const i32 iw = static_cast<i32>(w), ih = static_cast<i32>(h);
    auto luma = [&](const u8* img, i32 x, i32 y) -> i32 {
        return img[static_cast<usize>(std::clamp(y, 0, ih - 1)) * w + static_cast<usize>(std::clamp(x, 0, iw - 1))];
    };
    auto sad = [&](i32 bx, i32 by, i32 ox, i32 oy) {
        u32 s = 0u;
        for (i32 y = 0; y < 8; ++y) {
            for (i32 x = 0; x < 8; ++x) {
                s += static_cast<u32>(std::abs(luma(current, bx + x, by + y) - luma(previous, bx + x + ox, by + y + oy)));
            }
        }
        return s;
    };
    for (u32 fy = 0; fy < flowH; ++fy) {
        for (u32 fx = 0; fx < flowW; ++fx) {
            std::int16_t* out = flow + (static_cast<usize>(fy) * flowW + fx) * 2u;
            if (sceneChanged) {
                out[0] = out[1] = 0;
                continue;
            }
            const std::int16_t* p = prediction + (static_cast<usize>(fy) * flowW + fx) * 2u;
            const i32 px = usePrediction ? p[0] : 0, py = usePrediction ? p[1] : 0;
            const i32 bx = static_cast<i32>(fx * 8u), by = static_cast<i32>(fy * 8u);
            u32 best = ~0u;
            for (i32 cy = 0; cy < 16; ++cy) {
                for (i32 cx = 0; cx < 16; ++cx) {
                    const u32 key = (sad(bx, by, px + cx - 8, py + cy - 8) << 16) | (static_cast<u32>(std::abs(cy - 8)) << 12) |
                                    (static_cast<u32>(std::abs(cx - 8)) << 8) | (static_cast<u32>(cy) << 4) | static_cast<u32>(cx);
                    best = std::min(best, key);
                }
            }
            i32 vx = px + static_cast<i32>(best & 0xfu) - 8, vy = py + static_cast<i32>((best >> 4) & 0xfu) - 8;
            if (level == 0u && sad(bx, by, 0, 0) <= (best >> 16)) {
                vx = vy = 0;
            }
            out[0] = static_cast<std::int16_t>(vx);
            out[1] = static_cast<std::int16_t>(vy);
        }
    }
}

} // namespace fuse::renderer::framegen
