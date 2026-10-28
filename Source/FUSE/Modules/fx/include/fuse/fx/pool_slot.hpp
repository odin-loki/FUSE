#pragma once

#include <cstdint>

namespace fuse::fx {

/// True when `capacity` is positive and `index` is inside that range.
[[nodiscard]] inline bool pool_slot_ok(std::uint32_t index, std::uint32_t capacity) {
    return capacity > 0u && index < capacity;
}

} // namespace fuse::fx
