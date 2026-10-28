#pragma once

#include <cmath>

namespace fuse::mechanics {

/// True when `seconds` is finite and in [0, 60].
[[nodiscard]] inline bool destroy_delay_ok(float seconds) {
    return std::isfinite(seconds) && seconds >= 0.f && seconds <= 60.f;
}

} // namespace fuse::mechanics
