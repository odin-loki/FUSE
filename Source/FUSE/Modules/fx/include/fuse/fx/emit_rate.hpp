#pragma once

#include <cmath>

namespace fuse::fx {

/// True only when \p rate is finite and in (0, 1000].
[[nodiscard]] inline bool emit_rate_ok(float rate) {
    return std::isfinite(rate) && rate > 0.f && rate <= 1000.f;
}

} // namespace fuse::fx
