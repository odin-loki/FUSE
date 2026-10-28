#pragma once

namespace fuse::compute {

/// True when `taps` is a power of two in the closed interval [4, 32].
[[nodiscard]] inline bool ss_kernel_ok(int taps) {
    return taps >= 4 && taps <= 32 && (taps & (taps - 1)) == 0;
}

} // namespace fuse::compute
