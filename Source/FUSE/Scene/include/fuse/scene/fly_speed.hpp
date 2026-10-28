#pragma once

#include <cmath>

namespace fuse::scene {

[[nodiscard]] inline bool fly_speed_ok(float metersPerSecond) {
    return std::isfinite(metersPerSecond) && metersPerSecond >= 0.1f && metersPerSecond <= 100.f;
}

} // namespace fuse::scene
