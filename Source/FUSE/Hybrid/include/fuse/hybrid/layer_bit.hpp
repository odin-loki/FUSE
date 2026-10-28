#pragma once

#include <cstdint>

namespace fuse::hybrid {

/// True when `bit` is a single power of two and that bit is set in `mask`.
[[nodiscard]] inline bool layer_bit_visible(std::uint32_t mask, std::uint32_t bit) {
    return bit != 0u && (bit & (bit - 1u)) == 0u && (mask & bit) != 0u;
}

} // namespace fuse::hybrid
