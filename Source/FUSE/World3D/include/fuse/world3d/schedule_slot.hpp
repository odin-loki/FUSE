#pragma once

namespace fuse::world3d {

/// True for schedule slots 0 through 15 inclusive.
[[nodiscard]] inline bool schedule_slot_ok(int slot) {
    return slot >= 0 && slot <= 15;
}

} // namespace fuse::world3d
