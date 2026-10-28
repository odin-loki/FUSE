#pragma once

#include <cmath>

namespace fuse::animation {

/// True when \p weight is finite and in [0, 1].
[[nodiscard]] inline bool blend_weight_ok(float weight) {
    return std::isfinite(weight) && weight >= 0.f && weight <= 1.f;
}

} // namespace fuse::animation
