#pragma once

namespace fuse::editor {

inline bool drop_target_ok(int dragged, int target, int count) {
    if (count <= 0) {
        return false;
    }
    if (dragged < 0 || target < 0 || dragged >= count || target >= count) {
        return false;
    }
    return dragged != target;
}

} // namespace fuse::editor
