#pragma once

#include <cstdint>

namespace fuse::mechanics {

/// True when layer is in the inclusive range 0..31.
[[nodiscard]] inline bool proxy_layer_ok(std::uint32_t layer) {
    return layer <= 31u;
}

} // namespace fuse::mechanics
