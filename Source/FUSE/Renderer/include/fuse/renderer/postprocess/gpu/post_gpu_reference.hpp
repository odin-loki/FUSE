#pragma once

// WP-4.5 GPU post stack: the host-side settings, their resolution from the two front ends (the
// B5 PostStack and a Look effect graph), and the CPU references the parity gates compare the GPU
// against. Vulkan-free (builds and runs in the stub backend).
//
// The CPU oracle is renderer/postprocess/* (bloom_composite, dof_pass, motion_blur_pass,
// LuminanceHistogram + AutoExposure, TonemapCurve, apply_tone_map incl. AgX, the grade stages of
// grade_linear, vignette_factor, apply_film_grain, finalize_display) plus the Look kernels for the
// LUT and the look vignette / grain (look/look_kernels.hpp). Everything here composes those
// functions; the only restated arithmetic is `display_reference`'s stage sequence, which calls the
// oracle for every stage it can and mirrors grade_linear's three stages (clamp / lift + contrast,
// saturation, gamma + gain) one by one so neutral stages can be skipped exactly as the kernel does.

#include <fuse/math/vec.hpp>
#include <fuse/renderer/look/effect_graph.hpp>
#include <fuse/renderer/look/look_params.hpp>
#include <fuse/renderer/look/lut3d.hpp>
#include <fuse/renderer/postprocess/auto_exposure.hpp>
#include <fuse/renderer/postprocess/bloom.hpp>
#include <fuse/renderer/postprocess/color_grade.hpp>
#include <fuse/renderer/postprocess/dof.hpp>
#include <fuse/renderer/postprocess/gpu/post_gpu_types.hpp>
#include <fuse/renderer/postprocess/motion_blur.hpp>
#include <fuse/renderer/postprocess/tonemap.hpp>
#include <fuse/renderer/postprocess/tonemap_curve.hpp>
#include <fuse/types.hpp>

#include <array>
#include <vector>

namespace fuse::renderer {
class PostStack;
}

namespace fuse::renderer::post_gpu {

/// HDR spatial passes; they run in `PostGpuSettings::spatialOrder` (PostStack: bloom, DoF, motion
/// blur; a Look: its graph's order).
enum class PostSpatialPass : u8 { Bloom = 0, DepthOfField = 1, MotionBlur = 2 };
inline constexpr u32 kPostSpatialPassCount = 3u;

enum class PostVignetteMode : u8 { None = 0, Stack = 1, Look = 2 };
enum class PostGrainMode : u8 { None = 0, Stack = 1, Look = 2 };

/// Everything one GPU post frame needs besides its images. Plain data: the same settings always
/// produce the same PostFrameConstants (and therefore the same bits on the GPU).
struct PostGpuSettings {
    std::array<PostSpatialPass, kPostSpatialPassCount> spatialOrder{PostSpatialPass::Bloom, PostSpatialPass::DepthOfField,
                                                                    PostSpatialPass::MotionBlur};
    u32 spatialCount = kPostSpatialPassCount;

    bool bloom = false;
    BloomParams bloomParams{};
    math::Vec3 bloomTint{1.f, 1.f, 1.f};
    bool dof = false;             ///< runs only with a depth input
    DOFParams dofParams{};
    bool motionBlur = false;      ///< runs only with a velocity input
    MotionBlurParams motionBlurParams{};

    /// Manual exposure + mid-grey calibration (EV). The adapted EV (GPU state) is subtracted when
    /// `autoExposure` is set.
    f32 exposureEv = 0.f;
    bool autoExposure = false;
    AutoExposureParams autoExposureParams{};
    LuminanceHistogramParams histogram{};
    f32 deltaSeconds = 1.f / 60.f;

    bool curve = false;
    TonemapCurveParams curveParams{};
    ToneMapper toneMapper = ToneMapper::ACES;

