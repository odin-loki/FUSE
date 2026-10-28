#pragma once

namespace fuse::scene {

[[nodiscard]] inline bool bind_slot_ok(int slot) {
    return slot >= 0 && slot <= 15;
}

} // namespace fuse::scene
