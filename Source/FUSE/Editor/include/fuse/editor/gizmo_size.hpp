#pragma once

namespace fuse::editor {

[[nodiscard]] inline bool gizmo_size_ok(int px) {
    return px >= 8 && px <= 64;
}

} // namespace fuse::editor