    /// PostStack grade stages (grade_linear); each one only when non-neutral.
    bool gradeLiftContrast = false;
    bool gradeSaturation = false;
    bool gradeGammaGain = false;
    math::Vec3 lift{};
    f32 contrast = 1.f;
    f32 saturation = 1.f;
    math::Vec3 gamma{1.f, 1.f, 1.f};
    math::Vec3 gain{1.f, 1.f, 1.f};
    /// Look grade: the LUT given to PostStackGpu::setGradeLut (skipped when none is set).
    bool lut = false;

    PostVignetteMode vignetteMode = PostVignetteMode::None;
    f32 vignette = 0.f;
    f32 vignetteFalloff = 2.f;
    f32 vignetteRoundness = 0.f;
    math::Vec3 vignetteTint{};
    PostGrainMode grainMode = PostGrainMode::None;
    f32 grain = 0.f;
    f32 grainResponse = 0.f;
    u64 frameSeed = 0;
    bool outputSrgb = true;

    bool anyGrade() const { return gradeLiftContrast || gradeSaturation || gradeGammaGain; }
};

/// The PostStack front end: the settings PostStack::processFrame applies (bloom when its intensity is
/// non-zero, DoF / motion blur when enabled, exposure = manual + midGreyCalibrationEv, auto exposure
/// from the GPU adaptation state when enabled, the curve when it is ready to apply, the tone mapper,
/// the grade stages that are not neutral, vignette / grain, sRGB). `histogram` keeps its defaults.
PostGpuSettings settings_from_post_stack(const PostStack& stack, u64 frame_seed = 0, f32 delta_seconds = 1.f / 60.f);

/// Look effect nodes the GPU stack has no pass for (bit = 1 << LookEffect): lens dirt, lens flare,
/// sharpen, chromatic aberration. They are reported and skipped (the CPU LookPostChain runs them).
u32 look_unsupported_nodes(const look::LookEffectGraph& graph, const look::LookResolved& look);

/// The Look front end: walks the (validated) effect graph node by node. DoF / motion blur / bloom
/// follow the graph order; exposure = bias + calibration (auto exposure meters the GPU histogram,
/// where the CPU LookPostChain uses a strided mean); tone map; the grade node selects the LUT;
/// vignette / grain use the look's own formulas; output transform sRGB. Returns false for an invalid
/// graph. `unsupported` receives look_unsupported_nodes().
bool settings_from_look(const look::LookEffectGraph& graph, const look::LookResolved& look, u64 frame_seed,
                        f32 delta_seconds, PostGpuSettings& out, u32* unsupported = nullptr);

/// Host-resolved kernel constants for `settings` (everything except addresses, extents, handles and
/// the per-frame exposure reset). Deterministic.
void resolve_constants(const PostGpuSettings& settings, PostFrameConstants& c);

/// Pyramid depth the bloom pass uses (bloom_level_count, capped at kPostMaxBloomLevels).
u32 bloom_levels(const PostGpuSettings& settings, u32 width, u32 height);

// --- CPU references ------------------------------------------------------------------------------

/// The HDR spatial chain on the CPU oracle in the settings' order (bloom_composite with the tint,
/// dof_pass, motion_blur_pass), i.e. the input of the display transform.
void spatial_reference(const PostGpuSettings& settings, const math::Vec3* hdr, const f32* linear_depth,
                       const math::Vec2* velocity, u32 width, u32 height, std::vector<math::Vec3>& out);

/// Per-pixel display transform the kernel runs (exposure scale given, e.g. the oracle's 2^EV).
math::Vec3 display_reference(const PostGpuSettings& settings, const math::Vec3& hdr, f32 exposure_scale, u32 x, u32 y,
                             u32 width, u32 height, const look::Lut3D* lut);

/// LuminanceHistogram bins of an image (the oracle's accumulate), `bins` sized to bin_count.
void histogram_reference(const math::Vec3* image, u32 count, const LuminanceHistogramParams& params,
                         std::vector<u32>& bins);

} // namespace fuse::renderer::post_gpu
