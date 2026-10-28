#pragma once

#include <cmath>

namespace fuse::ai {

/// Prefer `a` when it is finite, otherwise `b` when it is finite, otherwise 0.
[[nodiscard]] inline float utility_score(float a, float b) {
    if (std::isfinite(a)) {
        return a;
    }
    if (std::isfinite(b)) {
        return b;
    }
    return 0.f;
}

} // namespace fuse::ai
