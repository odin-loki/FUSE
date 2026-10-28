#pragma once

namespace fuse::relight {

/// True when `level` is in the inclusive range 0..4.
[[nodiscard]] inline constexpr bool omm_level_ok(int level) {
    return level >= 0 && level <= 4;
}

} // namespace fuse::relight
