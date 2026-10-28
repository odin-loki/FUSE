#pragma once

#include <fuse/physics/math.hpp>

#include <cmath>

namespace fuse::physics {

/// Instant velocity change along a normalized hit direction: delta-v = direction * (impulse / mass).
/// A zero or non-finite direction, a non-positive or non-finite impulse, or a non-positive or
/// non-finite mass yields a zero delta-v.
[[nodiscard]] inline vec3 knockback_delta_v(vec3 hitDirection, f32 impulse, f32 mass) {
    if (!std::isfinite(hitDirection.x) || !std::isfinite(hitDirection.y) || !std::isfinite(hitDirection.z)) {
        return {};
    }
    const f32 lenSq = hitDirection.dot(hitDirection);
    if (!std::isfinite(lenSq) || !(lenSq > 0.f)) {
        return {};
    }
    if (!std::isfinite(impulse) || !(impulse > 0.f)) {
        return {};
    }
    if (!std::isfinite(mass) || !(mass > 0.f)) {
        return {};
    }
    return hitDirection * (impulse / mass);
}

} // namespace fuse::physics
