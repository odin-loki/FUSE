#pragma once

namespace fuse::jobs {

/// True only when `depth` is in the closed range [0, 1024].
[[nodiscard]] inline bool job_queue_ok(int depth) {
    return depth >= 0 && depth <= 1024;
}

} // namespace fuse::jobs
