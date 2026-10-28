#pragma once

#include <cstdint>

namespace fuse::mechanics {

/// Order-independent pair id. The smaller id occupies the high 32 bits.
[[nodiscard]] inline std::uint64_t trigger_pair_key(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t lo = a < b ? a : b;
    const std::uint32_t hi = a < b ? b : a;
    return (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint64_t>(hi);
}

} // namespace fuse::mechanics
