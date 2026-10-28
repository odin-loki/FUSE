#pragma once

namespace fuse::renderer {

/// G-buffer slots 0..4: albedo, normal, material, depth, motion.
[[nodiscard]] inline bool gbuffer_slot_ok(int slot) {
    return slot >= 0 && slot <= 4;
}

} // namespace fuse::renderer
