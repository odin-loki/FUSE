// D3D9 slot index: valid when it is in 0..3.
#pragma once

namespace fuse::relight {

/// True when \p slot is in 0..3.
[[nodiscard]] inline bool d3d9_slot_ok(int slot) {
    return slot >= 0 && slot <= 3;
}

} // namespace fuse::relight
