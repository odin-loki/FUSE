#pragma once

namespace fuse::relight {

/// True when `bytes` is a multiple of 4 in the closed range 4..256.
[[nodiscard]] inline bool vertex_stride_ok(int bytes) {
    return bytes >= 4 && bytes <= 256 && (bytes % 4) == 0;
}

} // namespace fuse::relight
