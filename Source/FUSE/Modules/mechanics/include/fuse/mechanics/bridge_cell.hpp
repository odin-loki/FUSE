#pragma once

namespace fuse::mechanics {

/// True when a bridge cell is in the inclusive range 1..256.
[[nodiscard]] inline bool bridge_cell_ok(int cell) {
    return cell >= 1 && cell <= 256;
}

} // namespace fuse::mechanics
