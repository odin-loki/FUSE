// FUSE Relight: windowed light range attenuation. Header-only. Unity at distance <= 0,
// zero at and beyond `range`, smoothstep's complement in between.
#pragma once

#include <cmath>

namespace fuse::relight::render::lights {

/// Smoothstep-complement falloff: 1 at `distance <= 0`, 0 at `distance >= range`.
/// `t = saturate(distance / range)`, result `(1 - t) * (1 - t) * (1 + 2 * t)` (that is, `1 - smoothstep`).
/// `range <= 0` or any non-finite input returns 0. A negative distance is treated as 0.
[[nodiscard]] inline float range_attenuation(float distance, float range) {
    if (!std::isfinite(distance) || !std::isfinite(range) || !(range > 0.f)) {
        return 0.f;
    }
    const float d = distance < 0.f ? 0.f : distance;
    const float raw = d / range;
    const float t = raw < 0.f ? 0.f : (raw > 1.f ? 1.f : raw);
    const float inv = 1.f - t;
    return inv * inv * (1.f + 2.f * t);
}

} // namespace fuse::relight::render::lights
