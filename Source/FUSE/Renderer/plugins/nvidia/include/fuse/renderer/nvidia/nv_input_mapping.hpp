#pragma once

// Host-side input contract for the NVIDIA features (docs/nvidia-plugin.md "Input mapping").
//
// NvFrameInputs is the provider-neutral form of one frame: one FuseNvResourceTag slot per
// FuseNvBufferKind plus FuseNvConstants. The upscaler adapter (nv_upscaler_backends.hpp) fills it
// from FUSE's UpscaleInputs; validate_inputs() then rejects a frame with a *specific* status before
// anything reaches the provider, so a missing G-buffer guide surfaces as "MissingNormals" rather
// than an opaque runtime error on an RTX machine.

#include <fuse/renderer/nvidia/fuse_nv_plugin_abi.h>
#include <fuse/types.hpp>

#include <array>
#include <string_view>
#include <vector>

namespace fuse::renderer::nvidia {

enum class InputStatus : u8 {
    Ok,
    // Required resources.
    MissingColorInput,
    MissingColorOutput,
    MissingDepth,
    MissingMotionVectors,
    MissingDiffuseAlbedo,
    MissingSpecularAlbedo,
    MissingNormals,
    MissingRoughness,
    MissingSpecularHitDistance,
    MissingHudlessColor,
    // Resource shape.
    ZeroRenderSize,
    ZeroOutputSize,
    OutputSmallerThanRender,  ///< SR/RR only upscale (or DLAA 1:1)
    RenderOutputSizeMismatch, ///< NR is one frame in, one frame out at the same size
    ResourceExtentMismatch,   ///< depth / motion vectors / guides not at render resolution
    // Constants.
    InvalidMotionVectorScale, ///< zero or non-finite mvec_scale
    JitterOutOfRange,         ///< |jitter| > 0.5 render pixel (Streamline's range) or non-finite
    MissingProjection,        ///< camera_view_to_clip all zero (RR reflections, FG)
    NonFiniteConstants,
    InvalidExposure,          ///< no exposure texture and exposure_scale <= 0
    HdrRequired,              ///< RR accepts linear HDR colour only
    UnknownFeature,
    // Warnings (never fail validation).
    WarnMissingUiColorAlpha,  ///< FG without UI alpha: HUD will be interpolated
    WarnMissingExposure,      ///< falls back to exposure_scale / auto exposure
};

[[nodiscard]] std::string_view input_status_name(InputStatus s) noexcept;
/// Status the provider ABI would use for the same condition (MISSING_INPUT / INVALID_CONSTANTS ...).
[[nodiscard]] FuseNvStatus to_plugin_status(InputStatus s) noexcept;
[[nodiscard]] constexpr bool is_warning(InputStatus s) noexcept {
    return s == InputStatus::WarnMissingUiColorAlpha || s == InputStatus::WarnMissingExposure;
}

/// FUSE_NV_BUFFER kinds (as 1u << kind) a feature cannot run without / should be given.
[[nodiscard]] u32 required_buffer_mask(FuseNvFeature feature) noexcept;
[[nodiscard]] u32 recommended_buffer_mask(FuseNvFeature feature) noexcept;
[[nodiscard]] std::string_view buffer_kind_name(FuseNvBufferKind kind) noexcept;

struct NvFrameInputs {
    std::array<FuseNvResourceTag, FUSE_NV_BUFFER_KIND_COUNT> tags{}; ///< slot == kind; native == 0 => absent
    FuseNvConstants constants{};

    NvFrameInputs();
    void set(FuseNvBufferKind kind, const FuseNvResource& resource,
             FuseNvLifecycle lifecycle = FUSE_NV_LIFECYCLE_VALID_UNTIL_EVALUATE);
    void clear(FuseNvBufferKind kind);
    [[nodiscard]] bool has(FuseNvBufferKind kind) const noexcept;
    [[nodiscard]] u32 present_mask() const noexcept;
    /// Present tags only, in kind order: what set_tags() receives.
    [[nodiscard]] std::vector<FuseNvResourceTag> packed() const;
};

struct InputIssue {
    InputStatus status = InputStatus::Ok;
    FuseNvBufferKind kind = FUSE_NV_BUFFER_KIND_COUNT; ///< offending buffer, KIND_COUNT if none
};

struct ValidationReport {
    std::vector<InputIssue> errors;   ///< in check order; errors.front() is the primary status
    std::vector<InputIssue> warnings;
    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
    [[nodiscard]] InputStatus status() const noexcept { return errors.empty() ? InputStatus::Ok : errors.front().status; }
    [[nodiscard]] bool has(InputStatus s) const noexcept;
};

/// Checks `in` against `feature`'s contract (Streamline / DLSS programming guides, research doc §1.3).
[[nodiscard]] ValidationReport validate_inputs(FuseNvFeature feature, const NvFrameInputs& in);

} // namespace fuse::renderer::nvidia
