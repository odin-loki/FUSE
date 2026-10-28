#pragma once

#include <cstdint>

namespace fuse::ecs {

/// True when `count` is in the inclusive range 1..65536.
[[nodiscard]] inline constexpr bool device_column_ok(std::uint32_t count) {
    return count >= 1u && count <= 65536u;
}

} // namespace fuse::ecs
