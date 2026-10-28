#pragma once

#include <fuse/math/vec.hpp>

#include <cmath>

namespace fuse::math {

/// Cubic Bernstein Bezier. t is clamped to 0..1. t = 0 returns p0, t = 1 returns p3.
[[nodiscard]] inline Vec3 cubic_bezier(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, f32 t) {
    auto finite = [](Vec3 p) {
        return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
    };
    if (!std::isfinite(t) || !finite(p0) || !finite(p1) || !finite(p2) || !finite(p3)) {
        return {};
    }
    if (t < 0.f) {
        t = 0.f;
    } else if (t > 1.f) {
        t = 1.f;
    }
    const f32 u = 1.f - t;
    const f32 uu = u * u;
    const f32 tt = t * t;
    const f32 w0 = uu * u;
    const f32 w1 = 3.f * uu * t;
    const f32 w2 = 3.f * u * tt;
    const f32 w3 = tt * t;
    return {w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x, w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y,
            w0 * p0.z + w1 * p1.z + w2 * p2.z + w3 * p3.z};
}

} // namespace fuse::math
