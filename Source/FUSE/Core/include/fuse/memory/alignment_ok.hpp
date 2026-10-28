#pragma once

#include <cstdint>

namespace fuse::memory {

/// True when `align` is a power of two in [1, 4096].
[[nodiscard]] inline bool alignment_ok(std::uint32_t align) {
    const bool power_of_two = align != 0u && (align & (align - 1u)) == 0u;
    return power_of_two && align >= 1u && align <= 4096u;
}

} // namespace fuse::memory
