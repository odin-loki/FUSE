#pragma once

namespace fuse::compute {

/// True when `extent` is a power of two in the closed interval [8, 64].
/// Zero is false.
[[nodiscard]] inline bool cull_tile_ok(int extent) {
    return extent >= 8 && extent <= 64 && (extent & (extent - 1)) == 0;
}

} // namespace fuse::compute
