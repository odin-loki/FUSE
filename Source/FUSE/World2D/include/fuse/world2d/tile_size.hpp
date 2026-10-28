#pragma once

namespace fuse::world2d {

/// True when `px` is a power of two in the closed interval [8, 256].
[[nodiscard]] inline bool tile_size_ok(int px) {
    if (px < 8 || px > 256) {
        return false;
    }
    return (px & (px - 1)) == 0;
}

} // namespace fuse::world2d
