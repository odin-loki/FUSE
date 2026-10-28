#pragma once

#include <cmath>

namespace fuse::script {

/// True when `age` has reached `limit`.
/// Non-finite inputs return false. A non-positive limit returns false.
[[nodiscard]] inline bool self_destroy_due(float age, float limit) {
    if (!std::isfinite(age) || !std::isfinite(limit) || !(limit > 0.f)) {
        return false;
    }
    return age >= limit;
}

} // namespace fuse::script
