#pragma once

#include <cmath>

namespace fuse::renderer {

/// True when `weight` is finite and lies in [0, 1].
[[nodiscard]] inline bool layer_blend_ok(float weight) {
    return std::isfinite(weight) && weight >= 0.f && weight <= 1.f;
}

} // namespace fuse::renderer
