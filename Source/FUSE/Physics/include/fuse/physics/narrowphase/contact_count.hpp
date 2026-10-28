#pragma once

namespace fuse::physics {

/// True when a contact count is in the inclusive range 0..64.
[[nodiscard]] inline bool contact_count_ok(int count) {
    return count >= 0 && count <= 64;
}

} // namespace fuse::physics
