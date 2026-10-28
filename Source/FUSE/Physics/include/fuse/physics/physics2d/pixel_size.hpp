#pragma once

#include <cmath>

namespace fuse::physics {

[[nodiscard]] inline bool pixel_size_ok(float metersPerPixel) {
    return std::isfinite(metersPerPixel) && metersPerPixel >= 0.001f && metersPerPixel <= 1.f;
}

} // namespace fuse::physics
