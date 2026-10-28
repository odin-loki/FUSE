#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::math {

/// 1 at distance 0, 0 at and beyond `range`.
[[nodiscard]] inline f32 falloff_linear(f32 distance, f32 range) {
    if (!std::isfinite(distance) || !std::isfinite(range) || range <= 0.f) {
        return 0.f;
    }
    const f32 d = std::fabs(distance);
    if (d >= range) {
        return 0.f;
    }
    return 1.f - d / range;
}

/// Smoothstep from 1 at distance 0 to 0 at `range`.
[[nodiscard]] inline f32 falloff_smooth(f32 distance, f32 range) {
    if (!std::isfinite(distance) || !std::isfinite(range) || range <= 0.f) {
        return 0.f;
    }
    const f32 d = std::fabs(distance);
    if (d >= range) {
        return 0.f;
    }
    const f32 t = d / range;
    const f32 s = t * t * (3.f - 2.f * t);
    return 1.f - s;
}

/// 1 / (1 + (distance/range)^2). 1 at the origin.
[[nodiscard]] inline f32 falloff_inverse_square(f32 distance, f32 range) {
    if (!std::isfinite(distance) || !std::isfinite(range) || range <= 0.f) {
        return 0.f;
    }
    const f32 d = std::fabs(distance) / range;
    return 1.f / (1.f + d * d);
}

} // namespace fuse::math
