#pragma once

#include <fuse/physics/math.hpp>

#include <cmath>

namespace fuse::physics {

struct DragParams {
    f32 linear = 0.f;
    f32 quadratic = 0.f;
};

/// F = -linear * v - quadratic * |v| * v.
/// Negative coefficients are treated as zero. A zero velocity, or any non-finite
/// velocity component or coefficient, returns a zero force.
[[nodiscard]] inline vec3 drag_force(vec3 velocity, DragParams params) {
    if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z) ||
        !std::isfinite(params.linear) || !std::isfinite(params.quadratic)) {
        return {};
    }

    const f32 linear = params.linear < 0.f ? 0.f : params.linear;
    const f32 quadratic = params.quadratic < 0.f ? 0.f : params.quadratic;
    const f32 speed = velocity.length();
    if (!(speed > 0.f) || !std::isfinite(speed)) {
        return {};
    }

    return velocity * -(linear + quadratic * speed);
}

} // namespace fuse::physics
