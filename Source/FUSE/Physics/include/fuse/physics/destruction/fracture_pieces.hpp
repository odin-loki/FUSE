#pragma once

namespace fuse::physics {

/// True when a fracture piece count is in the inclusive range 2..256.
[[nodiscard]] inline bool fracture_pieces_ok(int count) {
    return count >= 2 && count <= 256;
}

} // namespace fuse::physics
