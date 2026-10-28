#pragma once

namespace fuse::jobs {

/// True only when `batch` is in the closed range [1, 1024].
[[nodiscard]] inline bool job_batch_ok(int batch) {
    return batch >= 1 && batch <= 1024;
}

} // namespace fuse::jobs
