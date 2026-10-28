#pragma once

namespace fuse::ecs {

/// True when `capacity` is in the inclusive range 1..1000000.
[[nodiscard]] inline constexpr bool registry_cap_ok(int capacity) {
    return capacity >= 1 && capacity <= 1000000;
}

} // namespace fuse::ecs
