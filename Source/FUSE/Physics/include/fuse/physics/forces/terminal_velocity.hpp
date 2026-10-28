#pragma once

#include <fuse/physics/math.hpp>

#include <cmath>

namespace fuse::physics {

/// Clamp a velocity to maxSpeed while keeping its direction.
/// A non-positive or non-finite maxSpeed, or a non-finite velocity, returns zero.
/// A finite speed at or below maxSpeed is returned unchanged.
[[nodiscard]] inline vec3 clamp_terminal_velocity(vec3 velocity, f32 maxSpeed) {
    if (!std::isfinite(maxSpeed) || !(maxSpeed > 0.f)) {
        return {};
    }
    if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z)) {
        return {};
    }

    const f32 speed = velocity.length();
    if (!std::isfinite(speed)) {
        return {};
    }
    if (speed <= maxSpeed) {
        return velocity;
    }
    return velocity * (maxSpeed / speed);
}

} // namespace fuse::physics
