#pragma once

#include <fuse/math/vec.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace fuse::math {

namespace spline_detail {

[[nodiscard]] FUSE_HOST_DEVICE inline bool isFinite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] FUSE_HOST_DEVICE inline f32 clamp01(f32 t) { return std::clamp(t, 0.f, 1.f); }

/// Uniform Catmull-Rom basis on one axis. `t` is in [0, 1].
[[nodiscard]] FUSE_HOST_DEVICE inline f32 axis(f32 c0, f32 c1, f32 c2, f32 c3, f32 t) {
    const f32 t2 = t * t;
    const f32 t3 = t2 * t;
    return 0.5f * ((2.f * c1) + (-c0 + c2) * t + (2.f * c0 - 5.f * c1 + 4.f * c2 - c3) * t2 +
                   (-c0 + 3.f * c1 - 3.f * c2 + c3) * t3);
}

} // namespace spline_detail

/// Uniform Catmull-Rom segment. `t` is clamped to [0, 1].
/// `t == 0` returns `p1` and `t == 1` returns `p2`.
/// A non-finite control component or `t` returns the zero vector.
[[nodiscard]] FUSE_HOST_DEVICE inline Vec3 catmull_rom(const Vec3& p0, const Vec3& p1, const Vec3& p2,
                                                       const Vec3& p3, f32 t) {
    if (!spline_detail::isFinite(p0) || !spline_detail::isFinite(p1) || !spline_detail::isFinite(p2) ||
        !spline_detail::isFinite(p3) || !std::isfinite(t)) {
        return {};
    }
    const f32 u = spline_detail::clamp01(t);
    if (u == 0.f) {
        return p1;
    }
    if (u == 1.f) {
        return p2;
    }
    return {spline_detail::axis(p0.x, p1.x, p2.x, p3.x, u), spline_detail::axis(p0.y, p1.y, p2.y, p3.y, u),
            spline_detail::axis(p0.z, p1.z, p2.z, p3.z, u)};
}

/// Sample an open polyline. `t == 0` is `points.front()` and `t == 1` is `points.back()`.
/// End controls are duplicated (`p0 == p1` on the first segment, `p3 == p2` on the last).
/// Fewer than two points, or a non-finite control component or `t`, sets `ok` to false and
/// returns the zero vector.
[[nodiscard]] inline Vec3 sample_polyline(std::span<const Vec3> points, f32 t, bool& ok) {
    ok = false;
    if (points.size() < 2 || !std::isfinite(t)) {
        return {};
    }
    for (const Vec3& point : points) {
        if (!spline_detail::isFinite(point)) {
            return {};
        }
    }
    const f32 u = spline_detail::clamp01(t);
    if (u == 0.f) {
        ok = true;
        return points.front();
    }
    if (u == 1.f) {
        ok = true;
        return points.back();
    }

    const std::size_t segments = points.size() - 1;
    const f32 scaled = u * static_cast<f32>(segments);
    if (!std::isfinite(scaled)) {
        return {};
    }
    std::size_t segment = static_cast<std::size_t>(scaled);
    if (segment >= segments) {
        segment = segments - 1;
    }
    const f32 local = scaled - static_cast<f32>(segment);
    const Vec3& p1 = points[segment];
    const Vec3& p2 = points[segment + 1];
    const Vec3 p0 = (segment == 0) ? p1 : points[segment - 1];
    const Vec3 p3 = (segment + 1 == segments) ? p2 : points[segment + 2];
    ok = true;
    return catmull_rom(p0, p1, p2, p3, local);
}

} // namespace fuse::math
