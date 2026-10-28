#pragma once

#include <cstdint>

namespace fuse::net {

/// Frames to rewind from `current` toward `target`, capped by `maxFrames`.
/// Zero when `current` is at or behind `target`. `maxFrames` is used as given.
[[nodiscard]] inline std::uint32_t rewind_count(std::uint32_t current, std::uint32_t target,
                                                 std::uint32_t maxFrames) noexcept {
    if (current <= target) {
        return 0;
    }
    const std::uint32_t gap = current - target;
    return gap < maxFrames ? gap : maxFrames;
}

} // namespace fuse::net
