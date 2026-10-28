#pragma once

#include <cmath>

namespace fuse::physics {

/// True when length is finite and in (0, 1].
[[nodiscard]] inline bool softening_ok(float length) {
    return std::isfinite(length) && length > 0.f && length <= 1.f;
}

} // namespace fuse::physics
