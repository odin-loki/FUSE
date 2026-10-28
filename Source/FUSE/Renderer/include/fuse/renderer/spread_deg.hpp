#pragma once

#include <cmath>

namespace fuse::renderer {

/// True only when `degrees` is finite and in [0, 45].
[[nodiscard]] inline bool spread_deg_ok(float degrees) {
    return std::isfinite(degrees) && degrees >= 0.f && degrees <= 45.f;
}

} // namespace fuse::renderer
