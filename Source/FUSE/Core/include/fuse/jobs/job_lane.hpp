#pragma once

namespace fuse::jobs {

/// True when `lane` is in the closed range [0, 31].
[[nodiscard]] inline bool job_lane_ok(int lane) {
    return lane >= 0 && lane <= 31;
}

} // namespace fuse::jobs
