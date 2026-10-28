#pragma once

#include <fuse/physics/math.hpp>

#include <cmath>

namespace fuse::physics {

struct HingeLimit {
    f32 minAngle = 0.f;
    f32 maxAngle = 0.f;
};

struct HingeError {
    f32 correction = 0.f;
    bool active = false;
};

/// Angular error that restores a hinge into [minAngle, maxAngle], in radians.
/// If minAngle > maxAngle the bounds are swapped. Inside the closed interval the
/// error is inactive (correction 0). Below min, correction is min - angle (positive).
/// Above max, correction is max - angle (negative). A non-finite angle or limit
/// returns an inactive zero.
[[nodiscard]] inline HingeError hinge_limit_error(f32 angle, HingeLimit limit) {
    if (!std::isfinite(angle) || !std::isfinite(limit.minAngle) || !std::isfinite(limit.maxAngle)) {
        return {};
    }
    if (limit.minAngle > limit.maxAngle) {
        const f32 lo = limit.maxAngle;
        limit.maxAngle = limit.minAngle;
        limit.minAngle = lo;
    }
    if (angle < limit.minAngle) {
        return {limit.minAngle - angle, true};
    }
    if (angle > limit.maxAngle) {
        return {limit.maxAngle - angle, true};
    }
    return {};
}

} // namespace fuse::physics
