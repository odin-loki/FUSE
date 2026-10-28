#pragma once

#include <cstdint>

namespace fuse::world3d {

/// True when `depth` is in `[0, maxDepth]` and `maxDepth` is non-negative.
[[nodiscard]] inline bool hierarchy_depth_ok(int depth, int maxDepth) {
    return depth >= 0 && depth <= maxDepth && maxDepth >= 0;
}

/// Child depth under `parentDepth`, or -1 when that depth cannot advance inside int32.
[[nodiscard]] inline int hierarchy_child_depth(int parentDepth) {
    if (parentDepth >= 0 && parentDepth < INT32_MAX) {
        return parentDepth + 1;
    }
    return -1;
}

} // namespace fuse::world3d
