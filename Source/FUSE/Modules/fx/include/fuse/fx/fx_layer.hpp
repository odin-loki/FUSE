#pragma once

namespace fuse::fx {

/// True only for layers 0 through 7 inclusive.
[[nodiscard]] inline bool fx_layer_ok(int layer) {
    return layer >= 0 && layer <= 7;
}

} // namespace fuse::fx
