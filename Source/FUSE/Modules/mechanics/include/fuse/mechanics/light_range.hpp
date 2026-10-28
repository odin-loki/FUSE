#pragma once

#include <cmath>

namespace fuse::mechanics {

/// True when `range` is finite, greater than 0, and at most 10000.
[[nodiscard]] inline bool light_range_ok(float range) {
    return std::isfinite(range) && range > 0.f && range <= 10000.f;
}

} // namespace fuse::mechanics
