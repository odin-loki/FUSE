#pragma once

namespace fuse::renderer {

/// True only for slots 0 through 7 inclusive.
[[nodiscard]] inline bool plugin_slot_ok(int slot) {
    return slot >= 0 && slot <= 7;
}

} // namespace fuse::renderer
