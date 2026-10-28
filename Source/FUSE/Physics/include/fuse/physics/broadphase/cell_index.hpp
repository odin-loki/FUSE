#pragma once

#include <cmath>

namespace fuse::physics {

[[nodiscard]] inline int broadphase_cell(float position, float cellSize) {
    if (!std::isfinite(position) || !std::isfinite(cellSize) || !(cellSize > 0.f)) {
        return 0;
    }
    return static_cast<int>(std::floor(position / cellSize));
}

} // namespace fuse::physics
