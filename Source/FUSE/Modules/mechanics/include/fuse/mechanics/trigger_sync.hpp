#pragma once

namespace fuse::mechanics {

/// True when a trigger sync generation is in the inclusive range 0..15.
[[nodiscard]] inline bool trigger_sync_ok(int generation) {
    return generation >= 0 && generation <= 15;
}

} // namespace fuse::mechanics
