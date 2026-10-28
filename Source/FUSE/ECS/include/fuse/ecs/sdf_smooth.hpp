#pragma once

#include <cmath>

namespace fuse::ecs {

/// True when `k` is finite and in [0, 1].
[[nodiscard]] inline bool sdf_smooth_ok(float k) {
    return std::isfinite(k) && k >= 0.f && k <= 1.f;
}

} // namespace fuse::ecs
