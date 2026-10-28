#pragma once

namespace fuse::renderer {

/// True only for queues 0 through 7 inclusive.
[[nodiscard]] inline bool rg_queue_ok(int queue) {
    return queue >= 0 && queue <= 7;
}

} // namespace fuse::renderer
