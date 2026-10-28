#pragma once

namespace fuse::physics {

/// True when a convex hull vertex count is in the inclusive range 4..64.
[[nodiscard]] inline bool hull_vertices_ok(int count) {
    return count >= 4 && count <= 64;
}

} // namespace fuse::physics
