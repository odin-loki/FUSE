// Global-illumination spatial sample count. A count is valid only when it lies in [1, 16].
#pragma once

namespace fuse::relight {

/// True when `count` lies in [1, 16].
[[nodiscard]] inline bool gi_spatial_ok(int count) {
    return count >= 1 && count <= 16;
}

} // namespace fuse::relight
