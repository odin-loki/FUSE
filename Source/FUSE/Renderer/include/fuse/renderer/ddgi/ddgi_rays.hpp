#pragma once

namespace fuse::renderer {

/// True when `count` is a power of two in the closed interval [1, 256].
[[nodiscard]] inline bool ddgi_rays_ok(int count) {
    if (count < 1 || count > 256) {
        return false;
    }
    return (count & (count - 1)) == 0;
}

} // namespace fuse::renderer
