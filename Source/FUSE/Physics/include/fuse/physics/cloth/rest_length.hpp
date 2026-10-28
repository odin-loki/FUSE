#pragma once

#include <cmath>

namespace fuse::physics {

/// Signed cloth edge error: distance - rest.
/// A negative rest is treated as 0. A negative distance is treated as 0.
/// A non-finite distance or rest returns 0.
[[nodiscard]] inline float rest_length_error(float distance, float rest) {
    if (!std::isfinite(distance) || !std::isfinite(rest)) {
        return 0.f;
    }
    if (distance < 0.f) {
        distance = 0.f;
    }
    if (rest < 0.f) {
        rest = 0.f;
    }
    return distance - rest;
}

/// Position correction along an edge: error * saturate(stiffness).
/// Stiffness is clamped to 0..1. A non-finite stiffness returns 0.
/// Stretch is positive and compression is negative.
[[nodiscard]] inline float rest_length_correction(float distance, float rest, float stiffness) {
    const auto saturate = [](float value) {
        if (!std::isfinite(value) || value <= 0.f) {
            return 0.f;
        }
        return value >= 1.f ? 1.f : value;
    };
    return rest_length_error(distance, rest) * saturate(stiffness);
}

} // namespace fuse::physics
