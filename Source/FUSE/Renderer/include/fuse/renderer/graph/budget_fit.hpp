#pragma once

#include <cstdint>

namespace fuse::renderer::graph {

/// True when `used + incoming` is within `budget` and the addition does not wrap.
/// A budget of 0 fits only when `used` and `incoming` are both 0.
[[nodiscard]] inline bool budget_fits(std::uint64_t used, std::uint64_t incoming, std::uint64_t budget) {
    if (incoming > ~std::uint64_t{0} - used) {
        return false;
    }
    return used + incoming <= budget;
}

/// Bytes still available under `budget`. Zero when `used` is already at or past the cap.
[[nodiscard]] inline std::uint64_t budget_remain(std::uint64_t used, std::uint64_t budget) {
    if (used >= budget) {
        return 0;
    }
    return budget - used;
}

} // namespace fuse::renderer::graph
