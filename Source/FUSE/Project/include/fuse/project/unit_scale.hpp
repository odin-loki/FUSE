#pragma once

#include <cmath>

namespace fuse::project {

/// Meters per world unit. Non-finite or non-positive input returns 1.
[[nodiscard]] inline float unit_scale(float metersPerUnit) {
    if (!(metersPerUnit > 0.f) || !std::isfinite(metersPerUnit)) {
        return 1.f;
    }
    return metersPerUnit;
}

} // namespace fuse::project
