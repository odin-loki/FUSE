#pragma once

namespace fuse::project {

/// True for cook worker counts 1 through 16 inclusive.
[[nodiscard]] inline bool cook_worker_ok(int workers) {
    return workers >= 1 && workers <= 16;
}

} // namespace fuse::project
