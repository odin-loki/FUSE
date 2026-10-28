#pragma once

#include <cmath>

namespace fuse::mechanics {

/// True once `age` has reached the destroy delay.
/// Both inputs must be finite. A negative delay is treated as 0.
[[nodiscard]] inline bool destroy_ready(float age, float delay) {
    if (!std::isfinite(age) || !std::isfinite(delay)) {
        return false;
    }
    if (delay < 0.f) {
        delay = 0.f;
    }
    return age >= delay;
}

/// Advance a destroy timer by `dt` when both values are finite and `dt` is positive.
/// Otherwise returns `age` when `age` is finite, and 0 when it is not.
[[nodiscard]] inline float destroy_age(float age, float dt) {
    if (std::isfinite(age) && std::isfinite(dt) && dt > 0.f) {
        return age + dt;
    }
    if (std::isfinite(age)) {
        return age;
    }
    return 0.f;
}

} // namespace fuse::mechanics
