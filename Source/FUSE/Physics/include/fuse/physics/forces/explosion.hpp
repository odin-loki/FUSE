#pragma once

#include <fuse/physics/math.hpp>

#include <cmath>

namespace fuse::physics {

struct Explosion {
    vec3 origin{};
    f32 radius = 0.f;
    f32 impulse = 0.f;
};

/// Instant velocity change directed away from the blast origin.
/// Linear falloff: scale = 1 - distance/radius, delta-v = direction * (impulse * scale) / mass.
/// The origin has no direction. A point outside the radius, a non-positive radius, impulse, or mass,
/// or any non-finite input yields a zero delta-v.
[[nodiscard]] inline vec3 explosion_impulse(const Explosion& boom, vec3 point, f32 mass) {
    if (!std::isfinite(boom.origin.x) || !std::isfinite(boom.origin.y) || !std::isfinite(boom.origin.z) ||
        !std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
        !std::isfinite(boom.radius) || !std::isfinite(boom.impulse) || !std::isfinite(mass)) {
        return {};
    }
    if (!(boom.radius > 0.f) || !(boom.impulse > 0.f) || !(mass > 0.f)) {
        return {};
    }

    const vec3 offset = point - boom.origin;
    const f32 distSq = offset.dot(offset);
    if (!std::isfinite(distSq) || !(distSq > 0.f)) {
        return {};
    }
    const f32 distance = std::sqrt(distSq);
    if (!std::isfinite(distance) || distance > boom.radius) {
        return {};
    }

    const f32 scale = 1.f - distance / boom.radius;
    const f32 deltaV = (boom.impulse * scale) / (mass * distance);
    if (!std::isfinite(scale) || !std::isfinite(deltaV)) {
        return {};
    }
    return offset * deltaV;
}

} // namespace fuse::physics
