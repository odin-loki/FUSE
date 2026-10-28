// API slot index: valid when it is in 0..7.
#pragma once

namespace fuse::relight {

/// True only for slots 0 through 7.
[[nodiscard]] inline bool api_slot_ok(int slot) {
    return slot >= 0 && slot <= 7;
}

} // namespace fuse::relight
