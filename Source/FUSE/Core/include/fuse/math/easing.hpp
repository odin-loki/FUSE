#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::math {

namespace detail {

/// Non-finite `t` becomes 0. Finite `t` is clamped to [0, 1].
FUSE_HOST_DEVICE inline f32 clampedUnit(f32 t) {
    if (!std::isfinite(t)) {
        return 0.f;
    }
    if (t < 0.f) {
        return 0.f;
    }
    if (t > 1.f) {
        return 1.f;
    }
    return t;
}

} // namespace detail

/// Hermite smoothstep: 3t^2 - 2t^3.
FUSE_HOST_DEVICE inline f32 smoothstep(f32 t) {
    t = detail::clampedUnit(t);
    return t * t * (3.f - 2.f * t);
}

/// Perlin smootherstep: 6t^5 - 15t^4 + 10t^3.
FUSE_HOST_DEVICE inline f32 smootherstep(f32 t) {
    t = detail::clampedUnit(t);
    return t * t * t * (t * (t * 6.f - 15.f) + 10.f);
}

/// Cubic ease-in: t^3.
FUSE_HOST_DEVICE inline f32 ease_in_cubic(f32 t) {
    t = detail::clampedUnit(t);
    return t * t * t;
}

/// Cubic ease-out: 1 - (1 - t)^3.
FUSE_HOST_DEVICE inline f32 ease_out_cubic(f32 t) {
    t = detail::clampedUnit(t);
    const f32 u = 1.f - t;
    return 1.f - u * u * u;
}

/// Cubic ease-in-out. Midpoint t = 0.5 maps to 0.5.
FUSE_HOST_DEVICE inline f32 ease_in_out_cubic(f32 t) {
    t = detail::clampedUnit(t);
    if (t < 0.5f) {
        return 4.f * t * t * t;
    }
    const f32 u = -2.f * t + 2.f;
    return 1.f - (u * u * u) * 0.5f;
}

/// Ease-out back. Negative `overshoot` clamps to 0. Default overshoot is 1.70158.
FUSE_HOST_DEVICE inline f32 ease_out_back(f32 t, f32 overshoot = 1.70158f) {
    if (!std::isfinite(t)) {
        return 0.f;
    }
    t = detail::clampedUnit(t);
    if (overshoot < 0.f) {
        overshoot = 0.f;
    }
    const f32 c3 = overshoot + 1.f;
    const f32 u = t - 1.f;
    const f32 u2 = u * u;
    return 1.f + c3 * u2 * u + overshoot * u2;
}

} // namespace fuse::math
