#pragma once

#include <fuse/math/vec.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::math::SDF {

inline f32 sphere(Vec3 p, f32 radius) { return p.length() - radius; }

inline f32 box(Vec3 p, Vec3 halfExtents) {
    const Vec3 q{std::abs(p.x) - halfExtents.x, std::abs(p.y) - halfExtents.y,
                 std::abs(p.z) - halfExtents.z};
    const Vec3 outer{std::max(q.x, 0.f), std::max(q.y, 0.f), std::max(q.z, 0.f)};
    const f32 outside = outer.length();
    const f32 inside = std::min(std::max(q.x, std::max(q.y, q.z)), 0.f);
    return outside + inside;
}

inline f32 opSmoothUnion(f32 d1, f32 d2, f32 k) {
    const f32 h = std::max(k - std::abs(d1 - d2), 0.f) / k;
    return std::min(d1, d2) - h * h * k * 0.25f;
}

/// Analytic gradient of sphere(): unit direction away from the centre (+X at the centre).
inline Vec3 sphereGradient(Vec3 p) {
    const f32 len = p.length();
    if (len < 1e-8f) {
        return {1.f, 0.f, 0.f};
    }
    return p * (1.f / len);
}

/// Analytic gradient of box(). Outside: direction from the closest surface point. Inside: the
/// normal of the nearest face (undefined on the medial axis, where any adjacent face is returned).
inline Vec3 boxGradient(Vec3 p, Vec3 halfExtents) {
    const Vec3 q{std::abs(p.x) - halfExtents.x, std::abs(p.y) - halfExtents.y, std::abs(p.z) - halfExtents.z};
    const Vec3 sign{p.x < 0.f ? -1.f : 1.f, p.y < 0.f ? -1.f : 1.f, p.z < 0.f ? -1.f : 1.f};
    const Vec3 outer{std::max(q.x, 0.f), std::max(q.y, 0.f), std::max(q.z, 0.f)};
    const f32 outside = outer.length();
    if (outside > 0.f) {
        return {sign.x * outer.x / outside, sign.y * outer.y / outside, sign.z * outer.z / outside};
    }
    if (q.x >= q.y && q.x >= q.z) {
        return {sign.x, 0.f, 0.f};
    }
    if (q.y >= q.z) {
        return {0.f, sign.y, 0.f};
    }
    return {0.f, 0.f, sign.z};
}

/// Central-difference normal of any SDF `f(Vec3) -> f32` with step `h`.
template <typename Sdf>
inline Vec3 finiteDifferenceNormal(const Sdf& f, Vec3 p, f32 h = 1e-3f) {
    const Vec3 g{
        f(Vec3{p.x + h, p.y, p.z}) - f(Vec3{p.x - h, p.y, p.z}),
        f(Vec3{p.x, p.y + h, p.z}) - f(Vec3{p.x, p.y - h, p.z}),
        f(Vec3{p.x, p.y, p.z + h}) - f(Vec3{p.x, p.y, p.z - h}),
    };
    return g.normalized();
}

} // namespace fuse::math::SDF
