#pragma once

#include <fuse/cinematics/timeline.hpp>

#include <string>

namespace fuse::cinematics {

/// Load a timeline from a line-based `.seq` asset (no JSON dependency).
///
/// Format:
///   duration_ms=N
///   sprite <target_id> <t0>,<x>,<y>,<a> <t1>,<x>,<y>,<a> ...
///   camera <t0>,<px>,<py>,<pz>,<fov> <t1>,...
///   actor <actor_id> mount <time_ms> <mount_point> [<yaw_deg>]
///   actor <actor_id> unmount <time_ms>
///   motion <path_id> <t0>,<x>,<y>,<z> <t1>,...
bool load_timeline_from_asset(const std::string& text, Timeline& outTimeline, std::string* errorOut = nullptr);

struct SeqScrubPreview {
    TimelineMs time_ms = 0;
    bool valid = false;
    bool has_actor_events = false;
    bool has_motion_track = false;
    bool has_camera_track = false;
    bool has_sprite_track = false;
    std::string actor_id;
    std::string mount_point;
    float sprite_x = 0.f;
    float sprite_y = 0.f;
    float camera_fov = 0.f;
    float mount_yaw_deg = 0.f;
    float mount_pitch_deg = 0.f;
    float mount_roll_deg = 0.f;
};

/// Editor scrub stub — load `.seq` text and seek playhead without consuming cues.
[[nodiscard]] bool scrub_seq_preview(const std::string& text, TimelineMs time_ms, Timeline& outTimeline,
                                     SeqScrubPreview& outPreview, std::string* errorOut = nullptr);

} // namespace fuse::cinematics
