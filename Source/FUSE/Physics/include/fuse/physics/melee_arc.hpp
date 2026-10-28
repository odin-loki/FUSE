#pragma once

#include <cmath>

namespace fuse::physics {

/// True when degrees is finite and in (0, 180].
[[nodiscard]] inline bool melee_arc_ok(float degrees) {
    return std::isfinite(degrees) && degrees > 0.f && degrees <= 180.f;
}

} // namespace fuse::physics
