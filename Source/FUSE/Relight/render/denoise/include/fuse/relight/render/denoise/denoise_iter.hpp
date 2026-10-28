#pragma once

namespace fuse::relight {

/// True when `iterations` is in the inclusive range 1..8.
[[nodiscard]] inline bool denoise_iter_ok(int iterations) {
    return iterations >= 1 && iterations <= 8;
}

} // namespace fuse::relight
