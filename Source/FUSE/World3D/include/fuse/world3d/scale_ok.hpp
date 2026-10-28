#pragma once

#include <cmath>

namespace fuse::world3d {

[[nodiscard]] inline bool scale_ok(float scale) {
    return std::isfinite(scale) && scale > 0.f && scale <= 100.f;
}

} // namespace fuse::world3d
