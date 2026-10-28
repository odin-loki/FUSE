#pragma once

namespace fuse::renderer {

/// True only for layer counts 1 through 8 inclusive.
[[nodiscard]] inline bool mat_layer_ok(int layers) {
    return layers >= 1 && layers <= 8;
}

} // namespace fuse::renderer
