#pragma once

#include <cstdint>

namespace fuse::net {

/// True when `used + incoming` is within `budget` and the addition does not wrap uint32.
/// A budget of 0 fits only when `used` and `incoming` are both 0.
[[nodiscard]] inline bool packet_fits(std::uint32_t used, std::uint32_t incoming, std::uint32_t budget) noexcept {
    if (incoming > ~std::uint32_t{0} - used) {
        return false;
    }
    return used + incoming <= budget;
}

} // namespace fuse::net
