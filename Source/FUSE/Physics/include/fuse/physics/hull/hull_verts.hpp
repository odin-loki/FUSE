#pragma once

namespace fuse::physics {

/// True when a convex hull vertex count is in the inclusive range 4..256.
[[nodiscard]] inline bool hull_verts_ok(int count) {
    return count >= 4 && count <= 256;
}

} // namespace fuse::physics
