#pragma once

#include <cmath>

namespace fuse::mechanics {

/// True when margin is finite and in the inclusive range 0..2.
[[nodiscard]] inline bool leaf_margin_ok(float margin) {
    return std::isfinite(margin) && margin >= 0.f && margin <= 2.f;
}

} // namespace fuse::mechanics
