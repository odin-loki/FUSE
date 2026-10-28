#pragma once

namespace fuse::physics {

[[nodiscard]] inline bool world_cell_ok(int meters) {
    if (meters < 16 || meters > 1024) {
        return false;
    }
    return (meters & (meters - 1)) == 0;
}

} // namespace fuse::physics
