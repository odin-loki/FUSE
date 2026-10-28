#pragma once

namespace fuse::renderer {

/// True only for slots 0 through 15 inclusive.
[[nodiscard]] inline bool capture_slot_ok(int slot) {
    return slot >= 0 && slot <= 15;
}

} // namespace fuse::renderer
