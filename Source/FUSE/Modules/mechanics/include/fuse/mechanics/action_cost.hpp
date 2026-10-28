#pragma once

#include <cmath>

namespace fuse::mechanics {

/// True when `cost` is finite and in [0, 100].
[[nodiscard]] inline bool action_cost_ok(float cost) {
    return std::isfinite(cost) && cost >= 0.f && cost <= 100.f;
}

} // namespace fuse::mechanics
