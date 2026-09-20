#pragma once

// Ore: third_party/addons/AFX-Template/game/levels/*.mis mission script hooks

#include <fuse/fx/fx_composer.hpp>

#include <string>
#include <vector>

namespace fuse::fx {

struct AfxMissionHook {
    std::string missionId;
    std::string scriptHook;
    std::string spellId;
    u32 delayMs = 0;
};

/// Parse `mission` lines from AFX-Template pack text.
bool load_afx_mission_hooks_from_text(const std::string& text, std::vector<AfxMissionHook>& outHooks, std::string* errorOut = nullptr);

/// Register built-in AFXDemo_Minimal mission hooks (on_spell_cast → fireball).
bool registerAfxTemplateMissionHooks(FxComposer& composer, std::vector<AfxMissionHook>* outHooks = nullptr);

} // namespace fuse::fx
