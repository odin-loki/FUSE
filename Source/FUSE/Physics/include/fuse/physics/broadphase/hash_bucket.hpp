#pragma once

#include <cstdint>

namespace fuse::physics {

[[nodiscard]] inline std::uint32_t hash_bucket(int x, int y, int z, std::uint32_t bucketCount) {
    if (bucketCount == 0) {
        return 0;
    }
    const auto ux = static_cast<std::uint32_t>(x);
    const auto uy = static_cast<std::uint32_t>(y);
    const auto uz = static_cast<std::uint32_t>(z);
    const std::uint32_t mixed = ux * 73856093u ^ uy * 19349663u ^ uz * 83492791u;
    return mixed % bucketCount;
}

} // namespace fuse::physics
