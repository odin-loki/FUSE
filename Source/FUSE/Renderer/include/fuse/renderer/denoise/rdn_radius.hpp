#pragma once

namespace fuse::renderer {

/// True when `radius` is in the inclusive range 1..8.
[[nodiscard]] inline bool rdn_radius_ok(int radius) {
    return radius >= 1 && radius <= 8;
}

} // namespace fuse::renderer
