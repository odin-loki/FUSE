#pragma once

#include <cstdint>

namespace fuse::ecs {

/// True when bit `bit` of `flags` is set. A bit outside 0..7 returns false.
[[nodiscard]] inline bool transform_dirty(std::uint8_t flags, std::uint8_t bit) {
    if (bit > 7u) {
        return false;
    }
    const std::uint8_t mask = static_cast<std::uint8_t>(1u << bit);
    return (flags & mask) != 0u;
}

} // namespace fuse::ecs
