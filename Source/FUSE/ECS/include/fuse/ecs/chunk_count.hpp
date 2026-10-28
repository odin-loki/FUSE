#pragma once

#include <cstdint>

namespace fuse::ecs {

/// Chunks needed to cover `count` items of `chunkSize`.
/// A zero `chunkSize` or `count` returns 0. Otherwise `ceil(count / chunkSize)` as uint32,
/// saturating at `UINT32_MAX`.
[[nodiscard]] inline constexpr std::uint32_t chunk_count(std::uint32_t count, std::uint32_t chunkSize) {
    if (chunkSize == 0u || count == 0u) {
        return 0u;
    }
    const std::uint64_t chunks = (static_cast<std::uint64_t>(count) + static_cast<std::uint64_t>(chunkSize) - 1ull) /
                                 static_cast<std::uint64_t>(chunkSize);
    constexpr std::uint64_t kMax = 0xffffffffull;
    if (chunks > kMax) {
        return static_cast<std::uint32_t>(kMax);
    }
    return static_cast<std::uint32_t>(chunks);
}

} // namespace fuse::ecs
