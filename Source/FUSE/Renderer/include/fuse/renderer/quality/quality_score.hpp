#pragma once

#include <cmath>

namespace fuse::renderer {

/// True when `score` is finite and in [0, 1].
[[nodiscard]] inline bool quality_score_ok(float score) {
    return std::isfinite(score) && score >= 0.f && score <= 1.f;
}

} // namespace fuse::renderer
