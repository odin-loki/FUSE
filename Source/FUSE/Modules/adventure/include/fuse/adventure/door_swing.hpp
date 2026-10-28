#pragma once

#include <cmath>

namespace fuse::adventure {

/// Signed door swing in degrees from a normalized open amount.
/// `openAmount` is clamped to [0, 1]. Negative `maxDegrees` is treated as 0.
/// The result is `openAmount * maxDegrees`, negated when `inward`.
/// A non-finite `openAmount` returns 0.
[[nodiscard]] inline float door_swing_angle(float openAmount, float maxDegrees, bool inward) {
    if (!std::isfinite(openAmount)) {
        return 0.f;
    }
    if (openAmount < 0.f) {
        openAmount = 0.f;
    } else if (openAmount > 1.f) {
        openAmount = 1.f;
    }
    if (maxDegrees < 0.f) {
        maxDegrees = 0.f;
    }
    const float magnitude = openAmount * maxDegrees;
    return inward ? -magnitude : magnitude;
}

} // namespace fuse::adventure
