#pragma once

#include <fuse/physics/math.hpp>

#include <cmath>

namespace fuse::physics {

/// Torque of a force applied at `point` about `center`: cross(point - center, force).
/// Any non-finite component of center, point, or force returns a zero torque.
[[nodiscard]] inline vec3 torque_from_force(vec3 center, vec3 point, vec3 force) {
    if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z) ||
        !std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
        !std::isfinite(force.x) || !std::isfinite(force.y) || !std::isfinite(force.z)) {
        return {};
    }
    return (point - center).cross(force);
}

} // namespace fuse::physics
