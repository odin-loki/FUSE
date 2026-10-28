#pragma once

#include <cstdint>

namespace fuse::hybrid {

/// True when `slot` is in the inclusive range 0..31.
[[nodiscard]] inline bool binding_slot_ok(int slot) {
    return slot >= 0 && slot <= 31;
}

/// True when `count` is positive and `slot` is inside that range.
[[nodiscard]] inline bool binding_slot_ok(std::uint32_t slot, std::uint32_t count) {
    return count > 0u && slot < count;
}

} // namespace fuse::hybrid
