#pragma once

#include <fuse/physics/math.hpp>

#include <cmath>

namespace fuse::physics {

enum class ForceFalloff : u8 {
    None = 0,
    Linear = 1,
};

struct DirectionalVolume {
    vec3 center{};
    vec3 halfExtents{};
    vec3 force{};
    ForceFalloff falloff = ForceFalloff::None;
};

struct RadialVolume {
    vec3 center{};
    f32 radius = 0.f;
    f32 strength = 0.f;
    ForceFalloff falloff = ForceFalloff::None;
};

/// Linear falloff scales by 1 - max(|axis offset| / halfExtent). Outside the box the force is zero.
[[nodiscard]] inline vec3 sample_directional(const DirectionalVolume& volume, const vec3& point) {
    if (volume.halfExtents.x <= 0.f || volume.halfExtents.y <= 0.f || volume.halfExtents.z <= 0.f) {
        return {};
    }
    const f32 ax = std::fabs(point.x - volume.center.x) / volume.halfExtents.x;
    const f32 ay = std::fabs(point.y - volume.center.y) / volume.halfExtents.y;
    const f32 az = std::fabs(point.z - volume.center.z) / volume.halfExtents.z;
    if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az)) {
        return {};
    }
    const f32 t = std::max(ax, std::max(ay, az));
    if (t > 1.f) {
        return {};
    }
    const f32 scale = volume.falloff == ForceFalloff::Linear ? (1.f - t) : 1.f;
    return volume.force * scale;
}

/// Force points away from the center. The center itself has no direction, so the sample is zero.
[[nodiscard]] inline vec3 sample_radial(const RadialVolume& volume, const vec3& point) {
    if (!std::isfinite(volume.radius) || volume.radius <= 0.f || !std::isfinite(volume.strength)) {
        return {};
    }
    const vec3 delta = point - volume.center;
    const f32 dist = delta.length();
    if (!std::isfinite(dist) || dist <= 1e-8f || dist > volume.radius) {
        return {};
    }
    f32 mag = volume.strength;
    if (volume.falloff == ForceFalloff::Linear) {
        mag *= 1.f - dist / volume.radius;
    }
    return delta * (mag / dist);
}

} // namespace fuse::physics
