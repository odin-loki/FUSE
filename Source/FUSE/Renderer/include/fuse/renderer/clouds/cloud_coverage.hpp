#pragma once

#include <cmath>

namespace fuse::renderer {

/// True when `coverage` is finite and in the inclusive range [0, 1].
[[nodiscard]] inline bool cloud_coverage_ok(float coverage) {
    return std::isfinite(coverage) && coverage >= 0.f && coverage <= 1.f;
}

} // namespace fuse::renderer
