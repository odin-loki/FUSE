#pragma once

namespace fuse::adventure {

/// True when `capacity` is positive and `size` lies in `0 .. capacity - 1`.
[[nodiscard]] inline bool interact_queue_accept(int size, int capacity) {
    return capacity > 0 && size >= 0 && size < capacity;
}

/// Entries that would be dropped so `size` fits in `capacity`.
/// Zero when `size` is within capacity, `capacity` is non-positive, or `size` is negative.
[[nodiscard]] inline int interact_queue_overflow(int size, int capacity) {
    if (size < 0 || capacity <= 0 || size <= capacity) {
        return 0;
    }
    return size - capacity;
}

} // namespace fuse::adventure
