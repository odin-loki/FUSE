#pragma once

namespace fuse::renderer {

/// True when `samples` is a power of two in the closed interval [8, 128].
[[nodiscard]] inline bool vol_sample_ok(int samples) {
    if (samples < 8 || samples > 128) {
        return false;
    }
    return (samples & (samples - 1)) == 0;
}

} // namespace fuse::renderer
