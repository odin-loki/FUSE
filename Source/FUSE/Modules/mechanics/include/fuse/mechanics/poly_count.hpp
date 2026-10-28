#pragma once

namespace fuse::mechanics {

/// True when a polyhedron vertex count is in the inclusive range 3..64.
[[nodiscard]] inline bool poly_vertex_ok(int count) {
    return count >= 3 && count <= 64;
}

} // namespace fuse::mechanics
