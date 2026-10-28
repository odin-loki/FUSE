#pragma once

#include <cmath>

namespace fuse::renderer {

/// True when `scale` is finite and in the inclusive range 0.5..4.
[[nodiscard]] inline bool neural_scale_ok(float scale) {
    return std::isfinite(scale) && scale >= 0.5f && scale <= 4.f;
}

} // namespace fuse::renderer
