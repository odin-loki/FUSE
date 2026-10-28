#pragma once

namespace fuse::world3d {

/// World-space origin of `chunk` when each chunk is `chunkSize` units wide.
/// `chunkSize <= 0` returns 0. Otherwise `chunk * chunkSize`.
[[nodiscard]] inline int chunk_origin(int chunk, int chunkSize) {
    if (chunkSize <= 0) {
        return 0;
    }
    return chunk * chunkSize;
}

} // namespace fuse::world3d
