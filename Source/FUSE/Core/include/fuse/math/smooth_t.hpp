#pragma once

#include <cmath>

namespace fuse::math {

/// True only when `t` is finite and in [0, 1].
[[nodiscard]] inline bool smooth_t_ok(float t) {
    return std::isfinite(t) && t >= 0.f && t <= 1.f;
}

} // namespace fuse::math
