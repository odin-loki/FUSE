#pragma once

#include <cmath>

namespace fuse::renderer {

/// True when `weight` is finite and in the inclusive range [0, 1].
[[nodiscard]] inline bool layer_weight_ok(float weight) {
    return std::isfinite(weight) && weight >= 0.f && weight <= 1.f;
}

} // namespace fuse::renderer
