#pragma once

namespace fuse::net {

/// True when `bytes` is a multiple of 4 in the closed range 4..64.
[[nodiscard]] inline bool checksum_bytes_ok(int bytes) noexcept {
    return bytes >= 4 && bytes <= 64 && (bytes % 4) == 0;
}

} // namespace fuse::net
