#include <fuse/renderer/nvidia/nv_input_mapping.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::nvidia {

namespace {

constexpr u32 bit(FuseNvBufferKind k) noexcept { return 1u << k; }

constexpr u32 kSrRequired =
    bit(FUSE_NV_BUFFER_COLOR_IN) | bit(FUSE_NV_BUFFER_COLOR_OUT) | bit(FUSE_NV_BUFFER_DEPTH) | bit(FUSE_NV_BUFFER_MOTION_VECTORS);
constexpr u32 kRrGuides = bit(FUSE_NV_BUFFER_DIFFUSE_ALBEDO) | bit(FUSE_NV_BUFFER_SPECULAR_ALBEDO) | bit(FUSE_NV_BUFFER_NORMALS) |
                          bit(FUSE_NV_BUFFER_ROUGHNESS) | bit(FUSE_NV_BUFFER_SPECULAR_HIT_DISTANCE);
constexpr u32 kFgRequired = bit(FUSE_NV_BUFFER_DEPTH) | bit(FUSE_NV_BUFFER_MOTION_VECTORS) | bit(FUSE_NV_BUFFER_HUDLESS_COLOR);
constexpr u32 kNrRequired = bit(FUSE_NV_BUFFER_COLOR_IN) | bit(FUSE_NV_BUFFER_MOTION_VECTORS);

// Buffers that must be at render resolution for SR / RR.
constexpr u32 kRenderResBuffers = bit(FUSE_NV_BUFFER_DEPTH) | bit(FUSE_NV_BUFFER_MOTION_VECTORS) |
                                  bit(FUSE_NV_BUFFER_REACTIVE_MASK) | bit(FUSE_NV_BUFFER_TRANSPARENCY_MASK) | kRrGuides;

// Streamline expects the per-frame jitter in render pixels inside [-0.5, 0.5].
constexpr float kMaxJitterPx = 0.5f + 1e-3f;

InputStatus missing_status(FuseNvBufferKind k) noexcept {
    switch (k) {
        case FUSE_NV_BUFFER_COLOR_IN: return InputStatus::MissingColorInput;
        case FUSE_NV_BUFFER_COLOR_OUT: return InputStatus::MissingColorOutput;
        case FUSE_NV_BUFFER_DEPTH: return InputStatus::MissingDepth;
        case FUSE_NV_BUFFER_MOTION_VECTORS: return InputStatus::MissingMotionVectors;
        case FUSE_NV_BUFFER_DIFFUSE_ALBEDO: return InputStatus::MissingDiffuseAlbedo;
        case FUSE_NV_BUFFER_SPECULAR_ALBEDO: return InputStatus::MissingSpecularAlbedo;
        case FUSE_NV_BUFFER_NORMALS: return InputStatus::MissingNormals;
        case FUSE_NV_BUFFER_ROUGHNESS: return InputStatus::MissingRoughness;
        case FUSE_NV_BUFFER_SPECULAR_HIT_DISTANCE: return InputStatus::MissingSpecularHitDistance;
        case FUSE_NV_BUFFER_HUDLESS_COLOR: return InputStatus::MissingHudlessColor;
        default: return InputStatus::UnknownFeature;
    }
}

bool finite_all(const float* v, usize n) noexcept {
    return std::all_of(v, v + n, [](float x) { return std::isfinite(x); });
}

bool all_zero(const float* v, usize n) noexcept {
    return std::all_of(v, v + n, [](float x) { return x == 0.0f; });
}

// Used extent of a tagged resource; {0,0} when the host did not describe it (then not checked).
void used_extent(const FuseNvResourceTag& t, u32& w, u32& h) noexcept {
    w = t.extent_w ? t.extent_w : t.resource.width;
    h = t.extent_w ? t.extent_h : t.resource.height;
}

} // namespace

