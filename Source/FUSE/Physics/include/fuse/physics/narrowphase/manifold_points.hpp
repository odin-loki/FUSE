#pragma once

namespace fuse::physics {

[[nodiscard]] inline bool manifold_points_ok(int count) {
    return count >= 1 && count <= 4;
}

} // namespace fuse::physics
