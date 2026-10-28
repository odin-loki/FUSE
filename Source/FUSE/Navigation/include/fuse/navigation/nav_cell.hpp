#pragma once

#include <cmath>

namespace fuse::navigation {

/// True when `meters` is finite and in [0.1, 4].
[[nodiscard]] inline bool nav_cell_ok(float meters) {
    return std::isfinite(meters) && meters >= 0.1f && meters <= 4.f;
}

} // namespace fuse::navigation
