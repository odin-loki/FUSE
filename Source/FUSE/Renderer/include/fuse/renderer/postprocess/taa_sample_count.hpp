#pragma once

namespace fuse::renderer {

/// True only when `samples` is a power of two in the closed interval [1, 16].
[[nodiscard]] inline bool taa_sample_count_ok(int samples) {
    if (samples < 1 || samples > 16) {
        return false;
    }
    return (samples & (samples - 1)) == 0;
}

} // namespace fuse::renderer
