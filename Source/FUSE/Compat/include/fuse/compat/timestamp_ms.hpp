#pragma once

#include <cstdint>

namespace fuse::compat {

/// True when `ms` is in 0..86400000 inclusive (one day in milliseconds).
[[nodiscard]] inline bool timestamp_ms_ok(std::uint32_t ms) {
    return ms <= 86400000u;
}

} // namespace fuse::compat