std::string_view input_status_name(InputStatus s) noexcept {
    switch (s) {
        case InputStatus::Ok: return "Ok";
        case InputStatus::MissingColorInput: return "MissingColorInput";
        case InputStatus::MissingColorOutput: return "MissingColorOutput";
        case InputStatus::MissingDepth: return "MissingDepth";
        case InputStatus::MissingMotionVectors: return "MissingMotionVectors";
        case InputStatus::MissingDiffuseAlbedo: return "MissingDiffuseAlbedo";
        case InputStatus::MissingSpecularAlbedo: return "MissingSpecularAlbedo";
        case InputStatus::MissingNormals: return "MissingNormals";
        case InputStatus::MissingRoughness: return "MissingRoughness";
        case InputStatus::MissingSpecularHitDistance: return "MissingSpecularHitDistance";
        case InputStatus::MissingHudlessColor: return "MissingHudlessColor";
        case InputStatus::ZeroRenderSize: return "ZeroRenderSize";
        case InputStatus::ZeroOutputSize: return "ZeroOutputSize";
        case InputStatus::OutputSmallerThanRender: return "OutputSmallerThanRender";
        case InputStatus::RenderOutputSizeMismatch: return "RenderOutputSizeMismatch";
        case InputStatus::ResourceExtentMismatch: return "ResourceExtentMismatch";
        case InputStatus::InvalidMotionVectorScale: return "InvalidMotionVectorScale";
        case InputStatus::JitterOutOfRange: return "JitterOutOfRange";
        case InputStatus::MissingProjection: return "MissingProjection";
        case InputStatus::NonFiniteConstants: return "NonFiniteConstants";
        case InputStatus::InvalidExposure: return "InvalidExposure";
        case InputStatus::HdrRequired: return "HdrRequired";
        case InputStatus::UnknownFeature: return "UnknownFeature";
        case InputStatus::WarnMissingUiColorAlpha: return "WarnMissingUiColorAlpha";
        case InputStatus::WarnMissingExposure: return "WarnMissingExposure";
    }
    return "?";
}

FuseNvStatus to_plugin_status(InputStatus s) noexcept {
    switch (s) {
        case InputStatus::Ok:
        case InputStatus::WarnMissingUiColorAlpha:
        case InputStatus::WarnMissingExposure: return FUSE_NV_OK;
        case InputStatus::MissingColorInput:
        case InputStatus::MissingColorOutput:
        case InputStatus::MissingDepth:
        case InputStatus::MissingMotionVectors:
        case InputStatus::MissingDiffuseAlbedo:
        case InputStatus::MissingSpecularAlbedo:
        case InputStatus::MissingNormals:
        case InputStatus::MissingRoughness:
        case InputStatus::MissingSpecularHitDistance:
        case InputStatus::MissingHudlessColor: return FUSE_NV_ERR_MISSING_INPUT;
        case InputStatus::UnknownFeature: return FUSE_NV_ERR_UNSUPPORTED_FEATURE;
        case InputStatus::ZeroRenderSize:
        case InputStatus::ZeroOutputSize:
        case InputStatus::OutputSmallerThanRender:
        case InputStatus::RenderOutputSizeMismatch:
        case InputStatus::ResourceExtentMismatch: return FUSE_NV_ERR_INVALID_ARGUMENT;
        case InputStatus::InvalidMotionVectorScale:
        case InputStatus::JitterOutOfRange:
        case InputStatus::MissingProjection:
        case InputStatus::NonFiniteConstants:
        case InputStatus::InvalidExposure:
        case InputStatus::HdrRequired: return FUSE_NV_ERR_INVALID_CONSTANTS;
    }
    return FUSE_NV_ERR_INVALID_ARGUMENT;
}

u32 required_buffer_mask(FuseNvFeature feature) noexcept {
    switch (feature) {
        case FUSE_NV_FEATURE_DLSS_SR: return kSrRequired;
        case FUSE_NV_FEATURE_DLSS_RR: return kSrRequired | kRrGuides;
        case FUSE_NV_FEATURE_DLSS_FG: return kFgRequired;
        case FUSE_NV_FEATURE_DLSS_NR: return kNrRequired;
        default: return 0u;
    }
}

u32 recommended_buffer_mask(FuseNvFeature feature) noexcept {
    switch (feature) {
        case FUSE_NV_FEATURE_DLSS_SR:
        case FUSE_NV_FEATURE_DLSS_RR: return bit(FUSE_NV_BUFFER_EXPOSURE);
        case FUSE_NV_FEATURE_DLSS_FG: return bit(FUSE_NV_BUFFER_UI_COLOR_ALPHA);
        case FUSE_NV_FEATURE_DLSS_NR: return bit(FUSE_NV_BUFFER_NEURAL_CONTROL_MASK);
        default: return 0u;
    }
}

