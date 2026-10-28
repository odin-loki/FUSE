#pragma once

#include <cstdint>

namespace fuse::net {

/// Ring slot for `frame` in a buffer of `capacity` (0 when `capacity` is 0).
[[nodiscard]] inline std::uint32_t buffer_index(std::uint32_t frame, std::uint32_t capacity) noexcept {
    if (capacity == 0) {
        return 0;
    }
    return frame % capacity;
}

} // namespace fuse::net
