#pragma once

#include <cmath>

namespace fuse::world3d {

/// True when `degrees` is finite and in [-180, 180].
[[nodiscard]] inline bool spawn_yaw_ok(float degrees) {
    return std::isfinite(degrees) && degrees >= -180.f && degrees <= 180.f;
}

} // namespace fuse::world3d
