#include <fuse/ai/uaisk_script_import.hpp>

#include <fuse/ai/tree_loader.hpp>
#include <fuse/ai/uaisk_cs_parser.hpp>

namespace fuse::ai::uaisk {

u32 treeProfileForModule(std::string_view csModule) {
    return treeProfileForModuleText(csModule, {});
}

u32 treeProfileForModuleText(std::string_view csModule, std::string_view csText) {
    UaiskCsParseResult parsed;
    if (!parseCsModule(csModule, csText, parsed)) {
        return 0;
    }
    return treeProfileForParsedModule(parsed);
}

bool importTemplateAsset(const std::string& btText, u32 profileId, BehaviorRuntime& runtime, std::string* errorOut) {
    BehaviorTree tree;
    if (!loadTreeFromText(btText, tree, errorOut)) {
        return false;
    }
    runtime.registerTreeProfile(profileId, tree);
    return true;
}

bool registerPatrolSquadProfile(BehaviorRuntime& runtime, std::string* errorOut) {
    static const char* kPatrolSquadBt =
        "# UAISK patrol_squad template\n"
        "bb.condition.allies_in_radius threshold=8 loops=1\n"
        "bb.action.set_flag flag=1\n"
        "bb.condition.distance_less threshold=5\n"
        "bb.action.set_flag flag=0\n"
        "bb.sequence children=0,1\n"
        "bb.sequence children=2,3\n"
        "bb.selector children=4,5 hook=aiBehaviors.cs\n"
        "root=6\n";

    return importTemplateAsset(kPatrolSquadBt, 1, runtime, errorOut);
}

} // namespace fuse::ai::uaisk
