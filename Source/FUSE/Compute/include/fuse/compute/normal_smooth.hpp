#pragma once

#include <cmath>

namespace fuse::compute {

/// True when the dot product of the two vectors is >= `minDot`.
/// Neither vector is normalized, and `minDot` is not clamped.
/// Any non-finite component returns false.
[[nodiscard]] inline bool normals_smooth(float nx, float ny, float nz, float mx, float my, float mz,
                                          float minDot) {
    if (!std::isfinite(nx) || !std::isfinite(ny) || !std::isfinite(nz) || !std::isfinite(mx) ||
        !std::isfinite(my) || !std::isfinite(mz)) {
        return false;
    }
    const float dot = nx * mx + ny * my + nz * mz;
    return dot >= minDot;
}

} // namespace fuse::compute
