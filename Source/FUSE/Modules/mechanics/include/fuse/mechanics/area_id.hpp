#pragma once

namespace fuse::mechanics {

/// True when an area id is in the inclusive range 0..1023.
[[nodiscard]] inline bool area_id_ok(int id) {
    return id >= 0 && id <= 1023;
}

} // namespace fuse::mechanics
