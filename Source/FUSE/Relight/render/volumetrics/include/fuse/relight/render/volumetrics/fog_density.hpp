// Fog density is valid when it is finite and lies in [0, 1].
#pragma once

#include <cmath>

namespace fuse::relight {

/// True when `density` is finite and in [0, 1].
[[nodiscard]] inline bool fog_density_ok(float density) {
    return std::isfinite(density) && density >= 0.f && density <= 1.f;
}

} // namespace fuse::relight
