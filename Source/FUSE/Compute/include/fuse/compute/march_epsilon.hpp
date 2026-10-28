#pragma once

#include <cmath>

namespace fuse::compute {

/// True when `epsilon` is finite and lies in the closed interval [1e-5, 1e-1].
[[nodiscard]] inline bool march_epsilon_ok(float epsilon) {
    return std::isfinite(epsilon) && epsilon >= 1e-5f && epsilon <= 1e-1f;
}

} // namespace fuse::compute
