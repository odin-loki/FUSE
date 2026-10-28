#pragma once

namespace fuse::jobs {

/// True only when `count` is exactly one worker.
[[nodiscard]] inline bool single_worker_ok(int count) {
    return count == 1;
}

} // namespace fuse::jobs
