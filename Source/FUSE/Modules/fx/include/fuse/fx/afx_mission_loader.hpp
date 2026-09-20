#pragma once

// Ore: TorqueScript .mis mission files (AFXDemo_Minimal.mis) — function hook scan stub

#include <fuse/fx/afx_mission_hooks.hpp>

#include <string>
#include <vector>

namespace fuse::fx {

struct AfxMissionScheduleEntry {
    std::string hookName;
    u32 delayMs = 0;
};

struct AfxMissionBody {
    std::string missionName;
    std::vector<std::string> simObjectNames;
    std::vector<std::string> missionInfoKeys;
    std::vector<std::pair<std::string, std::string>> simObjectBodies;
    std::vector<std::pair<std::string, std::string>> nestedSimObjectBodies;
    std::vector<AfxMissionScheduleEntry> scheduleEntries;
    std::vector<std::string> callTargets;
};

struct AfxMissionBodyEffect {
    std::string simObjectName;
    std::string effectId;
};

struct AfxMissionBodySpell {
    std::string simObjectName;
    std::string spellId;
};

struct AfxMissionFunctionBody {
    std::string functionName;
    std::string bodyText;
};

/// Parse TorqueScript `.mis` body blocks (missionInfo, SimObject declarations, nested SimObjects).
[[nodiscard]] bool parse_afx_mission_body_from_mis(const std::string& misText, AfxMissionBody& outBody,
                                                   std::string* errorOut = nullptr);

/// Codegen effect ids from parsed SimObject bodies (SparkEmitter → spark_burst, etc.).
[[nodiscard]] u32 codegen_effects_from_mission_body(const AfxMissionBody& body,
                                                    std::vector<AfxMissionBodyEffect>& outEffects);

/// Codegen spell ids from parsed SimObject bodies (spellId= fields in nested bodies).
[[nodiscard]] u32 codegen_spells_from_mission_body(const AfxMissionBody& body,
                                                   std::vector<AfxMissionBodySpell>& outSpells);

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

/// Parse TorqueScript function bodies from `.mis` text (mission VM execution ore).
[[nodiscard]] u32 parse_afx_mission_functions_from_mis(const std::string& misText,
                                                       std::vector<AfxMissionFunctionBody>& outFunctions);

/// Execute parsed mission function bodies through the VM (TorqueScript execution stub).
[[nodiscard]] bool execute_afx_mission_from_mis(const std::string& misText, FxComposer& composer,
                                                AfxMissionScriptVm& vm, std::string* errorOut = nullptr);

} // namespace fuse::fx
