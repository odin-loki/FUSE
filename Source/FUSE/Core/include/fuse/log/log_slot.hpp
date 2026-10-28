#pragma once

namespace fuse::log {

/// True when `slot` is in [0, 255].
[[nodiscard]] inline bool log_slot_ok(int slot) {
    return slot >= 0 && slot <= 255;
}

} // namespace fuse::log
