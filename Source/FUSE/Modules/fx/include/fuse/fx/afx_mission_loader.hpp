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

/// Deepen TorqueScript bridge — scan `%spellId = "..."` assignments and attach to hooks.
[[nodiscard]] bool apply_afx_mission_spell_assignments(const std::string& misText,
                                                       std::vector<AfxMissionHook>& hooks);

class AfxMissionScriptVm;
class FxComposer;

/// TorqueScript `.mis` bridge — scan hooks and register them on the mission VM + composer.
[[nodiscard]] bool register_afx_mission_from_mis(const std::string& misText, FxComposer& composer,
                                                 AfxMissionScriptVm& vm, std::string* errorOut = nullptr);

/// Deepen TorqueScript bridge — dispatch all hooks found in `.mis` text through the VM.
[[nodiscard]] bool dispatch_afx_mission_from_mis(const std::string& misText, FxComposer& composer,
                                               AfxMissionScriptVm& vm, std::string* errorOut = nullptr);

} // namespace fuse::fx
