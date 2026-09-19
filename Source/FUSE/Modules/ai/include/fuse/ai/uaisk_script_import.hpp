#pragma once

#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/ai/behavior_tree.hpp>
#include <fuse/ai/uaisk_template_hooks.hpp>

#include <string>
#include <string_view>

namespace fuse::ai::uaisk {

/// Maps a UAISK `.cs` module name to a fuse_ai tree profile id (script-host import stub).
[[nodiscard]] u32 treeProfileForModule(std::string_view csModule);

/// Map a UAISK module + optional `.cs` text to a tree profile id (richer parser path).
[[nodiscard]] u32 treeProfileForModuleText(std::string_view csModule, std::string_view csText);

/// Load a UAISK template `.bt` asset and register it under `profileId`.
bool importTemplateAsset(const std::string& btText, u32 profileId, BehaviorRuntime& runtime, std::string* errorOut = nullptr);

/// Register built-in UAISK patrol_squad profile (profile 1) from embedded template text.
bool registerPatrolSquadProfile(BehaviorRuntime& runtime, std::string* errorOut = nullptr);

} // namespace fuse::ai::uaisk
