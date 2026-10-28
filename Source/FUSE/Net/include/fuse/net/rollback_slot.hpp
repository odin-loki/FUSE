#pragma once

namespace fuse::net {

/// True only for slots 0 through 63 inclusive.
[[nodiscard]] inline bool rollback_slot_ok(int slot) noexcept {
    return slot >= 0 && slot <= 63;
}

} // namespace fuse::net
