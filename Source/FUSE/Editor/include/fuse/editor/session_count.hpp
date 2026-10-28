#pragma once

namespace fuse::editor {

[[nodiscard]] inline bool session_count_ok(int count) {
    return count >= 1 && count <= 8;
}

} // namespace fuse::editor
