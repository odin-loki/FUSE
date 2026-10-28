#pragma once

namespace fuse::editor {

[[nodiscard]] inline bool phase6_step_ok(int step) {
    return step >= 0 && step <= 8;
}

} // namespace fuse::editor
