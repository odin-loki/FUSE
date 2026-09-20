#pragma once

// Qt viewport seq preview overlay stub (U5 wave 18) — headless-friendly scrub sample for runtime viewport.

#include <fuse/cinematics/types.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::editor {

struct ViewportSeqPreviewSample {
    fuse::cinematics::TimelineMs time_ms = 0;
    bool valid = false;
    std::string mount_point;
    std::string bone_name;
    float mount_yaw_deg = 0.f;
    float mount_pitch_deg = 0.f;
    float mount_roll_deg = 0.f;
    float sprite_x = 0.f;
    float sprite_y = 0.f;
};

/// Build a viewport overlay sample from loaded `.seq` asset text at a scrub time.
[[nodiscard]] ViewportSeqPreviewSample sample_viewport_seq_preview(const std::string& seqAssetText,
                                                                   fuse::cinematics::TimelineMs timeMs);

} // namespace fuse::editor
