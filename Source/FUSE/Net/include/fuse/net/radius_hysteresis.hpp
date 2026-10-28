#pragma once

#include <cmath>

namespace fuse::net {

/// True while inside `radius`, leaving only after `radius + slack`.
///
/// A non-finite `distance` is outside. A negative `radius` or `slack` counts as 0.
/// An entity already inside stays until `distance` is greater than `radius + slack`.
/// An entity outside enters only when `distance` is less than or equal to `radius`.
[[nodiscard]] inline bool radius_hysteresis(float distance, float radius, float slack,
                                             bool wasInside) noexcept {
    if (!std::isfinite(distance)) {
        return false;
    }
    const float enterRadius = radius < 0.f ? 0.f : radius;
    const float leaveSlack = slack < 0.f ? 0.f : slack;
    if (wasInside) {
        return !(distance > enterRadius + leaveSlack);
    }
    return distance <= enterRadius;
}

} // namespace fuse::net
