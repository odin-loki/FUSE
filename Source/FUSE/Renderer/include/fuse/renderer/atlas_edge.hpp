#pragma once

namespace fuse::renderer {

/// True only when `px` is a power of two in [8, 256]. Zero is false.
[[nodiscard]] inline bool atlas_edge_ok(int px) {
    return px >= 8 && px <= 256 && (px & (px - 1)) == 0;
}

} // namespace fuse::renderer
