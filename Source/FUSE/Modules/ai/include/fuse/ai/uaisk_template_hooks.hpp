#pragma once

#include <cstddef>
#include <string_view>

namespace fuse::ai::uaisk {

/// UAISK is scripts-only ore — these hooks map TorqueScript behavior modules to fuse_ai
/// registry type ids for future script-host import. No compile-time dependency on UAISK.
///
/// Ore: third_party/addons/UAISK/The_Universal_AI_Starter_Kit/Templates/Full/game/scripts/server/UAISK/
struct TemplateHook {
    std::string_view uaiskModule;
    std::string_view fuseRegistryTypeId;
    std::string_view notes;
};

inline constexpr TemplateHook kTemplateHooks[] = {
    {"aiBehaviors.cs", "bb.selector", "LeashedBehavior / FollowPlayer → composite selector"},
    {"aiMovement.cs", "gb.action.move_toward", "Patrol / chase movement → move-toward leaf"},
    {"aiActions.cs", "bb.action.set_flag", "Action executes → blackboard flag commit"},
    {"aiTargeting.cs", "bb.condition.distance_less", "Range checks → distance condition"},
};

inline constexpr std::size_t templateHookCount = sizeof(kTemplateHooks) / sizeof(TemplateHook);

} // namespace fuse::ai::uaisk
