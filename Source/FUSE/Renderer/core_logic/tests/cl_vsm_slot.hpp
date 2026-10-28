#pragma once

namespace fuse::renderer {

/// True only for slots 0 through 31 inclusive.
[[nodiscard]] inline bool cl_vsm_slot_ok(int slot) {
    return slot >= 0 && slot <= 31;
}

} // namespace fuse::renderer
