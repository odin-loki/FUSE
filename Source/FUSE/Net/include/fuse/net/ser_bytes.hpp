#pragma once

namespace fuse::net {

/// True for byte counts in the closed range 1..1500.
[[nodiscard]] inline bool ser_bytes_ok(int bytes) noexcept {
    return bytes >= 1 && bytes <= 1500;
}

} // namespace fuse::net
