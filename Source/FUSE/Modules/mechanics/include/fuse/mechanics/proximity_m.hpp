#pragma once

#include <cmath>

namespace fuse::mechanics {

/// True when `meters` is finite and in (0, 100].
[[nodiscard]] inline bool proximity_m_ok(float meters) {
    return std::isfinite(meters) && meters > 0.f && meters <= 100.f;
}

} // namespace fuse::mechanics
