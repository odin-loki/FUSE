#pragma once

#include <cstdint>

namespace fuse::ecs {

/// Round `value` up to a multiple of `alignment`.
/// Alignment 0, or an alignment that is not a power of two, returns `value`.
/// If `value + alignment - 1` would wrap, returns the largest multiple of `alignment` that fits in uint32.
[[nodiscard]] inline std::uint32_t align_up_u32(std::uint32_t value, std::uint32_t alignment) {
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) {
        return value;
    }
    const std::uint32_t mask = alignment - 1u;
    const std::uint32_t rounded = value + mask;
    if (rounded < value) {
        return ~mask;
    }
    return rounded & ~mask;
}

} // namespace fuse::ecs