std::string_view buffer_kind_name(FuseNvBufferKind kind) noexcept {
    switch (kind) {
        case FUSE_NV_BUFFER_COLOR_IN: return "color_in";
        case FUSE_NV_BUFFER_COLOR_OUT: return "color_out";
        case FUSE_NV_BUFFER_DEPTH: return "depth";
        case FUSE_NV_BUFFER_MOTION_VECTORS: return "motion_vectors";
        case FUSE_NV_BUFFER_EXPOSURE: return "exposure";
        case FUSE_NV_BUFFER_REACTIVE_MASK: return "reactive_mask";
        case FUSE_NV_BUFFER_TRANSPARENCY_MASK: return "transparency_mask";
        case FUSE_NV_BUFFER_HUDLESS_COLOR: return "hudless_color";
        case FUSE_NV_BUFFER_UI_COLOR_ALPHA: return "ui_color_alpha";
        case FUSE_NV_BUFFER_DIFFUSE_ALBEDO: return "diffuse_albedo";
        case FUSE_NV_BUFFER_SPECULAR_ALBEDO: return "specular_albedo";
        case FUSE_NV_BUFFER_NORMALS: return "normals";
        case FUSE_NV_BUFFER_ROUGHNESS: return "roughness";
        case FUSE_NV_BUFFER_SPECULAR_HIT_DISTANCE: return "specular_hit_distance";
        case FUSE_NV_BUFFER_NEURAL_CONTROL_MASK: return "neural_control_mask";
        default: return "unknown";
    }
}

NvFrameInputs::NvFrameInputs() {
    for (u32 k = 0; k < FUSE_NV_BUFFER_KIND_COUNT; ++k) {
        tags[k].kind = k;
    }
    constants.struct_size = sizeof(FuseNvConstants);
    constants.mvec_scale[0] = 1.0f;
    constants.mvec_scale[1] = 1.0f;
    constants.pre_exposure = 1.0f;
    constants.exposure_scale = 1.0f;
}

void NvFrameInputs::set(FuseNvBufferKind kind, const FuseNvResource& resource, FuseNvLifecycle lifecycle) {
    if (kind >= FUSE_NV_BUFFER_KIND_COUNT) {
        return;
    }
    FuseNvResourceTag& t = tags[kind];
    t = FuseNvResourceTag{};
    t.kind = kind;
    t.lifecycle = lifecycle;
    t.resource = resource;
}

void NvFrameInputs::clear(FuseNvBufferKind kind) {
    if (kind < FUSE_NV_BUFFER_KIND_COUNT) {
        tags[kind] = FuseNvResourceTag{};
        tags[kind].kind = kind;
    }
}

bool NvFrameInputs::has(FuseNvBufferKind kind) const noexcept {
    return kind < FUSE_NV_BUFFER_KIND_COUNT && tags[kind].resource.native != 0u;
}

u32 NvFrameInputs::present_mask() const noexcept {
    u32 m = 0;
    for (u32 k = 0; k < FUSE_NV_BUFFER_KIND_COUNT; ++k) {
        m |= has(k) ? bit(k) : 0u;
    }
    return m;
}

std::vector<FuseNvResourceTag> NvFrameInputs::packed() const {
    std::vector<FuseNvResourceTag> out;
    for (u32 k = 0; k < FUSE_NV_BUFFER_KIND_COUNT; ++k) {
        if (has(k)) {
            out.push_back(tags[k]);
        }
    }
    return out;
}

bool ValidationReport::has(InputStatus s) const noexcept {
    auto match = [s](const InputIssue& i) { return i.status == s; };
    return std::any_of(errors.begin(), errors.end(), match) || std::any_of(warnings.begin(), warnings.end(), match);
}

