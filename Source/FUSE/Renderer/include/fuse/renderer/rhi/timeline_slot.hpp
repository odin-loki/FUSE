#pragma once

// CPU check for timeline semaphore slots. Valid range is 0..15.

namespace fuse::renderer {

[[nodiscard]] inline bool timeline_slot_ok(int slot) {
    return slot >= 0 && slot <= 15;
}

} // namespace fuse::renderer
