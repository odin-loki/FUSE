#pragma once

#include <cstdint>

namespace fuse::hybrid {

/// True when both dimensions are in 1..16384.
[[nodiscard]] inline bool swapchain_extent_ok(std::uint32_t width, std::uint32_t height) {
    return width >= 1u && width <= 16384u && height >= 1u && height <= 16384u;
}

} // namespace fuse::hybrid
