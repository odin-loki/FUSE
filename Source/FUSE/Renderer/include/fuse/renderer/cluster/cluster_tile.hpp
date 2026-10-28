#pragma once

namespace fuse::renderer {

/// True only when `px` is a power of two in the closed interval [8, 32].
[[nodiscard]] inline bool cluster_tile_ok(int px) {
    if (px < 8 || px > 32) {
        return false;
    }
    return (px & (px - 1)) == 0;
}

} // namespace fuse::renderer
