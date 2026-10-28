#pragma once

namespace fuse::jobs {

/// True when `priority` is in the closed range [0, 3].
[[nodiscard]] inline bool job_priority_ok(int priority) {
    return priority >= 0 && priority <= 3;
}

} // namespace fuse::jobs
