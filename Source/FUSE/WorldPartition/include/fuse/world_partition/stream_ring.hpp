#pragma once

namespace fuse::world_partition {

/// True when `ring` is in [0, 8].
[[nodiscard]] inline bool stream_ring_ok(int ring) {
    return ring >= 0 && ring <= 8;
}

} // namespace fuse::world_partition
