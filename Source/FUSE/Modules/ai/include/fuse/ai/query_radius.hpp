#pragma once

#include <cmath>

namespace fuse::ai {

/// True when `radius` is finite and in (0, 128].
[[nodiscard]] inline bool query_radius_ok(float radius) {
    return std::isfinite(radius) && radius > 0.f && radius <= 128.f;
}

} // namespace fuse::ai
