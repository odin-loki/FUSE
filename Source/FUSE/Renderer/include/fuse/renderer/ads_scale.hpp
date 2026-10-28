#pragma once

#include <cmath>

namespace fuse::renderer {

/// True when `scale` is finite and in (0, 1].
[[nodiscard]] inline bool ads_scale_ok(float scale) {
    return std::isfinite(scale) && scale > 0.f && scale <= 1.f;
}

} // namespace fuse::renderer
