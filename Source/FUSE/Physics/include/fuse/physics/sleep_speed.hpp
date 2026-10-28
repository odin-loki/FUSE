#pragma once

#include <cmath>

namespace fuse::physics {

/// True when speed is finite and in [0, 1].
[[nodiscard]] inline bool sleep_speed_ok(float speed) {
    return std::isfinite(speed) && speed >= 0.f && speed <= 1.f;
}

} // namespace fuse::physics
