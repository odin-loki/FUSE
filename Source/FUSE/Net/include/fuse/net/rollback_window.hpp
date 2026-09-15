#pragma once

#include <fuse/types.hpp>

namespace fuse::net {

/// Earliest frame reachable from `current_frame` within `max_rollback_frames` (0 when the window spans frame 0).
[[nodiscard]] u32 earliest_rewindable_frame(u32 current_frame, u32 max_rollback_frames);

/// Returns false when `target_frame` is in the future or outside the rollback window.
[[nodiscard]] bool can_rewind_to_frame(u32 current_frame, u32 target_frame, u32 max_rollback_frames);

/// Clamps `target_frame` into `[earliest_rewindable_frame, current_frame]` (future targets map to `current_frame`).
[[nodiscard]] u32 clamp_rewind_target(u32 current_frame, u32 target_frame, u32 max_rollback_frames);

/// Number of simulation steps needed to advance from `from_frame` to `to_frame` (0 when `to_frame <= from_frame`).
[[nodiscard]] u32 resimulate_frame_count(u32 from_frame, u32 to_frame);

} // namespace fuse::net
