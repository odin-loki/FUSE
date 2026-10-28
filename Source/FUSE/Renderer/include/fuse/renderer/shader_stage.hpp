#pragma once

namespace fuse::renderer {

/// True only for stages 0 through 5 inclusive.
[[nodiscard]] inline bool shader_stage_ok(int stage) {
    return stage >= 0 && stage <= 5;
}

} // namespace fuse::renderer
