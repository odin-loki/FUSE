#pragma once

#include <cmath>

namespace fuse::physics {

/// Impulse as mass times dv. Non-finite mass or dv, or non-positive mass, returns 0.
[[nodiscard]] inline float impulse_scale(float mass, float dv) {
    if (!std::isfinite(mass) || mass <= 0.f || !std::isfinite(dv)) {
        return 0.f;
    }
    return mass * dv;
}

} // namespace fuse::physics
