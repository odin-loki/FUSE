#pragma once

namespace fuse::physics {

[[nodiscard]] inline bool body_slot_ok(int slot, int capacity) {
    if (capacity < 1 || capacity > 1000000) {
        return false;
    }
    return slot >= 0 && slot < capacity;
}

} // namespace fuse::physics
