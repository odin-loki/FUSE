#pragma once

#include <cmath>

namespace fuse::terrain {

/// True when `radius` is finite and lies in the closed interval [1, 64].
[[nodiscard]] inline bool cave_radius_ok(float radius) {
    return std::isfinite(radius) && radius >= 1.f && radius <= 64.f;
}

} // namespace fuse::terrain
