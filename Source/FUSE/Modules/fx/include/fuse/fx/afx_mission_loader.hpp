#pragma once

// Ore: TorqueScript .mis mission files (AFXDemo_Minimal.mis) — function hook scan stub

#include <fuse/fx/afx_mission_hooks.hpp>

#include <string>
#include <vector>

namespace fuse::fx {

/// Scan TorqueScript `.mis` text for AFX mission hook function declarations.
[[nodiscard]] bool load_afx_mission_hooks_from_mis(const std::string& misText,
                                                 std::vector<AfxMissionHook>& outHooks,
                                                 std::string* errorOut = nullptr);

} // namespace fuse::fx
