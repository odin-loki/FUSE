#pragma once

#include <cstdint>

namespace fuse::world_partition {

/// Packs the low 32 bits of each XZ cell coordinate. `x` occupies the high half and `z` the low
/// half, using each coordinate's two's-complement bit pattern.
[[nodiscard]] inline std::uint64_t cell_key(int x, int z) {
    const auto x_bits = static_cast<std::uint32_t>(x);
    const auto z_bits = static_cast<std::uint32_t>(z);
    return (static_cast<std::uint64_t>(x_bits) << 32) | static_cast<std::uint64_t>(z_bits);
}

} // namespace fuse::world_partition
