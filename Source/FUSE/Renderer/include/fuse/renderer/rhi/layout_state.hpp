#pragma once

// CPU check for Vulkan image layout states. Valid states are 0..7
// (undefined through transfer destination).

namespace fuse::renderer {

[[nodiscard]] inline bool layout_state_ok(int state) {
    return state >= 0 && state <= 7;
}

} // namespace fuse::renderer
