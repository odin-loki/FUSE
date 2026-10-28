#pragma once

namespace fuse::renderer {

/// True only when `samples` is a power of two in the closed interval [1, 64].
/// Zero is false: a power-of-two test must require n > 0.
[[nodiscard]] inline bool pt_sample_ok(int samples) {
    if (samples <= 0 || samples > 64) {
        return false;
    }
    return (samples & (samples - 1)) == 0;
}

} // namespace fuse::renderer
