#pragma once

#include <cstdint>

namespace fuse::memory {

/// True when `bytes` is a power of two in [256, 67108864].
[[nodiscard]] inline bool gpu_block_ok(std::uint64_t bytes) {
    const bool power_of_two = bytes != 0u && (bytes & (bytes - 1u)) == 0u;
    return power_of_two && bytes >= 256u && bytes <= 67108864u;
}

} // namespace fuse::memory
