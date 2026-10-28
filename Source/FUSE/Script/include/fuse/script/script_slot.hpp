#pragma once

namespace fuse::script {

/// True when `slot` is in the closed range [0, 63].
[[nodiscard]] inline bool script_slot_ok(int slot) {
    return slot >= 0 && slot <= 63;
}

} // namespace fuse::script
