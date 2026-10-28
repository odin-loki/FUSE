#pragma once

namespace fuse::renderer {

/// True when `index` is in the inclusive range 0..7.
[[nodiscard]] inline bool composite_layer_ok(int index) {
    return index >= 0 && index <= 7;
}

} // namespace fuse::renderer
