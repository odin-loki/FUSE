#pragma once

#include <cstdint>

namespace fuse::physics {

/// True when bytes is in 1 .. 64 MiB inclusive.
[[nodiscard]] inline bool resident_bytes_ok(std::uint32_t bytes) {
    constexpr std::uint32_t kMaxResidentBytes = 64u * 1024u * 1024u;
    return bytes >= 1u && bytes <= kMaxResidentBytes;
}

} // namespace fuse::physics
