// Volume slice count is valid for 1 through 64 inclusive.
#pragma once

namespace fuse::relight {

/// True only for slice counts 1 through 64 inclusive.
[[nodiscard]] inline bool vol_slice_ok(int slices) {
    return slices >= 1 && slices <= 64;
}

} // namespace fuse::relight
