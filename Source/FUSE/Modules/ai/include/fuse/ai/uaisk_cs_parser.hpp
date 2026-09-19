#pragma once

#include <fuse/ai/uaisk_template_hooks.hpp>
#include <fuse/types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace fuse::ai::uaisk {

/// Parsed metadata from a UAISK `.cs` module stub (beyond static hook table mapping).
struct UaiskCsParseResult {
    std::string moduleName;
    std::string className;
    std::string baseClass;
    std::string fuseRegistryTypeId;
    std::vector<std::string> behaviorTreeHooks;
    std::vector<std::string> methodNames;
    std::vector<std::string> fieldNames;
    u32 profileId = 0;
    bool valid = false;
};

/// Parse UAISK `.cs` text for class name, behavior-tree hook refs, and registry type mapping.
[[nodiscard]] bool parseCsModule(std::string_view csModule, std::string_view csText, UaiskCsParseResult& outResult);

/// Resolve a fuse_ai tree profile id from parsed `.cs` metadata (falls back to hook table).
[[nodiscard]] u32 treeProfileForParsedModule(const UaiskCsParseResult& parsed);

/// Map a fuse registry type id to a stable tree profile slot.
[[nodiscard]] u32 profileIdForRegistryType(std::string_view fuseRegistryTypeId);

} // namespace fuse::ai::uaisk
