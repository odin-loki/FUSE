#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/postprocess/tonemap.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Colour grading parameters (B5.10 — P5 §5.10).
struct ColorGradeParams {
    ToneMapper tone_mapper = ToneMapper::ACES;
    f32 exposure = 0.f;
    f32 contrast = 1.f;
    f32 saturation = 1.f;
    fuse::math::Vec3 lift{};
    fuse::math::Vec3 gamma{1.f, 1.f, 1.f};
    fuse::math::Vec3 gain{1.f, 1.f, 1.f};
    f32 vignette = 0.3f;
    f32 film_grain = 0.02f;
    bool output_srgb = true;
    /// Add `tone_mapper_mid_grey_calibration_ev(tone_mapper)` to the exposure so scene-linear 0.18
    /// maps to display-linear 0.18 (sRGB-encoded ~0.461) at exposure 0.
    bool calibrate_mid_grey = true;
};

/// Exposure (EV) actually applied before the tone-map operator: exposure + calibration bias.
f32 color_grade_total_exposure_ev(const ColorGradeParams& params);

fuse::math::Vec3 linear_to_srgb(const fuse::math::Vec3& linear);

/// Per-pixel, per-frame film-grain sample in [-1, 1): zero mean, variance 1/3, independent across
/// pixels and across frame seeds (64-bit avalanche hash of seed and pixel coordinates).
f32 film_grain_noise(u64 frame_seed, u32 px, u32 py);
/// Additive luminance grain: color + intensity * film_grain_noise. A zero seed disables grain.
fuse::math::Vec3 apply_film_grain(const fuse::math::Vec3& color, f32 intensity, u64 frame_seed, u32 px, u32 py);

/// Radial vignette gain: 1 - strength * r^2, r^2 = (u^2 + v^2) / 2 with u, v in [-1, 1] across the
/// frame (1 at the centre, 1 - strength in the corners).
f32 vignette_factor(f32 strength, u32 px, u32 py, u32 width, u32 height);

/// Exposure + tone map + lift/contrast/saturation/gamma/gain (display-linear, unclamped).
fuse::math::Vec3 grade_linear(const fuse::math::Vec3& input, const ColorGradeParams& params);
/// Saturate to [0, 1] and optionally apply the sRGB OETF.
fuse::math::Vec3 finalize_display(const fuse::math::Vec3& color, bool output_srgb);

/// Single-pixel grade (frame centre, grain sampled at pixel (0, 0)).
fuse::math::Vec3 apply_color_grade(const fuse::math::Vec3& input, const ColorGradeParams& params,
                                   u64 frame_seed = 0);
/// Full grade at pixel (px, py) of a width x height frame: grade → vignette → grain → display.
fuse::math::Vec3 apply_color_grade(const fuse::math::Vec3& input, const ColorGradeParams& params, u64 frame_seed,
                                   u32 px, u32 py, u32 width, u32 height);

/// Host-side colour-grade pass stub (CUDA kernel deferred).
class ColorGrade {
public:
    void setParams(const ColorGradeParams& params) { m_params = params; }
    const ColorGradeParams& params() const { return m_params; }
    bool isReady() const { return m_ready; }
    void init();
    void destroy();

    fuse::math::Vec3 apply(const fuse::math::Vec3& input, u64 frame_seed = 0) const;

private:
    ColorGradeParams m_params{};
    bool m_ready = false;
};

} // namespace fuse::renderer
