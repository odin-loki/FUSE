// FUSE Relight: Henyey-Greenstein phase function [Henyey and Greenstein 1941]. Header-only.
#pragma once

#include <cmath>

namespace fuse::relight::render::volumetrics {

/// `(1 - g^2) / (4 * pi * (1 + g^2 - 2 * g * cosTheta)^1.5)`.
/// `g` is clamped to [-0.99, 0.99] and `cosTheta` to [-1, 1]. A non-finite input or result returns 0.
[[nodiscard]] inline float henyey_greenstein(float cosTheta, float g) {
    if (!std::isfinite(cosTheta) || !std::isfinite(g)) {
        return 0.f;
    }
    if (g < -0.99f) {
        g = -0.99f;
    } else if (g > 0.99f) {
        g = 0.99f;
    }
    if (cosTheta < -1.f) {
        cosTheta = -1.f;
    } else if (cosTheta > 1.f) {
        cosTheta = 1.f;
    }
    constexpr float kPi = 3.14159265358979323846f;
    const float g2 = g * g;
    const float denom = 1.f + g2 - 2.f * g * cosTheta;
    const float value = (1.f - g2) / (4.f * kPi * std::pow(denom, 1.5f));
    return std::isfinite(value) ? value : 0.f;
}

} // namespace fuse::relight::render::volumetrics
