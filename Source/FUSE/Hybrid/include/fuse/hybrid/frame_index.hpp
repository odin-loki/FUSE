#pragma once

#include <cstdint>

namespace fuse::hybrid {

/// Ring slot of `frame`. A zero `ring` returns 0. Otherwise the low 32 bits of
/// `frame` modulo `ring`. The modulo saturates at `UINT32_MAX`.
[[nodiscard]] inline std::uint32_t hybrid_frame_index(std::uint64_t frame, std::uint32_t ring) noexcept {
    if (ring == 0u) {
        return 0u;
    }
    const std::uint64_t low = static_cast<std::uint32_t>(frame);
    const std::uint64_t mod = low % static_cast<std::uint64_t>(ring);
    constexpr std::uint64_t kMax = 0xffffffffull;
    if (mod > kMax) {
        return static_cast<std::uint32_t>(kMax);
    }
    return static_cast<std::uint32_t>(mod);
}

} // namespace fuse::hybrid
