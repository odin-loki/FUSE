#pragma once

#include <cstdint>

namespace fuse::platform {

/// True when `ms` is at most one day (86400000 ms).
[[nodiscard]] inline bool clock_ms_ok(std::uint64_t ms) {
    return ms <= 86400000ull;
}

} // namespace fuse::platform
