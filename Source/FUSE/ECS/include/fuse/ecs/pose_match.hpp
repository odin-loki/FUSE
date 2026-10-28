#pragma once

#include <cmath>

namespace fuse::ecs {

/// True when squared distance between the two poses is at most epsilon squared.
/// A negative epsilon is treated as 0. Any non-finite component returns false.
[[nodiscard]] inline bool poses_match(float ax, float ay, float az, float bx, float by, float bz,
                                       float epsilon) {
    if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az) || !std::isfinite(bx) ||
        !std::isfinite(by) || !std::isfinite(bz)) {
        return false;
    }
    if (epsilon < 0.f) {
        epsilon = 0.f;
    }
    const float dx = ax - bx;
    const float dy = ay - by;
    const float dz = az - bz;
    return (dx * dx + dy * dy + dz * dz) <= (epsilon * epsilon);
}

} // namespace fuse::ecs
