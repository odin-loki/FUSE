#pragma once

#include <cmath>

namespace fuse::renderer {

/// True when each channel is finite and in the inclusive range 0..1.
[[nodiscard]] inline bool sky_zenith_ok(float r, float g, float b) {
    return std::isfinite(r) && r >= 0.f && r <= 1.f && std::isfinite(g) && g >= 0.f && g <= 1.f &&
           std::isfinite(b) && b >= 0.f && b <= 1.f;
}

} // namespace fuse::renderer
