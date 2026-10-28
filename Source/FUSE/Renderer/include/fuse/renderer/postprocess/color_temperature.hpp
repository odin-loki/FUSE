#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

/// Linear RGB for a blackbody colour temperature (Tanner Helland). Kelvin is clamped to
/// 1000..40000, channels are clamped to 0..255, then scaled so the brightest channel is 1.
/// A non-finite temperature returns white.
[[nodiscard]] inline fuse::math::Vec3 color_temperature_rgb(f32 kelvin) {
    if (!std::isfinite(kelvin)) {
        return {1.f, 1.f, 1.f};
    }
    const f32 temperature = std::clamp(kelvin, 1000.f, 40000.f) / 100.f;

    f32 red;
    f32 green;
    f32 blue;
    if (temperature <= 66.f) {
        red = 255.f;
        green = 99.4708025861f * std::log(temperature) - 161.1195681661f;
    } else {
        const f32 shifted = temperature - 60.f;
        red = 329.698727446f * std::pow(shifted, -0.1332047592f);
        green = 288.1221695283f * std::pow(shifted, -0.0755148492f);
    }
    if (temperature >= 66.f) {
        blue = 255.f;
    } else if (temperature <= 19.f) {
        blue = 0.f;
    } else {
        blue = 138.5177312231f * std::log(temperature - 10.f) - 305.0447927307f;
    }

    red = std::clamp(red, 0.f, 255.f);
    green = std::clamp(green, 0.f, 255.f);
    blue = std::clamp(blue, 0.f, 255.f);
    const f32 peak = std::max(red, std::max(green, blue));
    return {red / peak, green / peak, blue / peak};
}

} // namespace fuse::renderer
