// 2D barycentric weights for the raster remaster. u, v and w weight triangle vertices a, b and c
// (u + v + w = 1). A point is inside when every weight is >= -1e-5. A degenerate triangle (area ~ 0)
// and any non-finite input return inside false and zero weights.
#pragma once

#include <cmath>

namespace fuse::relight::render::raster {

/// 2D point (x, y).
struct Vec2 {
    float x = 0.f;
    float y = 0.f;
};

/// Weights of vertices a, b and c.
struct Bary {
    float u = 0.f;
    float v = 0.f;
    float w = 0.f;
    bool inside = false;
};

/// Barycentric weights of `p` in triangle (`a`, `b`, `c`).
[[nodiscard]] inline Bary barycentric(Vec2 p, Vec2 a, Vec2 b, Vec2 c) {
    const auto finite = [](Vec2 q) { return std::isfinite(q.x) && std::isfinite(q.y); };
    if (!finite(p) || !finite(a) || !finite(b) || !finite(c)) {
        return {};
    }
    // Twice the signed area of triangle ijk. The factor of two cancels in the weight ratios.
    const auto area = [](Vec2 i, Vec2 j, Vec2 k) {
        return (j.x - i.x) * (k.y - i.y) - (k.x - i.x) * (j.y - i.y);
    };
    const float abc = area(a, b, c);
    if (!std::isfinite(abc) || std::fabs(abc) <= 1e-12f) {
        return {};
    }
    const float u = area(p, b, c) / abc;
    const float v = area(p, c, a) / abc;
    const float w = area(p, a, b) / abc;
    if (!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(w)) {
        return {};
    }
    Bary out;
    out.u = u;
    out.v = v;
    out.w = w;
    constexpr float kInside = -1e-5f;
    out.inside = u >= kInside && v >= kInside && w >= kInside;
    return out;
}

} // namespace fuse::relight::render::raster
