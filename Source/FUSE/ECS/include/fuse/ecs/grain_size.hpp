#pragma once

namespace fuse::ecs {

/// True when `grain` is in the inclusive range 1..4096.
[[nodiscard]] inline constexpr bool grain_size_ok(int grain) {
    return grain >= 1 && grain <= 4096;
}

} // namespace fuse::ecs
