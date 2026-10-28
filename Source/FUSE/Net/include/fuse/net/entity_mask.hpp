#pragma once

#include <cstdint>

namespace fuse::net {

/// True only when `entityIndex` is in `0..63` (fits a 64-bit entity mask).
[[nodiscard]] inline bool entity_in_mask64(std::uint32_t entityIndex) noexcept {
    return entityIndex < 64u;
}

} // namespace fuse::net
