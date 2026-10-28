#pragma once

namespace fuse::renderer {

/// True only when `n` is a power of two in [16, 256].
/// Zero is false: a power-of-two test must require n > 0 (0 & (0 - 1) is 0).
[[nodiscard]] inline bool ltc_lut_ok(int n) {
    return n > 0 && (n & (n - 1)) == 0 && n >= 16 && n <= 256;
}

} // namespace fuse::renderer
