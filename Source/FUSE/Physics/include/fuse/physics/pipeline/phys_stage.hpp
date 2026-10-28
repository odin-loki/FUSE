#pragma once

namespace fuse::physics {

/// True when a physics stage is in the inclusive range 0..7.
[[nodiscard]] inline bool phys_stage_ok(int stage) {
    return stage >= 0 && stage <= 7;
}

} // namespace fuse::physics
