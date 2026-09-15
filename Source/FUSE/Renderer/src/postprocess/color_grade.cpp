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

f32 film_grain_noise(u64 seed, f32 x, f32 y) {
    const u64 hash = seed ^ (static_cast<u64>(x * 9973.f) * 0x9E3779B97F4A7C15ull) ^
                     (static_cast<u64>(y * 7919.f) * 0xBF58476D1CE4E5B9ull);
    const f32 unit = static_cast<f32>(hash & 0xFFFFu) / static_cast<f32>(0xFFFFu);
    return unit * 2.f - 1.f;
}

} // namespace

fuse::math::Vec3 linear_to_srgb(const fuse::math::Vec3& linear) {
    const auto encode = [](f32 channel) {
        if (channel <= 0.0031308f) {
            return 12.92f * channel;
        }
        return 1.055f * std::pow(channel, 1.f / 2.4f) - 0.055f;
    };
    return {encode(linear.x), encode(linear.y), encode(linear.z)};
}

fuse::math::Vec3 apply_color_grade(const fuse::math::Vec3& input, const ColorGradeParams& params,
                                   u64 frame_seed) {
    fuse::math::Vec3 color = input * std::pow(2.f, params.exposure);
    color = apply_tone_map(color, params.tone_mapper);
    color = (color + params.lift - fuse::math::Vec3{0.5f, 0.5f, 0.5f}) * params.contrast +
            fuse::math::Vec3{0.5f, 0.5f, 0.5f};
    color = apply_saturation(color, params.saturation);
    color = fuse::math::Vec3{std::pow(std::max(color.x, 0.f), 1.f / params.gamma.x) * params.gain.x,
                             std::pow(std::max(color.y, 0.f), 1.f / params.gamma.y) * params.gain.y,
                             std::pow(std::max(color.z, 0.f), 1.f / params.gamma.z) * params.gain.z};

    if (params.film_grain > 0.f && frame_seed != 0u) {
        const f32 grain = film_grain_noise(frame_seed, color.x, color.y) * params.film_grain;
        color = color + fuse::math::Vec3{grain, grain, grain};
    }

    color = {saturate_channel(color.x), saturate_channel(color.y), saturate_channel(color.z)};
    if (params.output_srgb) {
        color = linear_to_srgb(color);
    }
    return color;
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
