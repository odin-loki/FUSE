#pragma once

namespace fuse::renderer {

/// True only when `px` is a power of two in [256, 4096]. Zero is false.
[[nodiscard]] inline bool shadow_res_ok(int px) {
    return px >= 256 && px <= 4096 && (px & (px - 1)) == 0;
}

} // namespace fuse::renderer
