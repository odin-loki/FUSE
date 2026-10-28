#pragma once

#include <cmath>

namespace fuse::net {

/// True when `radius` is finite and in (0, 256].
[[nodiscard]] inline bool interest_radius_ok(float radius) noexcept {
    return std::isfinite(radius) && radius > 0.f && radius <= 256.f;
}

} // namespace fuse::net
