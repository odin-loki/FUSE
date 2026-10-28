#pragma once

#include <cmath>

namespace fuse::renderer {

/// True when `radius` is finite and in the inclusive range 0..1.
[[nodiscard]] inline bool bloom_radius_ok(float radius) {
    return std::isfinite(radius) && radius >= 0.f && radius <= 1.f;
}

} // namespace fuse::renderer
