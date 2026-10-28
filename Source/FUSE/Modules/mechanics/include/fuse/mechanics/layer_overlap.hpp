#pragma once

#include <cstdint>

namespace fuse::mechanics {

/// True when the two layer masks share any set bit.
[[nodiscard]] inline bool layers_overlap(std::uint32_t a, std::uint32_t b) {
    return (a & b) != 0;
}

} // namespace fuse::mechanics
