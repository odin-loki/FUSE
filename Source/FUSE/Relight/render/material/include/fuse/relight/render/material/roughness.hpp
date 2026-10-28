#pragma once

#include <cmath>

namespace fuse::relight {

/// True when `roughness` is finite and in [0, 1].
[[nodiscard]] inline bool roughness_ok(float roughness) {
    return std::isfinite(roughness) && roughness >= 0.f && roughness <= 1.f;
}

} // namespace fuse::relight
