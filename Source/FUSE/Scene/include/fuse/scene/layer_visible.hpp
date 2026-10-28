#pragma once

#include <cstdint>

namespace fuse::scene {

/// True when bit `layer` of `mask` is set. `layer` outside 0..31 is false.
[[nodiscard]] inline bool scene_layer_visible(std::uint32_t mask, std::uint32_t layer) {
    if (layer > 31u) {
        return false;
    }
    return (mask & (std::uint32_t{1} << layer)) != 0u;
}

} // namespace fuse::scene
