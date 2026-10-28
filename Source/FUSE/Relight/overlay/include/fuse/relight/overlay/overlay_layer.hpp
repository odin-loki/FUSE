#pragma once

namespace fuse::relight {

/// True when \p layer is in the inclusive range 0..7.
[[nodiscard]] inline bool overlay_layer_ok(int layer) {
    return layer >= 0 && layer <= 7;
}

} // namespace fuse::relight
