#pragma once

namespace fuse::physics {

/// True when stage is in [0, 7].
[[nodiscard]] inline bool pipeline_stage_ok(int stage) {
    return stage >= 0 && stage <= 7;
}

} // namespace fuse::physics
