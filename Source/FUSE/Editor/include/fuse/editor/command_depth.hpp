#pragma once

namespace fuse::editor {

[[nodiscard]] inline bool command_depth_ok(int depth) {
    return depth >= 1 && depth <= 512;
}

} // namespace fuse::editor
