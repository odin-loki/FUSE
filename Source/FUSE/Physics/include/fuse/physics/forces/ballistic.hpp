#pragma once

#include <fuse/physics/math.hpp>

#include <cmath>

namespace fuse::physics {

/// p(t) = origin + velocity * t + 0.5 * gravity * t^2.
/// Negative time is clamped to zero. Any non-finite input returns origin when origin is finite,
/// otherwise a zero vector.
[[nodiscard]] inline vec3 ballistic_position(vec3 origin, vec3 velocity, vec3 gravity, f32 time) {
    const bool originFinite =
        std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z);
    if (!originFinite || !std::isfinite(velocity.x) || !std::isfinite(velocity.y) ||
        !std::isfinite(velocity.z) || !std::isfinite(gravity.x) || !std::isfinite(gravity.y) ||
        !std::isfinite(gravity.z) || !std::isfinite(time)) {
        return originFinite ? origin : vec3{};
    }

    if (time < 0.f) {
        time = 0.f;
    }

    const f32 halfT2 = 0.5f * time * time;
    return origin + velocity * time + gravity * halfT2;
}

} // namespace fuse::physics
