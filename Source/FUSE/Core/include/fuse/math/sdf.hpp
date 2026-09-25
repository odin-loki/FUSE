#pragma once

#include <fuse/math/vec.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::math::SDF {

FUSE_HOST_DEVICE inline f32 sphere(Vec3 p, f32 radius) { return p.length() - radius; }

FUSE_HOST_DEVICE inline f32 box(Vec3 p, Vec3 halfExtents) {
    const Vec3 q{std::abs(p.x) - halfExtents.x, std::abs(p.y) - halfExtents.y,
                 std::abs(p.z) - halfExtents.z};
    const Vec3 outer{std::max(q.x, 0.f), std::max(q.y, 0.f), std::max(q.z, 0.f)};
    const f32 outside = outer.length();
    const f32 inside = std::min(std::max(q.x, std::max(q.y, q.z)), 0.f);
    return outside + inside;
}

FUSE_HOST_DEVICE inline f32 opSmoothUnion(f32 d1, f32 d2, f32 k) {
    const f32 h = std::max(k - std::abs(d1 - d2), 0.f) / k;
    return std::min(d1, d2) - h * h * k * 0.25f;
}

FUSE_HOST_DEVICE inline f32 capsule(Vec3 p, f32 radius, f32 halfHeight) {
    const f32 y = std::clamp(p.y, -halfHeight, halfHeight);
    return Vec3{p.x, p.y - y, p.z}.length() - radius;
}

FUSE_HOST_DEVICE inline f32 cylinder(Vec3 p, f32 radius, f32 halfHeight) {
    const f32 radial = std::sqrt(p.x * p.x + p.z * p.z) - radius;
    const f32 vertical = std::abs(p.y) - halfHeight;
    const f32 outside = std::sqrt(std::max(radial, 0.f) * std::max(radial, 0.f) +
                                  std::max(vertical, 0.f) * std::max(vertical, 0.f));
    const f32 inside = std::min(std::max(radial, vertical), 0.f);
    return outside + inside;
}

FUSE_HOST_DEVICE inline f32 torus(Vec3 p, f32 majorRadius, f32 minorRadius) {
    const f32 ring = std::sqrt(p.x * p.x + p.z * p.z) - majorRadius;
    return std::sqrt(ring * ring + p.y * p.y) - minorRadius;
}

struct MarchHit {
    bool hit = false;
    f32 distance = 0.f;
    u32 steps = 0;
};

/// Sphere-trace `sdf` from `origin` along a normalized `direction`.
template <typename SdfFn>
FUSE_HOST_DEVICE inline MarchHit march(Vec3 origin, Vec3 direction, SdfFn sdf, u32 maxSteps = 64,
                                       f32 maxDistance = 32.f, f32 epsilon = 1e-3f) {
    MarchHit hit;
    const f32 dirLen = direction.length();
    if (dirLen <= 1e-8f || maxSteps == 0u || maxDistance <= 0.f) {
        return hit;
    }
    const Vec3 dir = direction * (1.f / dirLen);
    f32 traveled = 0.f;
    for (u32 step = 0; step < maxSteps; ++step) {
        const f32 dist = sdf(origin + dir * traveled);
        hit.steps = step + 1u;
        if (dist < epsilon) {
            hit.hit = true;
            hit.distance = traveled;
            return hit;
        }
        traveled += dist;
        if (traveled > maxDistance) {
            hit.distance = traveled;
            return hit;
        }
    }
    hit.distance = traveled;
    return hit;
}

/// Analytic gradient of sphere(): unit direction away from the centre (+X at the centre).
FUSE_HOST_DEVICE inline Vec3 sphereGradient(Vec3 p) {
    const f32 len = p.length();
    if (len < 1e-8f) {
        return {1.f, 0.f, 0.f};
    }
    return p * (1.f / len);
}

/// Analytic gradient of box(). Outside: direction from the closest surface point. Inside: the
/// normal of the nearest face (undefined on the medial axis, where any adjacent face is returned).
FUSE_HOST_DEVICE inline Vec3 boxGradient(Vec3 p, Vec3 halfExtents) {
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
FUSE_HOST_DEVICE inline Vec3 finiteDifferenceNormal(const Sdf& f, Vec3 p, f32 h = 1e-3f) {
    const Vec3 g{
        f(Vec3{p.x + h, p.y, p.z}) - f(Vec3{p.x - h, p.y, p.z}),
        f(Vec3{p.x, p.y + h, p.z}) - f(Vec3{p.x, p.y - h, p.z}),
        f(Vec3{p.x, p.y, p.z + h}) - f(Vec3{p.x, p.y, p.z - h}),
    };
    return g.normalized();
}

} // namespace fuse::math::SDF
