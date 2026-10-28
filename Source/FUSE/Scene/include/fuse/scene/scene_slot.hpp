#pragma once

namespace fuse::scene {

/// True when `slot` is in the inclusive range 0..31.
[[nodiscard]] inline bool scene_slot_ok(int slot) {
    return slot >= 0 && slot <= 31;
}

} // namespace fuse::scene
