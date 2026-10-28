#pragma once

#include <cmath>

namespace fuse::physics {

[[nodiscard]] inline bool toi_in_range(float toi) {
    return std::isfinite(toi) && toi >= 0.f && toi <= 1.f;
}

} // namespace fuse::physics
