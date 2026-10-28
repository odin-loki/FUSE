#pragma once

#include <cmath>

namespace fuse::physics {

/// True when dt is finite and within 1e-5 of 1/30, 1/60, or 1/120.
[[nodiscard]] inline bool fixed_dt_ok(float dt) {
    if (!std::isfinite(dt)) {
        return false;
    }
    constexpr float kTol = 1e-5f;
    constexpr float kTargets[] = {1.f / 30.f, 1.f / 60.f, 1.f / 120.f};
    for (const float target : kTargets) {
        if (std::fabs(dt - target) <= kTol) {
            return true;
        }
    }
    return false;
}

} // namespace fuse::physics
