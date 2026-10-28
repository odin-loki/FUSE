#pragma once

namespace fuse::editor {

/// True when `slot` is in the inclusive range 0..7.
[[nodiscard]] inline bool transport_slot_ok(int slot) {
    return slot >= 0 && slot <= 7;
}

} // namespace fuse::editor
