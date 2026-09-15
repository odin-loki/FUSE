#pragma once

#include <fuse/types.hpp>

namespace fuse::net {

/// Returns false when `target_frame` is in the future or outside the rollback window.
[[nodiscard]] bool can_rewind_to_frame(u32 current_frame, u32 target_frame, u32 max_rollback_frames);

/// Number of simulation steps needed to advance from `from_frame` to `to_frame` (0 when `to_frame <= from_frame`).
[[nodiscard]] u32 resimulate_frame_count(u32 from_frame, u32 to_frame);

} // namespace fuse::net
