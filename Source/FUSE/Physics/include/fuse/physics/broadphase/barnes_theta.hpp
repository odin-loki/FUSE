#pragma once

#include <cmath>

namespace fuse::physics {

/// True when theta is finite and in (0, 1].
[[nodiscard]] inline bool barnes_theta_ok(float theta) {
    return std::isfinite(theta) && theta > 0.f && theta <= 1.f;
}

} // namespace fuse::physics
