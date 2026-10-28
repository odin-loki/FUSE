// FUSE Relight: light-set count gate. Header-only.
// light_count_ok is true for a count in the inclusive range [0, 256].
#pragma once

namespace fuse::relight {

/// True when `count` is in the inclusive range [0, 256].
[[nodiscard]] inline bool light_count_ok(int count) {
    return count >= 0 && count <= 256;
}

} // namespace fuse::relight
