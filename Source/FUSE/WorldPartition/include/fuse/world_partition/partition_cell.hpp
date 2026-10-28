#pragma once

namespace fuse::world_partition {

/// True when `cell` is in [0, 1023].
[[nodiscard]] inline bool partition_cell_ok(int cell) {
    return cell >= 0 && cell <= 1023;
}

} // namespace fuse::world_partition
