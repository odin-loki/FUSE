#pragma once

namespace fuse::editor {

/// True when `id` is in the inclusive range 0..7.
[[nodiscard]] inline bool viewport_id_ok(int id) {
    return id >= 0 && id <= 7;
}

} // namespace fuse::editor
