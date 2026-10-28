#pragma once

#include <cmath>

namespace fuse::relight {

/// True when \p opacity is finite and in [0, 1].
[[nodiscard]] inline bool overlay_opacity_ok(float opacity) {
    return std::isfinite(opacity) && opacity >= 0.f && opacity <= 1.f;
}

} // namespace fuse::relight
