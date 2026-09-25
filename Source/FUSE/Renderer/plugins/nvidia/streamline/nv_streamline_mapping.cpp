#include "nv_streamline_mapping.hpp"

// The vendored headers are pinned (Engine/lib/streamline/VERSION); fail the build if they drift.
static_assert(SL_VERSION_MAJOR == 2 && SL_VERSION_MINOR == 14 && SL_VERSION_PATCH == 1,
              "Engine/lib/streamline headers do not match the VERSION pin (2.14.1)");
// FUSE's ABI numbering is its own; these are the Streamline ids the mapping relies on.
static_assert(sl::kFeatureDLSS == 0 && sl::kFeatureDLSS_G == 1000 && sl::kFeatureDLSS_RR == 1001 &&
              sl::kFeatureDLSS_NR == 1004 && sl::kFeatureReflex == 3);

namespace fuse::renderer::nvidia::streamline {

namespace {

sl::Boolean flag(uint32_t flags, uint32_t bit) noexcept {
    return (flags & bit) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
}

void to_matrix(const float (&m)[16], sl::float4x4& out) noexcept {
    for (uint32_t r = 0; r < 4; ++r) {
        out.row[r] = sl::float4(m[r * 4 + 0], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
    }
}

} // namespace

bool to_sl_feature(FuseNvFeature feature, sl::Feature& out) noexcept {
    switch (feature) {
        case FUSE_NV_FEATURE_DLSS_SR: out = sl::kFeatureDLSS; return true;
        case FUSE_NV_FEATURE_DLSS_RR: out = sl::kFeatureDLSS_RR; return true;
        case FUSE_NV_FEATURE_DLSS_FG: out = sl::kFeatureDLSS_G; return true;
        case FUSE_NV_FEATURE_DLSS_NR: out = sl::kFeatureDLSS_NR; return true;
        case FUSE_NV_FEATURE_REFLEX: out = sl::kFeatureReflex; return true;
        default: return false;
    }
}

bool to_sl_buffer_type(FuseNvBufferKind kind, FuseNvFeature feature, sl::BufferType& out) noexcept {
    const bool nr = feature == FUSE_NV_FEATURE_DLSS_NR;
    switch (kind) {
        case FUSE_NV_BUFFER_COLOR_IN: out = nr ? sl::kBufferTypeUpliftInputColor : sl::kBufferTypeScalingInputColor; return true;
        case FUSE_NV_BUFFER_COLOR_OUT: out = nr ? sl::kBufferTypeUpliftOutputColor : sl::kBufferTypeScalingOutputColor; return true;
        case FUSE_NV_BUFFER_DEPTH: out = sl::kBufferTypeDepth; return true;
        case FUSE_NV_BUFFER_MOTION_VECTORS: out = sl::kBufferTypeMotionVectors; return true;
        case FUSE_NV_BUFFER_EXPOSURE: out = sl::kBufferTypeExposure; return true;
        // DLSS's "reactive" input is the bias-current-colour mask (kBufferTypeReactiveMaskHint is the
        // FSR-style / DirectSR tag and is ignored by sl.dlss).
        case FUSE_NV_BUFFER_REACTIVE_MASK: out = sl::kBufferTypeBiasCurrentColorHint; return true;
        case FUSE_NV_BUFFER_TRANSPARENCY_MASK: out = sl::kBufferTypeTransparencyHint; return true;
        case FUSE_NV_BUFFER_HUDLESS_COLOR: out = sl::kBufferTypeHUDLessColor; return true;
        case FUSE_NV_BUFFER_UI_COLOR_ALPHA: out = sl::kBufferTypeUIColorAndAlpha; return true;
        case FUSE_NV_BUFFER_DIFFUSE_ALBEDO: out = sl::kBufferTypeAlbedo; return true;
        case FUSE_NV_BUFFER_SPECULAR_ALBEDO: out = sl::kBufferTypeSpecularAlbedo; return true;
        case FUSE_NV_BUFFER_NORMALS: out = sl::kBufferTypeNormals; return true;
        case FUSE_NV_BUFFER_ROUGHNESS: out = sl::kBufferTypeRoughness; return true;
        case FUSE_NV_BUFFER_SPECULAR_HIT_DISTANCE: out = sl::kBufferTypeSpecularHitDistance; return true;
        case FUSE_NV_BUFFER_NEURAL_CONTROL_MASK:
            if (!nr) {
                return false;
            }
            out = sl::kBufferTypeUpliftControlMask;
            return true;
        default: return false;
    }
}

sl::ResourceLifecycle to_sl_lifecycle(FuseNvLifecycle lifecycle) noexcept {
    switch (lifecycle) {
        case FUSE_NV_LIFECYCLE_VALID_UNTIL_PRESENT: return sl::ResourceLifecycle::eValidUntilPresent;
        case FUSE_NV_LIFECYCLE_ONLY_VALID_NOW: return sl::ResourceLifecycle::eOnlyValidNow;
        default: return sl::ResourceLifecycle::eValidUntilEvaluate;
    }
}

sl::Resource to_sl_resource(const FuseNvResource& r) noexcept {
    sl::Resource out(sl::ResourceType::eTex2d, reinterpret_cast<void*>(static_cast<uintptr_t>(r.native)),
                     reinterpret_cast<void*>(static_cast<uintptr_t>(r.memory)),
                     reinterpret_cast<void*>(static_cast<uintptr_t>(r.view)), r.state);
    out.width = r.width;
    out.height = r.height;
    out.nativeFormat = r.native_format;
    out.mipLevels = 1;
    out.arrayLayers = 1;
    out.flags = 0;
    return out;
}

sl::Extent to_sl_extent(const FuseNvResourceTag& tag) noexcept {
    sl::Extent e{};
    if (tag.extent_w && tag.extent_h) {
        e.left = tag.extent_x;
        e.top = tag.extent_y;
        e.width = tag.extent_w;
        e.height = tag.extent_h;
    }
    return e;
}

void to_sl_constants(const FuseNvConstants& in, sl::Constants& out) noexcept {
    to_matrix(in.camera_view_to_clip, out.cameraViewToClip);
    to_matrix(in.clip_to_camera_view, out.clipToCameraView);
    // FUSE has no lens-distortion pass: clipToLensClip is identity.
    static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    to_matrix(kIdentity, out.clipToLensClip);
    to_matrix(in.clip_to_prev_clip, out.clipToPrevClip);
    to_matrix(in.prev_clip_to_clip, out.prevClipToClip);
    out.jitterOffset = sl::float2(in.jitter_offset_px[0], in.jitter_offset_px[1]);
    out.mvecScale = sl::float2(in.mvec_scale[0], in.mvec_scale[1]);
    out.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    out.cameraPos = sl::float3(in.camera_pos[0], in.camera_pos[1], in.camera_pos[2]);
    out.cameraUp = sl::float3(in.camera_up[0], in.camera_up[1], in.camera_up[2]);
    out.cameraRight = sl::float3(in.camera_right[0], in.camera_right[1], in.camera_right[2]);
    out.cameraFwd = sl::float3(in.camera_fwd[0], in.camera_fwd[1], in.camera_fwd[2]);
    out.cameraNear = in.camera_near;
    out.cameraFar = in.camera_far;
    out.cameraFOV = in.camera_vfov_rad;
    out.cameraAspectRatio = in.camera_aspect;
    out.motionVectorsInvalidValue = sl::INVALID_FLOAT;
    out.depthInverted = flag(in.flags, FUSE_NV_CONST_DEPTH_INVERTED);
    out.cameraMotionIncluded = flag(in.flags, FUSE_NV_CONST_CAMERA_MOTION_INCLUDED);
    out.motionVectors3D = flag(in.flags, FUSE_NV_CONST_MV_3D);
    out.reset = flag(in.flags, FUSE_NV_CONST_RESET);
    out.orthographicProjection = flag(in.flags, FUSE_NV_CONST_ORTHOGRAPHIC);
    out.motionVectorsDilated = flag(in.flags, FUSE_NV_CONST_MV_DILATED);
    out.motionVectorsJittered = flag(in.flags, FUSE_NV_CONST_MV_JITTERED);
}

sl::DLSSMode to_sl_dlss_mode(FuseNvQuality quality) noexcept {
    switch (quality) {
        case FUSE_NV_QUALITY_DLAA: return sl::DLSSMode::eDLAA;
        case FUSE_NV_QUALITY_QUALITY: return sl::DLSSMode::eMaxQuality;
        case FUSE_NV_QUALITY_BALANCED: return sl::DLSSMode::eBalanced;
        case FUSE_NV_QUALITY_PERFORMANCE: return sl::DLSSMode::eMaxPerformance;
        case FUSE_NV_QUALITY_ULTRA_PERFORMANCE: return sl::DLSSMode::eUltraPerformance;
        case FUSE_NV_QUALITY_ULTRA_QUALITY: return sl::DLSSMode::eUltraQuality;
        default: return sl::DLSSMode::eOff;
    }
}

FuseNvStatus from_sl_result(sl::Result result) noexcept {
    switch (result) {
        case sl::Result::eOk: return FUSE_NV_OK;
        case sl::Result::eErrorDriverOutOfDate: return FUSE_NV_ERR_DRIVER_TOO_OLD;
        case sl::Result::eErrorNoSupportedAdapterFound:
        case sl::Result::eErrorAdapterNotSupported: return FUSE_NV_ERR_NO_NVIDIA_GPU;
        case sl::Result::eErrorFeatureMissing:
        case sl::Result::eErrorFeatureNotSupported: return FUSE_NV_ERR_UNSUPPORTED_FEATURE;
        case sl::Result::eErrorMissingInputParameter: return FUSE_NV_ERR_MISSING_INPUT;
        case sl::Result::eErrorMissingConstants:
        case sl::Result::eErrorCommonConstantsMissing: return FUSE_NV_ERR_MISSING_CONSTANTS;
        case sl::Result::eErrorInvalidParameter: return FUSE_NV_ERR_INVALID_ARGUMENT;
        case sl::Result::eErrorNotInitialized:
        case sl::Result::eErrorInitNotCalled: return FUSE_NV_ERR_NOT_INITIALIZED;
        case sl::Result::eErrorNoPlugins:
        case sl::Result::eErrorFeatureFailedToLoad: return FUSE_NV_ERR_RUNTIME_MISSING;
        case sl::Result::eErrorVulkanAPI:
        case sl::Result::eErrorD3DAPI:
        case sl::Result::eErrorMissingOrInvalidAPI: return FUSE_NV_ERR_GRAPHICS_API;
        default: return FUSE_NV_ERR_RUNTIME_FAILURE;
    }
}

} // namespace fuse::renderer::nvidia::streamline
