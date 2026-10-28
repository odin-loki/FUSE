#pragma once

#include <cstdint>

namespace fuse::hybrid {

/// Next frame-barrier generation. Increments by one and saturates at `UINT64_MAX`.
[[nodiscard]] inline constexpr std::uint64_t next_barrier_generation(std::uint64_t generation) noexcept {
    if (generation == UINT64_MAX) {
        return UINT64_MAX;
    }
    return generation + 1ull;
}

} // namespace fuse::hybrid
