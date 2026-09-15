#include <fuse/net/rollback_window.hpp>

namespace fuse::net {

bool can_rewind_to_frame(u32 current_frame, u32 target_frame, u32 max_rollback_frames) {
    if (target_frame > current_frame) {
        return false;
    }

    const u32 distance = current_frame - target_frame;
    return distance <= max_rollback_frames;
}

u32 resimulate_frame_count(u32 from_frame, u32 to_frame) {
    if (to_frame <= from_frame) {
        return 0;
    }
    return to_frame - from_frame;
}

} // namespace fuse::net
