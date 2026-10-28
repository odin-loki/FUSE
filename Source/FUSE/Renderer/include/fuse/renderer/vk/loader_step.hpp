#pragma once

// CPU check for volk loader init steps. Steps 0 through 3 are accepted.

namespace fuse::renderer {

[[nodiscard]] inline bool loader_step_ok(int step) {
    return step >= 0 && step <= 3;
}

} // namespace fuse::renderer