ValidationReport validate_inputs(FuseNvFeature feature, const NvFrameInputs& in) {
    ValidationReport r;
    auto err = [&r](InputStatus s, FuseNvBufferKind k = FUSE_NV_BUFFER_KIND_COUNT) { r.errors.push_back({s, k}); };
    auto warn = [&r](InputStatus s, FuseNvBufferKind k = FUSE_NV_BUFFER_KIND_COUNT) { r.warnings.push_back({s, k}); };

    const u32 required = required_buffer_mask(feature);
    if (required == 0u) {
        err(InputStatus::UnknownFeature);
        return r;
    }
    const FuseNvConstants& c = in.constants;

    // 1. Required resources, in kind order.
    for (u32 k = 0; k < FUSE_NV_BUFFER_KIND_COUNT; ++k) {
        if ((required & bit(k)) && !in.has(k)) {
            err(missing_status(k), k);
        }
    }

    // 2. Sizes.
    const bool upscaler = feature == FUSE_NV_FEATURE_DLSS_SR || feature == FUSE_NV_FEATURE_DLSS_RR;
    if (c.render_width == 0u || c.render_height == 0u) {
        err(InputStatus::ZeroRenderSize);
    }
    if (upscaler || feature == FUSE_NV_FEATURE_DLSS_NR) {
        if (c.output_width == 0u || c.output_height == 0u) {
            err(InputStatus::ZeroOutputSize);
        }
    }
    if (upscaler && c.output_width && c.output_height &&
        (c.output_width < c.render_width || c.output_height < c.render_height)) {
        err(InputStatus::OutputSmallerThanRender);
    }
    if (feature == FUSE_NV_FEATURE_DLSS_NR && (c.output_width != c.render_width || c.output_height != c.render_height)) {
        err(InputStatus::RenderOutputSizeMismatch);
    }
    auto check_extent = [&](FuseNvBufferKind k, u32 ew, u32 eh) {
        if (!in.has(k)) {
            return;
        }
        u32 w = 0, h = 0;
        used_extent(in.tags[k], w, h);
        if (w != 0u && h != 0u && ew != 0u && eh != 0u && (w != ew || h != eh)) {
            err(InputStatus::ResourceExtentMismatch, k);
        }
    };
    if (upscaler) {
        check_extent(FUSE_NV_BUFFER_COLOR_IN, c.render_width, c.render_height);
        check_extent(FUSE_NV_BUFFER_COLOR_OUT, c.output_width, c.output_height);
        for (u32 k = 0; k < FUSE_NV_BUFFER_KIND_COUNT; ++k) {
            if (kRenderResBuffers & bit(k)) {
                check_extent(k, c.render_width, c.render_height);
            }
        }
    } else if (feature == FUSE_NV_FEATURE_DLSS_FG) {
        check_extent(FUSE_NV_BUFFER_DEPTH, c.render_width, c.render_height);
        check_extent(FUSE_NV_BUFFER_MOTION_VECTORS, c.render_width, c.render_height);
        check_extent(FUSE_NV_BUFFER_HUDLESS_COLOR, c.output_width, c.output_height);
    } else if (feature == FUSE_NV_FEATURE_DLSS_NR) {
        check_extent(FUSE_NV_BUFFER_COLOR_IN, c.render_width, c.render_height);
        check_extent(FUSE_NV_BUFFER_COLOR_OUT, c.render_width, c.render_height);
        // NR runs after the upscaler at display resolution; its MVs may stay at render resolution
        // (they are UV-normalised through mvec_scale), so their extent is not checked.
    }

    // 3. Constants.
    const bool finite = finite_all(c.camera_view_to_clip, 16) && finite_all(c.clip_to_camera_view, 16) &&
                        finite_all(c.clip_to_prev_clip, 16) && finite_all(c.prev_clip_to_clip, 16) &&
                        finite_all(c.world_to_camera_view, 16) && finite_all(c.camera_view_to_world, 16) &&
                        finite_all(c.camera_pos, 3) && finite_all(c.camera_up, 3) && finite_all(c.camera_right, 3) &&
                        finite_all(c.camera_fwd, 3) && std::isfinite(c.camera_near) && std::isfinite(c.camera_far) &&
                        std::isfinite(c.camera_vfov_rad) && std::isfinite(c.camera_aspect) && std::isfinite(c.pre_exposure);
    if (!finite) {
        err(InputStatus::NonFiniteConstants);
    }
    if (!std::isfinite(c.mvec_scale[0]) || !std::isfinite(c.mvec_scale[1]) || c.mvec_scale[0] == 0.0f || c.mvec_scale[1] == 0.0f) {
        err(InputStatus::InvalidMotionVectorScale);
    }
    if (feature != FUSE_NV_FEATURE_DLSS_NR &&
        (!std::isfinite(c.jitter_offset_px[0]) || !std::isfinite(c.jitter_offset_px[1]) ||
         std::fabs(c.jitter_offset_px[0]) > kMaxJitterPx || std::fabs(c.jitter_offset_px[1]) > kMaxJitterPx)) {
        err(InputStatus::JitterOutOfRange);
    }
    if ((feature == FUSE_NV_FEATURE_DLSS_RR || feature == FUSE_NV_FEATURE_DLSS_FG) && all_zero(c.camera_view_to_clip, 16)) {
        err(InputStatus::MissingProjection);
    }
    if (feature == FUSE_NV_FEATURE_DLSS_RR && !(c.flags & FUSE_NV_CONST_HDR)) {
        err(InputStatus::HdrRequired);
    }
    if (upscaler && !in.has(FUSE_NV_BUFFER_EXPOSURE)) {
        if (!std::isfinite(c.exposure_scale) || c.exposure_scale <= 0.0f) {
            err(InputStatus::InvalidExposure, FUSE_NV_BUFFER_EXPOSURE);
        } else {
            warn(InputStatus::WarnMissingExposure, FUSE_NV_BUFFER_EXPOSURE);
        }
    }
    if (feature == FUSE_NV_FEATURE_DLSS_FG && !in.has(FUSE_NV_BUFFER_UI_COLOR_ALPHA)) {
        warn(InputStatus::WarnMissingUiColorAlpha, FUSE_NV_BUFFER_UI_COLOR_ALPHA);
    }
    return r;
}

} // namespace fuse::renderer::nvidia
