#pragma once

namespace fuse::editor {

/// True when `slot` is in [0, 15].
[[nodiscard]] inline bool material_slot_ok(int slot) {
    return slot >= 0 && slot <= 15;
}

} // namespace fuse::editor
