#pragma once

// Stack merge. stacks_can_merge is true when both stacks are the same item,
// both counts are non-negative, maxCount is positive, and the combined count
// fits in that max.

#include <cstdint>

namespace fuse::adventure {

[[nodiscard]] inline bool stacks_can_merge(std::uint32_t itemA, std::uint32_t itemB, int countA,
                                            int countB, int maxCount) {
    if (itemA != itemB || countA < 0 || countB < 0 || !(maxCount > 0)) {
        return false;
    }
    const long long combined = static_cast<long long>(countA) + static_cast<long long>(countB);
    return combined <= static_cast<long long>(maxCount);
}

} // namespace fuse::adventure
