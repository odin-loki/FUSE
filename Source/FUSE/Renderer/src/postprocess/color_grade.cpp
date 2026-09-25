#include <fuse/renderer/postprocess/color_grade.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

namespace {

f32 saturate_channel(f32 value) {
    return std::clamp(value, 0.f, 1.f);
}

fuse::math::Vec3 apply_saturation(const fuse::math::Vec3& rgb, f32 saturation) {
    const f32 lum = 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
    const fuse::math::Vec3 grey{lum, lum, lum};
    return grey + (rgb - grey) * saturation;
}

u64 splitmix64(u64 value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

} // namespace

f32 color_grade_total_exposure_ev(const ColorGradeParams& params) {
    const f32 calibration = params.calibrate_mid_grey ? tone_mapper_mid_grey_calibration_ev(params.tone_mapper) : 0.f;
    return params.exposure + calibration;
}

fuse::math::Vec3 linear_to_srgb(const fuse::math::Vec3& linear) {
    const auto encode = [](f32 channel) {
        if (channel <= 0.0031308f) {
            return 12.92f * channel;
        }
        return 1.055f * std::pow(channel, 1.f / 2.4f) - 0.055f;
    };
    return {encode(linear.x), encode(linear.y), encode(linear.z)};
}

f32 film_grain_noise(u64 frame_seed, u32 px, u32 py) {
    const u64 pixel = (static_cast<u64>(py) << 32) | static_cast<u64>(px);
    const u64 hash = splitmix64(splitmix64(frame_seed) ^ pixel);
    // Top 24 bits -> cell centres of [0, 1): exactly zero-mean over the full range.
    const f32 unit = (static_cast<f32>(hash >> 40) + 0.5f) * (1.f / 16777216.f);
    return unit * 2.f - 1.f;
}

fuse::math::Vec3 apply_film_grain(const fuse::math::Vec3& color, f32 intensity, u64 frame_seed, u32 px, u32 py) {
    if (intensity <= 0.f || frame_seed == 0u) {
        return color;
    }
    const f32 grain = film_grain_noise(frame_seed, px, py) * intensity;
    return color + fuse::math::Vec3{grain, grain, grain};
}

f32 vignette_factor(f32 strength, u32 px, u32 py, u32 width, u32 height) {
    if (strength == 0.f || width == 0u || height == 0u) {
        return 1.f;
    }
    const f32 u = (static_cast<f32>(px) + 0.5f) / static_cast<f32>(width) * 2.f - 1.f;
    const f32 v = (static_cast<f32>(py) + 0.5f) / static_cast<f32>(height) * 2.f - 1.f;
    const f32 r2 = 0.5f * (u * u + v * v);
    return std::max(1.f - strength * r2, 0.f);
}

fuse::math::Vec3 grade_linear(const fuse::math::Vec3& input, const ColorGradeParams& params) {
    fuse::math::Vec3 color = input * std::exp2(color_grade_total_exposure_ev(params));
    color = apply_tone_map(color, params.tone_mapper);
    color = (color + params.lift - fuse::math::Vec3{0.5f, 0.5f, 0.5f}) * params.contrast +
            fuse::math::Vec3{0.5f, 0.5f, 0.5f};
    color = apply_saturation(color, params.saturation);
    color = fuse::math::Vec3{std::pow(std::max(color.x, 0.f), 1.f / params.gamma.x) * params.gain.x,
                             std::pow(std::max(color.y, 0.f), 1.f / params.gamma.y) * params.gain.y,
                             std::pow(std::max(color.z, 0.f), 1.f / params.gamma.z) * params.gain.z};
    return color;
}

fuse::math::Vec3 finalize_display(const fuse::math::Vec3& color, bool output_srgb) {
    const fuse::math::Vec3 clamped{saturate_channel(color.x), saturate_channel(color.y), saturate_channel(color.z)};
    return output_srgb ? linear_to_srgb(clamped) : clamped;
}

fuse::math::Vec3 apply_color_grade(const fuse::math::Vec3& input, const ColorGradeParams& params,
                                   u64 frame_seed) {
    fuse::math::Vec3 color = grade_linear(input, params);
    color = apply_film_grain(color, params.film_grain, frame_seed, 0u, 0u);
    return finalize_display(color, params.output_srgb);
}

fuse::math::Vec3 apply_color_grade(const fuse::math::Vec3& input, const ColorGradeParams& params, u64 frame_seed,
                                   u32 px, u32 py, u32 width, u32 height) {
    fuse::math::Vec3 color = grade_linear(input, params);
    color = color * vignette_factor(params.vignette, px, py, width, height);
    color = apply_film_grain(color, params.film_grain, frame_seed, px, py);
    return finalize_display(color, params.output_srgb);
}

void ColorGrade::init() {
    m_ready = true;
}

void ColorGrade::destroy() {
    m_ready = false;
}

fuse::math::Vec3 ColorGrade::apply(const fuse::math::Vec3& input, u64 frame_seed) const {
    return apply_color_grade(input, m_params, frame_seed);
}

} // namespace fuse::renderer
