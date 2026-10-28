#pragma once

#include <cstdint>

namespace fuse::renderer {

/// Flat clustered-light tile index: `y * tilesX + x`.
/// A zero `tilesX` returns 0. A product or sum past UINT32_MAX returns UINT32_MAX.
[[nodiscard]] inline constexpr std::uint32_t cluster_tile_index(std::uint32_t x, std::uint32_t y,
                                                                std::uint32_t tilesX) {
    if (tilesX == 0u) {
        return 0u;
    }
    const std::uint64_t index =
        static_cast<std::uint64_t>(y) * static_cast<std::uint64_t>(tilesX) + static_cast<std::uint64_t>(x);
    constexpr std::uint64_t kMax = 0xffffffffull;
    if (index > kMax) {
        return static_cast<std::uint32_t>(kMax);
    }
    return static_cast<std::uint32_t>(index);
}

/// True when `x < tilesX` and `y < tilesY`. A zero tile count is not inside.
[[nodiscard]] inline constexpr bool cluster_tile_inside(std::uint32_t x, std::uint32_t y, std::uint32_t tilesX,
                                                        std::uint32_t tilesY) {
    if (tilesX == 0u || tilesY == 0u) {
        return false;
    }
    return x < tilesX && y < tilesY;
}

} // namespace fuse::renderer
