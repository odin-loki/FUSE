#pragma once

namespace fuse::mechanics {

/// True when a toggle id is in the inclusive range 0..255.
[[nodiscard]] inline bool toggle_id_ok(int id) {
    return id >= 0 && id <= 255;
}

} // namespace fuse::mechanics
