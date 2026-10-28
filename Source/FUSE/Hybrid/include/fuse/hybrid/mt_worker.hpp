#pragma once

namespace fuse::hybrid {

/// True only for worker counts 1 through 32 inclusive.
[[nodiscard]] inline bool mt_worker_ok(int workers) {
    return workers >= 1 && workers <= 32;
}

} // namespace fuse::hybrid
