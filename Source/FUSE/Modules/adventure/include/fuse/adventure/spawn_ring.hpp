#pragma once

namespace fuse::adventure {

/// True when a spawn ring is in the inclusive range 1..16.
[[nodiscard]] inline bool spawn_ring_ok(int ring) {
    return ring >= 1 && ring <= 16;
}

} // namespace fuse::adventure
