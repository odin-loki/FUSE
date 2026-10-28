#pragma once

namespace fuse::renderer {

/// True when `px` is a power of two in the closed interval [8, 64].
[[nodiscard]] inline bool sw_tile_ok(int px) {
    if (px < 8 || px > 64) {
        return false;
    }
    return (px & (px - 1)) == 0;
}

} // namespace fuse::renderer
