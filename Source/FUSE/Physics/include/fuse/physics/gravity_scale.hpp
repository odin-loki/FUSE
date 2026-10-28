#pragma once

#include <cmath>

namespace fuse::physics {

/// True when scale is finite and in [-2, 2].
[[nodiscard]] inline bool gravity_scale_ok(float scale) {
    return std::isfinite(scale) && scale >= -2.f && scale <= 2.f;
}

} // namespace fuse::physics
