#pragma once

#include <cmath>

namespace fuse::physics {

/// True when bias is finite and in (0, 1].
[[nodiscard]] inline bool contact_bias_ok(float bias) {
    return std::isfinite(bias) && bias > 0.f && bias <= 1.f;
}

} // namespace fuse::physics
