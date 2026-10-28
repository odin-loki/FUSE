#pragma once

namespace fuse::platform {

/// True when `slot` is in the inclusive range 0..63.
[[nodiscard]] inline bool fiber_slot_ok(int slot) {
    return slot >= 0 && slot <= 63;
}

} // namespace fuse::platform
