#pragma once

namespace fuse::ecs {

/// True only for byte counts 1 through (1<<20) inclusive.
[[nodiscard]] inline constexpr bool serial_bytes_ok(int bytes) {
    return bytes >= 1 && bytes <= (1 << 20);
}

} // namespace fuse::ecs
