#pragma once

#include <cmath>

namespace fuse::renderer {

/// True when `meters` is finite and in (0, 10000].
[[nodiscard]] inline bool cull_distance_ok(float meters) {
    return std::isfinite(meters) && meters > 0.f && meters <= 10000.f;
}

} // namespace fuse::renderer
