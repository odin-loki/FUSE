#pragma once

namespace fuse::world2d {

/// Row-major tile index `y * width + x`.
/// Returns -1 unless `width` is in 1..4096 and `x` and `y` are each in 0..width-1.
[[nodiscard]] inline int tile_coord(int x, int y, int width) {
    if (width < 1 || width > 4096 || x < 0 || y < 0 || x >= width || y >= width) {
        return -1;
    }
    return y * width + x;
}

} // namespace fuse::world2d
