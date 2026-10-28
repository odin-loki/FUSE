#pragma once

#include <cstdint>

namespace fuse::ecs {

/// Byte stride of one column element.
/// `elementSize` 0 returns 0.
/// Alignment 0, or an alignment that is not a power of two, returns `elementSize`.
/// Otherwise rounds `elementSize` up to a multiple of `alignment`.
/// If `elementSize + alignment - 1` would wrap, returns the largest multiple of `alignment` that fits in uint32.
[[nodiscard]] inline std::uint32_t column_stride(std::uint32_t elementSize, std::uint32_t alignment) {
    if (elementSize == 0u) {
        return 0u;
    }
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) {
        return elementSize;
    }
    const std::uint32_t mask = alignment - 1u;
    const std::uint32_t rounded = elementSize + mask;
    if (rounded < elementSize) {
        return ~mask;
    }
    return rounded & ~mask;
}

} // namespace fuse::ecs
