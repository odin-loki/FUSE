// FUSE Relight: supported hash widths.
//
// hash_bytes_ok() is true only for 4, 8, 16, and 32 byte widths.
#pragma once

namespace fuse::relight::hash {

/// True when \p bytes is a supported hash width: 4, 8, 16, or 32.
inline bool hash_bytes_ok(int bytes) {
    return bytes == 4 || bytes == 8 || bytes == 16 || bytes == 32;
}

} // namespace fuse::relight::hash
