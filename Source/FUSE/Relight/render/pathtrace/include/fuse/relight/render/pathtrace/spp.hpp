// Samples per pixel: a count is valid only when it is a power of two in [1, 64].
#pragma once

namespace fuse::relight {

/// True when `samples` is a power of two and lies in [1, 64].
[[nodiscard]] inline bool spp_ok(int samples) {
    return samples >= 1 && samples <= 64 && (samples & (samples - 1)) == 0;
}

} // namespace fuse::relight
