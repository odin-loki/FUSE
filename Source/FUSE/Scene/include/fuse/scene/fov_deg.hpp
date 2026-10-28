#pragma once

#include <cmath>

namespace fuse::scene {

[[nodiscard]] inline bool fov_deg_ok(float degrees) {
    return std::isfinite(degrees) && degrees >= 20.f && degrees <= 120.f;
}

} // namespace fuse::scene
