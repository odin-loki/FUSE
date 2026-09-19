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
///   actor <actor_id> mount <time_ms> <mount_point>
///   actor <actor_id> unmount <time_ms>
bool load_timeline_from_asset(const std::string& text, Timeline& outTimeline, std::string* errorOut = nullptr);

} // namespace fuse::cinematics
