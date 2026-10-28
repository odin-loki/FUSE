#pragma once

#include <cmath>

namespace fuse::renderer {

/// True when `ratio` is finite and within 1e-3 of 1, 1.5, 2, or 3.
[[nodiscard]] inline bool upscale_ratio_ok(float ratio) {
    if (!std::isfinite(ratio)) {
        return false;
    }
    const double value = static_cast<double>(ratio);
    constexpr double kEps = 1e-3;
    return std::fabs(value - 1.0) <= kEps || std::fabs(value - 1.5) <= kEps ||
           std::fabs(value - 2.0) <= kEps || std::fabs(value - 3.0) <= kEps;
}

} // namespace fuse::renderer
