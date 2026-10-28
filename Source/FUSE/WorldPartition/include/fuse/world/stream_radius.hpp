#pragma once

#include <cmath>

namespace fuse::world {

/// True when \p meters is finite and in [16, 4096].
[[nodiscard]] inline bool stream_radius_ok(float meters) {
    return std::isfinite(meters) && meters >= 16.f && meters <= 4096.f;
}

} // namespace fuse::world
