#pragma once

#include <cstdint>
#include <limits>

namespace fuse::hybrid {

/// True when `used + incoming` does not wrap and the sum is at most `budget`.
/// A zero budget fits only when both sides are zero.
[[nodiscard]] inline bool alloc_fits(std::uint64_t used, std::uint64_t incoming, std::uint64_t budget) {
    if (incoming > std::numeric_limits<std::uint64_t>::max() - used) {
        return false;
    }
    return used + incoming <= budget;
}

} // namespace fuse::hybrid
