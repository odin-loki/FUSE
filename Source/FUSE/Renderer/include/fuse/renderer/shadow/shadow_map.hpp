#pragma once

namespace fuse::renderer {

/// True only when `px` is a power of two in the closed interval [256, 4096].
[[nodiscard]] inline bool shadow_map_ok(int px) {
    if (px < 256 || px > 4096) {
        return false;
    }
    return (px & (px - 1)) == 0;
}

} // namespace fuse::renderer
