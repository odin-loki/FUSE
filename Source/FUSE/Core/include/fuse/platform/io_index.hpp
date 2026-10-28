#pragma once

namespace fuse::platform {

/// True when `capacity` is in 1..4096 and `index` is in [0, capacity).
[[nodiscard]] inline bool io_index_ok(int index, int capacity) {
    return capacity >= 1 && capacity <= 4096 && index >= 0 && index < capacity;
}

} // namespace fuse::platform
