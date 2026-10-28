#pragma once

#include <cmath>

namespace fuse::renderer {

/// True when `scale` is finite and in the inclusive range 0.5..1.
[[nodiscard]] inline bool taau_scale_ok(float scale) {
    return std::isfinite(scale) && scale >= 0.5f && scale <= 1.f;
}

} // namespace fuse::renderer
