#pragma once

namespace fuse::mechanics {

[[nodiscard]] inline bool counter_reached(int value, int target) {
    return value >= target;
}

} // namespace fuse::mechanics
