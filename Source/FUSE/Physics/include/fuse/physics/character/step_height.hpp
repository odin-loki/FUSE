#pragma once

#include <cmath>

namespace fuse::physics {

[[nodiscard]] inline bool step_height_ok(float height) {
    return std::isfinite(height) && height >= 0.f && height <= 1.f;
}

} // namespace fuse::physics
