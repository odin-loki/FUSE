#pragma once

namespace fuse::mechanics {

/// True when a counter capacity is in the inclusive range 1..1000000.
[[nodiscard]] inline bool counter_cap_ok(int cap) {
    return cap >= 1 && cap <= 1000000;
}

} // namespace fuse::mechanics
