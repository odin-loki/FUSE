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

} // namespace fuse::math::SDF
