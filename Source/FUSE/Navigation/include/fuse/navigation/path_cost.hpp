#pragma once

#include <cmath>

namespace fuse::navigation {

/// True when `cost` is finite and in [0, 10000].
[[nodiscard]] inline bool path_cost_ok(float cost) {
    return std::isfinite(cost) && cost >= 0.f && cost <= 10000.f;
}

} // namespace fuse::navigation
