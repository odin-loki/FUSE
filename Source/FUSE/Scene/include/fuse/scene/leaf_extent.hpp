#pragma once

#include <cmath>

namespace fuse::scene {

[[nodiscard]] inline bool leaf_extent_ok(float meters) {
    return std::isfinite(meters) && meters >= 0.01f && meters <= 8.f;
}

} // namespace fuse::scene
