#pragma once

namespace fuse::platform {

/// True when `level` is in the inclusive range 0..3.
[[nodiscard]] inline bool harden_level_ok(int level) {
    return level >= 0 && level <= 3;
}

} // namespace fuse::platform
