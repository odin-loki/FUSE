#pragma once

namespace fuse::relight::options {

/// True when `layer` is in the inclusive range 0..7.
[[nodiscard]] inline bool config_layer_ok(int layer) {
    return layer >= 0 && layer <= 7;
}

} // namespace fuse::relight::options
