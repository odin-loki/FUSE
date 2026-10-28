#pragma once

#include <cstdint>

namespace fuse::log {

/// True when `slots` is a power of two in [16, 65536].
[[nodiscard]] inline bool ring_slots_ok(std::uint32_t slots) {
    const bool power_of_two = slots != 0u && (slots & (slots - 1u)) == 0u;
    return power_of_two && slots >= 16u && slots <= 65536u;
}

} // namespace fuse::log
