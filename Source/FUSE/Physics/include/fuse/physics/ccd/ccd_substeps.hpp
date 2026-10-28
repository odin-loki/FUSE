#pragma once

namespace fuse::physics {

/// True when a CCD substep count is in the inclusive range 1..8.
[[nodiscard]] inline bool ccd_substeps_ok(int count) {
    return count >= 1 && count <= 8;
}

} // namespace fuse::physics
