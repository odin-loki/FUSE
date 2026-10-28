#pragma once

namespace fuse::jobs {

/// True when `lane` is in the closed range [0, 7].
[[nodiscard]] inline bool cuda_lane_ok(int lane) {
    return lane >= 0 && lane <= 7;
}

} // namespace fuse::jobs
