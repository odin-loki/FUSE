#pragma once

#include <cmath>

namespace fuse::physics {

/// True when margin is finite and in (0, 1].
[[nodiscard]] inline bool ccd_margin_ok(float margin) {
    return std::isfinite(margin) && margin > 0.f && margin <= 1.f;
}

} // namespace fuse::physics
