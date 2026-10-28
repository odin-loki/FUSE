#pragma once

namespace fuse::ecs {

/// True only for bit counts 0 through 32 inclusive.
[[nodiscard]] inline constexpr bool filter_bits_ok(int bits) {
    return bits >= 0 && bits <= 32;
}

} // namespace fuse::ecs
