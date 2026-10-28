#pragma once

namespace fuse::platform {

[[nodiscard]] inline bool worker_count_ok(int count) {
    return count >= 1 && count <= 64;
}

} // namespace fuse::platform
