#pragma once

#include <cstdint>

namespace fuse::net {

/// Header plus body byte count, saturating at `UINT32_MAX`.
[[nodiscard]] inline constexpr std::uint32_t payload_size(std::uint32_t headerBytes,
                                                           std::uint32_t bodyBytes) noexcept {
    const std::uint64_t sum =
        static_cast<std::uint64_t>(headerBytes) + static_cast<std::uint64_t>(bodyBytes);
    if (sum > static_cast<std::uint64_t>(UINT32_MAX)) {
        return UINT32_MAX;
    }
    return static_cast<std::uint32_t>(sum);
}

} // namespace fuse::net
