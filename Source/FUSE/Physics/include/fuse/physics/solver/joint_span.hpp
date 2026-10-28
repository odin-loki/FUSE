#pragma once

#include <cmath>

namespace fuse::physics {

[[nodiscard]] inline bool joint_span_ok(float minAngle, float maxAngle) {
    if (!std::isfinite(minAngle) || !std::isfinite(maxAngle)) {
        return false;
    }
    constexpr float kPi = 3.14159265358979323846f;
    if (minAngle < -kPi || minAngle > kPi || maxAngle < -kPi || maxAngle > kPi) {
        return false;
    }
    return minAngle <= maxAngle;
}

} // namespace fuse::physics
