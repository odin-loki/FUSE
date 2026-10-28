#pragma once

namespace fuse::renderer {

/// True only when `rays` is a power of two in [4, 64]. Zero is false.
[[nodiscard]] inline bool cascade_rays_ok(int rays) {
    return rays >= 4 && rays <= 64 && (rays & (rays - 1)) == 0;
}

} // namespace fuse::renderer
