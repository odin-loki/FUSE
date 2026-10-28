#pragma once

#include <cmath>

namespace fuse::compute {

/// True when `radius` is finite and lies in (0, 64].
[[nodiscard]] inline bool ss_radius_ok(float radius) {
    return std::isfinite(radius) && radius > 0.f && radius <= 64.f;
}

} // namespace fuse::compute
