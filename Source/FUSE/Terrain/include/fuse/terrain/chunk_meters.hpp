#pragma once

namespace fuse::terrain {

/// True when `meters` is a power of two in the closed interval [16, 256].
[[nodiscard]] inline bool chunk_meters_ok(int meters) {
    if (meters < 16 || meters > 256) {
        return false;
    }
    return (meters & (meters - 1)) == 0;
}

} // namespace fuse::terrain
