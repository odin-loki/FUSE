#pragma once

namespace fuse::ecs {

/// True when `cap` is in the inclusive range 1..4096.
[[nodiscard]] inline constexpr bool archetype_cap_ok(int cap) {
    return cap >= 1 && cap <= 4096;
}

} // namespace fuse::ecs
