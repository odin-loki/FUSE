#pragma once

namespace fuse::hybrid {

/// True only for layers 0 through 7 inclusive.
[[nodiscard]] inline bool compose_layer_ok(int layer) {
    return layer >= 0 && layer <= 7;
}

} // namespace fuse::hybrid
