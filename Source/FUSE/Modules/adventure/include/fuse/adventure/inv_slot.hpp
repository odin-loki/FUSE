#pragma once

namespace fuse::adventure {

/// True when `slot` is in 0..63.
[[nodiscard]] inline bool inv_slot_ok(int slot) {
    return slot >= 0 && slot <= 63;
}

} // namespace fuse::adventure
