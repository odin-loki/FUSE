#pragma once

// Ore: Verve editor scrub preview without consume-once cue ledger

#include <fuse/cinematics/cue_payload.hpp>
#include <fuse/cinematics/timeline.hpp>
#include <fuse/cinematics/types.hpp>

#include <string>
#include <vector>

namespace fuse::cinematics {

struct CuePreviewEntry {
    std::string label;
    std::string track_label;
    std::string group_label;
    TrackKind track_kind = TrackKind::Generic;
    TimelineMs trigger_ms = 0;
    CuePayload payload;
};

/// Preview cues that would fire on a forward scrub to `time_ms` (no consume-once side effects).
[[nodiscard]] std::vector<CuePreviewEntry> preview_cues_at(const Timeline& timeline, TimelineMs time_ms);

} // namespace fuse::cinematics
