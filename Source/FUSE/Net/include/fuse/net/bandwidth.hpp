#pragma once

#include <cmath>
#include <cstdint>

namespace fuse::net {

/// Throughput as `(bytes / seconds) / 1000`.
/// Non-finite or non-positive `seconds` returns 0.
[[nodiscard]] inline float bandwidth_kbps(std::uint32_t bytes, float seconds) noexcept {
    if (!(seconds > 0.f) || !std::isfinite(seconds)) {
        return 0.f;
    }
    return (static_cast<float>(bytes) / seconds) / 1000.f;
}

} // namespace fuse::net
