#pragma once

#include <cmath>

namespace fuse::mechanics {

/// True when `radius` is finite and in (0, 1000].
[[nodiscard]] inline bool zone_radius_ok(float radius) {
    return std::isfinite(radius) && radius > 0.f && radius <= 1000.f;
}

} // namespace fuse::mechanics
