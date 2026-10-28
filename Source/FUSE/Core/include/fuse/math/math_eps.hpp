#pragma once

#include <cmath>

namespace fuse::math {

/// True when `eps` is finite and in (0, 0.001]. Zero and negatives are false.
[[nodiscard]] inline bool math_eps_ok(float eps) {
    return std::isfinite(eps) && eps > 0.f && eps <= 0.001f;
}

} // namespace fuse::math
