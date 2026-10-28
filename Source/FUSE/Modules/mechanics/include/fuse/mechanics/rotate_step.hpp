#pragma once

#include <cmath>

namespace fuse::mechanics {

/// Advance `angleDegrees` by `degreesPerSec * dt` and wrap into [0, 360).
/// Non-finite inputs return `angleDegrees` when that angle is finite, otherwise 0.
/// A negative `dt` counts as 0.
[[nodiscard]] inline float rotate_step(float angleDegrees, float degreesPerSec, float dt) {
    if (!std::isfinite(angleDegrees) || !std::isfinite(degreesPerSec) || !std::isfinite(dt)) {
        if (std::isfinite(angleDegrees)) {
            return angleDegrees;
        }
        return 0.f;
    }
    if (dt < 0.f) {
        dt = 0.f;
    }

    const float stepped = angleDegrees + degreesPerSec * dt;
    if (!std::isfinite(stepped)) {
        return angleDegrees;
    }

    constexpr float kFullTurn = 360.f;
    float wrapped = std::fmod(stepped, kFullTurn);
    if (wrapped < 0.f) {
        wrapped += kFullTurn;
    }
    if (!std::isfinite(wrapped) || wrapped < 0.f || wrapped >= kFullTurn) {
        return 0.f;
    }
    return wrapped;
}

} // namespace fuse::mechanics
