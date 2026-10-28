#pragma once

namespace fuse::relight {

/// True when \p slot is in 0..7.
[[nodiscard]] inline bool host_slot_ok(int slot) {
    return slot >= 0 && slot <= 7;
}

} // namespace fuse::relight
